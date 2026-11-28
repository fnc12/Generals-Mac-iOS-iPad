using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using EndEditor.Core;
using Vortice.D3DCompiler;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace EndEditor.Render;

/// The one Direct3D 11 device of the app (the map view and the model preview share it), with
/// small helpers. Hardware first; WARP (software) where there is no GPU, e.g. a session nobody sees.
public sealed class Gpu
{
    private static Gpu? shared;
    public static Gpu Shared => shared ??= new Gpu();

    public readonly ID3D11Device Device;
    public readonly ID3D11DeviceContext Context;
    public readonly IDXGIFactory2 Factory;
    public readonly bool Software;
    public readonly string AdapterName;

    private Gpu()
    {
        var levels = new[] { FeatureLevel.Level_11_0, FeatureLevel.Level_10_1, FeatureLevel.Level_10_0 };
        var flags = DeviceCreationFlags.BgraSupport;
        var result = D3D11.D3D11CreateDevice((IDXGIAdapter?)null, DriverType.Hardware, flags, levels, out ID3D11Device device,
                                             out ID3D11DeviceContext context);
        if (result.Failure || device == null)
        {
            D3D11.D3D11CreateDevice((IDXGIAdapter?)null, DriverType.Warp, flags, levels, out device, out context).CheckError();
            Software = true;
        }
        Device = device;
        Context = context;
        using var dxgiDevice = Device.QueryInterface<IDXGIDevice>();
        using var adapter = dxgiDevice.GetAdapter();
        AdapterName = adapter.Description.Description;
        Factory = adapter.GetParent<IDXGIFactory2>();
    }

    public ID3D11Buffer? Buffer(byte[] data, BindFlags bind)
    {
        if (data.Length == 0) return null;
        return Device.CreateBuffer<byte>(data, bind);
    }

    public ID3D11Buffer Buffer<T>(ReadOnlySpan<T> data, BindFlags bind) where T : unmanaged => Device.CreateBuffer(data, bind);

    /// A constant buffer for one T (written with Write).
    public ID3D11Buffer ConstantBuffer<T>() where T : unmanaged =>
        Device.CreateBuffer(new BufferDescription((uint)((Unsafe.SizeOf<T>() + 15) / 16 * 16), BindFlags.ConstantBuffer,
                                                  ResourceUsage.Dynamic, CpuAccessFlags.Write));

    /// A vertex buffer for up to `bytes` bytes, rewritten with Write.
    public ID3D11Buffer DynamicVertexBuffer(int bytes) =>
        Device.CreateBuffer(new BufferDescription((uint)bytes, BindFlags.VertexBuffer, ResourceUsage.Dynamic, CpuAccessFlags.Write));

    public unsafe void Write<T>(ID3D11Buffer buffer, in T value) where T : unmanaged
    {
        var mapped = Context.Map(buffer, 0, MapMode.WriteDiscard);
        Unsafe.Write((void*)mapped.DataPointer, value);
        Context.Unmap(buffer, 0);
    }

    public unsafe void Write<T>(ID3D11Buffer buffer, ReadOnlySpan<T> values) where T : unmanaged
    {
        var mapped = Context.Map(buffer, 0, MapMode.WriteDiscard);
        values.CopyTo(new Span<T>((void*)mapped.DataPointer, values.Length));
        Context.Unmap(buffer, 0);
    }

    /// An RGBA8 texture (rows top to bottom), with mipmaps when asked.
    public unsafe ID3D11ShaderResourceView? Texture(TextureData image, bool mipmaps = true, Format format = Format.R8G8B8A8_UNorm)
    {
        if (image.Width <= 0 || image.Height <= 0 || image.Rgba.Length < image.Width * image.Height * 4) return null;
        var description = new Texture2DDescription
        {
            Width = (uint)image.Width,
            Height = (uint)image.Height,
            MipLevels = mipmaps ? 0u : 1u,
            ArraySize = 1,
            Format = format,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default,
            BindFlags = mipmaps ? BindFlags.ShaderResource | BindFlags.RenderTarget : BindFlags.ShaderResource,
            MiscFlags = mipmaps ? ResourceOptionFlags.GenerateMips : ResourceOptionFlags.None,
        };
        using var texture = Device.CreateTexture2D(description);
        fixed (byte* pixels = image.Rgba)
        {
            Context.UpdateSubresource(texture, 0, null, (nint)pixels, (uint)image.Width * 4, 0);
        }
        var view = Device.CreateShaderResourceView(texture);
        if (mipmaps) Context.GenerateMips(view);
        return view;
    }

    public static ReadOnlyMemory<byte> Compile(string source, string entry, string profile)
    {
        var result = Compiler.Compile(source, entry, "end-editor.hlsl", profile, out var blob, out var errors);
        if (result.Failure || blob == null)
        {
            var message = errors != null ? Marshal.PtrToStringAnsi(errors.BufferPointer) : result.ToString();
            throw new InvalidOperationException($"{entry}: {message}");
        }
        ReadOnlyMemory<byte> bytes = blob.AsBytes();
        blob.Dispose();
        errors?.Dispose();
        return bytes;
    }

    public ID3D11BlendState Blend(Vortice.Direct3D11.Blend source, Vortice.Direct3D11.Blend destination, bool enable = true)
    {
        var description = new BlendDescription();
        description.RenderTarget[0] = new RenderTargetBlendDescription
        {
            BlendEnable = enable,
            SourceBlend = source,
            DestinationBlend = destination,
            BlendOperation = BlendOperation.Add,
            // The target's alpha stays (the frame is opaque).
            SourceBlendAlpha = Vortice.Direct3D11.Blend.Zero,
            DestinationBlendAlpha = Vortice.Direct3D11.Blend.One,
            BlendOperationAlpha = BlendOperation.Add,
            RenderTargetWriteMask = ColorWriteEnable.All,
        };
        return Device.CreateBlendState(description);
    }

    public ID3D11SamplerState Sampler(TextureAddressMode address, Filter filter = Filter.MinMagMipLinear, float maxLod = float.MaxValue) =>
        Device.CreateSamplerState(new SamplerDescription
        {
            Filter = filter,
            AddressU = address,
            AddressV = address,
            AddressW = address,
            ComparisonFunc = ComparisonFunction.Never,
            MinLOD = 0,
            MaxLOD = maxLod,
            MaxAnisotropy = 1,
        });
}

/// A render target with its depth buffer: the swap chain's back buffer or an offscreen texture
/// (snapshots, the UI check's pixel reads).
public sealed class Target : IDisposable
{
    public readonly ID3D11RenderTargetView Color;
    public readonly ID3D11DepthStencilView Depth;
    public readonly ID3D11Texture2D ColorTexture;
    public readonly int Width, Height;

    public Target(ID3D11Texture2D color, int width, int height)
    {
        var device = Gpu.Shared.Device;
        ColorTexture = color;
        Width = width;
        Height = height;
        Color = device.CreateRenderTargetView(color);
        using var depth = device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)width, Height = (uint)height, MipLevels = 1, ArraySize = 1, Format = Format.D32_Float,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.DepthStencil,
        });
        Depth = device.CreateDepthStencilView(depth);
    }

    public static Target Offscreen(int width, int height) =>
        new(Gpu.Shared.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)width, Height = (uint)height, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default,
            BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
        }), width, height);

    public void Begin(Vortice.Mathematics.Color4 clear)
    {
        var context = Gpu.Shared.Context;
        context.OMSetRenderTargets(Color, Depth);
        context.ClearRenderTargetView(Color, clear);
        context.ClearDepthStencilView(Depth, DepthStencilClearFlags.Depth, 1, 0);
        context.RSSetViewport(new Vortice.Mathematics.Viewport(Width, Height));
    }

    /// The pixels as BGRA rows, top to bottom.
    public unsafe byte[] ReadPixels()
    {
        var gpu = Gpu.Shared;
        using var staging = gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)Width, Height = (uint)Height, MipLevels = 1, ArraySize = 1, Format = ColorTexture.Description.Format,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Staging, CPUAccessFlags = CpuAccessFlags.Read,
        });
        gpu.Context.CopyResource(staging, ColorTexture);
        var mapped = gpu.Context.Map(staging, 0, MapMode.Read);
        var pixels = new byte[Width * Height * 4];
        for (int y = 0; y < Height; ++y)
        {
            new ReadOnlySpan<byte>((byte*)mapped.DataPointer + y * mapped.RowPitch, Width * 4).CopyTo(pixels.AsSpan(y * Width * 4));
        }
        gpu.Context.Unmap(staging, 0);
        return pixels;
    }

    public void Dispose()
    {
        Color.Dispose();
        Depth.Dispose();
        ColorTexture.Dispose();
    }
}
