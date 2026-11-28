using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices;
using EndEditor.Core;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace EndEditor.Render;

/// Playback state shared by the controls and the renderer.
public sealed class PreviewPlayback
{
    public bool Playing = true;
    public bool ShowHidden;
    public readonly Stopwatch Clock = Stopwatch.StartNew();
    public double StartTime;
    public int PausedFrame;
    public float Yaw = MathF.PI * 0.75f;
    public float Pitch = 0.45f;
    public float Zoom = 1;
    public Vector4 HouseColor = new(0.16f, 0.38f, 0.95f, 1);  // the preview's player color

    public double Now => Clock.Elapsed.TotalSeconds;

    public void Restart()
    {
        StartTime = Now;
        PausedFrame = 0;
    }

    public int Frame(int count, int rate)
    {
        if (count <= 1) return 0;
        if (!Playing) return Math.Min(PausedFrame, count - 1);
        return (int)((Now - StartTime) * Math.Max(rate, 1)) % count;
    }
}

/// Draws the preview's model: each vertex moved by its bone's matrix of the current frame (the
/// core's pose holds them for every frame).
public sealed class PreviewRenderer : IViewRenderer
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Uniforms
    {
        public Matrix4x4 ViewProjection;
        public Vector4 LightDirection, BaseColor;
        public uint FlagX, FlagY, FlagZ, FlagW;
        public Vector4 HouseColor;
        public uint FirstBone, Pad0, Pad1, Pad2;
    }

    private sealed record GpuPart(ID3D11Buffer Vertices, ID3D11Buffer Indices, uint IndexCount, ID3D11ShaderResourceView? Texture,
                                  int Bone, bool Skinned, bool Hidden, int Blend, int HouseColor);

    private readonly Gpu gpu = Gpu.Shared;
    private readonly ID3D11VertexShader vertexShader;
    private readonly ID3D11PixelShader pixelShader;
    private readonly ID3D11InputLayout layout;
    private readonly ID3D11Buffer uniforms;
    private readonly ID3D11BlendState opaque, alpha, additive, multiply;
    private readonly ID3D11DepthStencilState depthWrite, depthRead;
    private readonly ID3D11RasterizerState rasterizer;
    private readonly ID3D11SamplerState sampler;
    public readonly PreviewPlayback Playback = new();
    private List<GpuPart> parts = [];
    private readonly List<ID3D11ShaderResourceView> textures = [];
    private ID3D11Buffer? pose;
    private ID3D11ShaderResourceView? poseView;
    private PreviewModel model = new();
    private string geometryKey = "";
    private long poseKey = -2;
    private (int Frame, float Yaw, float Pitch, float Zoom, bool Hidden, string Key, long Pose, Vector2 Size)? presented;

    public PreviewRenderer()
    {
        var code = Gpu.Compile(Shaders.Preview, "previewVertex", "vs_5_0");
        vertexShader = gpu.Device.CreateVertexShader(code.Span);
        pixelShader = gpu.Device.CreatePixelShader(Gpu.Compile(Shaders.Preview, "previewPixel", "ps_5_0").Span);
        layout = gpu.Device.CreateInputLayout(
        [
            new InputElementDescription("POSITION", 0, Format.R32G32B32_Float, 0, 0),
            new InputElementDescription("NORMAL", 0, Format.R32G32B32_Float, 12, 0),
            new InputElementDescription("TEXCOORD", 0, Format.R32G32_Float, 24, 0),
            new InputElementDescription("BLENDINDICES", 0, Format.R32_UInt, 32, 0),
        ], code.Span);
        uniforms = gpu.ConstantBuffer<Uniforms>();
        opaque = gpu.Blend(Blend.One, Blend.Zero, false);
        alpha = gpu.Blend(Blend.SourceAlpha, Blend.InverseSourceAlpha);
        additive = gpu.Blend(Blend.One, Blend.One);
        multiply = gpu.Blend(Blend.Zero, Blend.SourceColor);
        depthWrite = gpu.Device.CreateDepthStencilState(new DepthStencilDescription(true, DepthWriteMask.All, ComparisonFunction.Less));
        // See-through parts are drawn last and do not hide others.
        depthRead = gpu.Device.CreateDepthStencilState(new DepthStencilDescription(true, DepthWriteMask.Zero, ComparisonFunction.Less));
        rasterizer = gpu.Device.CreateRasterizerState(new RasterizerDescription(CullMode.None, FillMode.Solid) { DepthClipEnable = true });
        sampler = gpu.Sampler(TextureAddressMode.Wrap);
    }

    public PreviewModel Model => model;
    public int PartCount => parts.Count;

    public void Show(PreviewModel newModel)
    {
        if (newModel.GeometryKey != geometryKey)
        {
            geometryKey = newModel.GeometryKey;
            Upload(newModel);
        }
        if (newModel.Revision != poseKey)
        {
            poseKey = newModel.Revision;
            poseView?.Dispose();
            pose?.Dispose();
            poseView = null;
            pose = null;
            var count = newModel.Pose.Length / 64;
            if (count > 0)
            {
                pose = gpu.Device.CreateBuffer<byte>(newModel.Pose.AsSpan(), new BufferDescription((uint)newModel.Pose.Length, BindFlags.ShaderResource,
                                                                                   ResourceUsage.Immutable, CpuAccessFlags.None,
                                                                                   ResourceOptionFlags.BufferStructured, 64));
                poseView = gpu.Device.CreateShaderResourceView(pose, new ShaderResourceViewDescription
                {
                    Format = Format.Unknown, ViewDimension = ShaderResourceViewDimension.Buffer,
                    Buffer = new BufferShaderResourceView { FirstElement = 0, NumElements = (uint)count },
                });
            }
            if (newModel.Animation != model.Animation || newModel.Name != model.Name) Playback.Restart();
        }
        model = newModel;
    }

    private void Upload(PreviewModel source)
    {
        foreach (var part in parts)
        {
            part.Vertices.Dispose();
            part.Indices.Dispose();
        }
        foreach (var texture in textures) texture.Dispose();
        textures.Clear();
        var byName = new Dictionary<string, ID3D11ShaderResourceView>();
        foreach (var (name, image) in source.Textures)
        {
            if (gpu.Texture(image) is { } texture)
            {
                byName[name] = texture;
                textures.Add(texture);
            }
        }
        parts = [];
        foreach (var part in source.Parts)
        {
            if (part.Vertices.Length == 0 || part.Indices.Length < 4) continue;
            parts.Add(new GpuPart(gpu.Buffer(part.Vertices, BindFlags.VertexBuffer)!, gpu.Buffer(part.Indices, BindFlags.IndexBuffer)!,
                                  (uint)(part.Indices.Length / 4), byName.GetValueOrDefault(part.Texture), part.Bone, part.Skinned,
                                  part.Hidden, part.Blend, part.HouseColor));
        }
    }

    private (int, float, float, float, bool, string, long, Vector2) Key(Vector2 size) =>
        (Playback.Frame(model.FrameCount, model.FrameRate), Playback.Yaw, Playback.Pitch, Playback.Zoom, Playback.ShowHidden,
         geometryKey, poseKey, size);

    public bool NeedsFrame(Vector2 size) => presented != Key(size);

    public void Render(Target target, Vector2 size, float scale)
    {
        presented = Key(size);
        target.Begin(new Color4(0.16f, 0.17f, 0.19f, 1));
        Encode(size.X / MathF.Max(size.Y, 1), Playback.Frame(model.FrameCount, model.FrameRate));
    }

    private void Encode(float aspect, int requestedFrame)
    {
        if (parts.Count == 0 || poseView == null || model.BoneCount <= 0) return;
        var frame = Math.Clamp(requestedFrame, 0, model.FrameCount - 1);
        if ((frame + 1) * model.BoneCount * 64 > model.Pose.Length) return;
        var context = gpu.Context;
        context.RSSetState(rasterizer);
        context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        context.IASetInputLayout(layout);
        context.VSSetShader(vertexShader);
        context.PSSetShader(pixelShader);
        context.VSSetConstantBuffer(0, uniforms);
        context.PSSetConstantBuffer(0, uniforms);
        context.VSSetShaderResource(1, poseView);
        context.PSSetSampler(0, sampler);
        var u = new Uniforms
        {
            ViewProjection = ViewProjection(aspect), LightDirection = new Vector4(0.4f, -0.5f, 0.75f, 0),
            BaseColor = new Vector4(0.62f, 0.64f, 0.66f, 1), HouseColor = Playback.HouseColor, FirstBone = (uint)(frame * model.BoneCount),
        };
        // Solid parts first, then the see-through ones (glows, smoke) over them.
        foreach (var part in parts.Where(p => p.Blend <= 1).Concat(parts.Where(p => p.Blend > 1)))
        {
            if (part.Hidden && !Playback.ShowHidden) continue;
            if (!part.Skinned && IsHidden(part.Bone, frame)) continue;
            context.OMSetBlendState(part.Blend switch { 2 => alpha, 3 => additive, 4 => multiply, _ => opaque });
            context.OMSetDepthStencilState(part.Blend > 1 ? depthRead : depthWrite);
            u.FlagX = part.Texture == null ? 0u : 1u;
            u.FlagY = part.Blend is 1 or 2 ? 1u : 0u;
            u.FlagZ = part.Blend <= 2 ? 1u : 0u;
            u.FlagW = (uint)part.HouseColor;
            gpu.Write(uniforms, u);
            context.IASetVertexBuffer(0, part.Vertices, 36);
            context.IASetIndexBuffer(part.Indices, Format.R32_UInt, 0);
            context.PSSetShaderResource(0, part.Texture!);
            context.DrawIndexed(part.IndexCount, 0, 0);
        }
    }

    private bool IsHidden(int bone, int frame)
    {
        var index = frame * model.BoneCount + bone;
        return index < model.Visible.Length && model.Visible[index] == 0;
    }

    /// Orbit camera around the model's bind-pose bounds; the game's world has Z up.
    private Matrix4x4 ViewProjection(float aspect)
    {
        var center = (model.BoundsMin + model.BoundsMax) * 0.5f;
        var radius = MathF.Max((model.BoundsMax - model.BoundsMin).Length() * 0.5f, 0.01f);
        var distance = radius * 2.6f * Playback.Zoom;
        var eye = center + distance * new Vector3(MathF.Cos(Playback.Pitch) * MathF.Cos(Playback.Yaw),
                                                  MathF.Cos(Playback.Pitch) * MathF.Sin(Playback.Yaw), MathF.Sin(Playback.Pitch));
        var view = Matrix4x4.CreateLookAt(eye, center, Vector3.UnitZ);
        var projection = Matrix4x4.CreatePerspectiveFieldOfView(0.75f, aspect, MathF.Max(distance - radius * 3, distance * 0.01f),
                                                                distance + radius * 3);
        return view * projection;
    }

    /// The current frame's pixels (BGRA rows) drawn offscreen: what the UI check looks at.
    public byte[] RenderPixels(int width, int height)
    {
        using var target = Target.Offscreen(width, height);
        target.Begin(new Color4(0.16f, 0.17f, 0.19f, 1));
        Encode(width / (float)height, Playback.Frame(model.FrameCount, model.FrameRate));
        return target.ReadPixels();
    }
}
