using System.Globalization;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using EndEditor.Core;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using Rect = System.Windows.Rect;
using Point = System.Windows.Point;
using Size = System.Windows.Size;

namespace EndEditor.Render;

/// What the renderer draws over the map besides the store's objects.
public sealed record MapOverlay(IReadOnlyList<long> Selection, MapGesture Gesture, Vector2? RoadStart, Vector2? Pending,
                                (Vector2 A, Vector2 B)? Band)
{
    public static readonly MapOverlay Empty = new([], MapGesture.None, null, null, null);

    public bool Same(MapOverlay other) =>
        Selection.SequenceEqual(other.Selection) && Gesture == other.Gesture && RoadStart == other.RoadStart &&
        Pending == other.Pending && Band == other.Band;
}

/// A start waypoint's label as the view last drew it, in pixels (origin top left): `Anchor` is the
/// waypoint's top, the label's pointer touches it.
public readonly record struct StartLabel(int Start, long Object, uint Color, Vector2 Anchor, Rect Frame);

/// Draws the open map from the core's map_* tables: terrain, water, models (instanced), markers,
/// links, selection boxes, the points waiting for input, start labels and the rubber band.
public sealed class MapRenderer : IViewRenderer
{
    public readonly MapCamera Camera = new();
    public bool ShowModels = true, ShowMarkers = true, ShowWater = true;

    [StructLayout(LayoutKind.Sequential)]
    private struct FrameUniforms
    {
        public Matrix4x4 ViewProjection;
        public Vector4 TerrainToLight, TerrainAmbient, TerrainDiffuse, ObjectToLight, ObjectAmbient, ObjectDiffuse;
        public Vector4 Playable, Water, Params;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PartUniforms
    {
        public uint FlagX, FlagY, FlagZ, FlagW;
        public Vector4 BaseColor;
        public Vector4 Rect;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Instance
    {
        public Matrix4x4 Transform;
        public Vector4 Color;
    }

    private sealed record GpuMesh(ID3D11Buffer Vertices, ID3D11Buffer Indices, uint IndexCount);
    private sealed record GpuPart(GpuMesh Mesh, ID3D11ShaderResourceView? Texture, int Blend, int HouseColor);
    private sealed record GpuModel(List<GpuPart> Parts, Vector3 BoundsMin, Vector3 BoundsMax);

    private readonly Gpu gpu = Gpu.Shared;
    private readonly ID3D11VertexShader terrainVs, modelVs, waterVs, lineVs, screenLineVs, labelVs;
    private readonly ID3D11PixelShader terrainPs, modelPs, waterPs, linePs, labelPs, fillPs;
    private readonly ID3D11InputLayout terrainLayout, modelLayout, pointLayout, screenPointLayout;
    private readonly ID3D11Buffer frameBuffer, partBuffer;
    private ID3D11Buffer lineBuffer;
    private int lineCapacity = 4096;
    private readonly ID3D11BlendState opaque, alpha, additive, multiply, premultiplied;
    private readonly ID3D11DepthStencilState depthWrite, depthRead, depthAlways;
    private readonly ID3D11RasterizerState rasterizer;
    private readonly ID3D11SamplerState sampler, terrainSampler, labelSampler;

    private string sceneKey = "";
    public MapScene Scene { get; private set; } = new();
    private ID3D11ShaderResourceView? atlas, waterTexture;
    private readonly List<GpuMesh> terrain = [], water = [];
    private readonly Dictionary<long, GpuModel> models = [];
    private readonly HashSet<long> failedModels = [];
    private readonly Dictionary<string, ID3D11ShaderResourceView> modelTextures = [];
    private readonly List<(long Model, ID3D11Buffer Instances, uint Count)> batches = [];
    private ID3D11Buffer? markerInstances;
    private uint markerCount;
    private readonly GpuMesh marker;
    private bool batchesDirty = true;
    private (bool Models, bool Markers) batchOptions = (true, true);
    private int contentGeneration;

    public Func<MapOverlay>? OverlaySource;
    private MapOverlay overlay = MapOverlay.Empty;
    private HashSet<long> selectionSet = [];

    private readonly Dictionary<(int Start, uint Color, float Scale), (ID3D11ShaderResourceView Texture, Vector2 Size)> labelTextures = [];
    /// The start labels of the last frame drawn in the view, what the UI check reads.
    public IReadOnlyList<StartLabel> StartLabels { get; private set; } = [];

    private readonly record struct FrameKey(int Generation, MapOverlay Overlay, bool ShowModels, bool ShowMarkers,
                                            bool ShowWater, Vector3 Target, float Yaw, float Pitch, float Distance, Vector2 Size);
    private FrameKey? presented;

    public MapRenderer()
    {
        var source = Shaders.Map;
        var terrainCode = Gpu.Compile(source, "terrainVertex", "vs_5_0");
        var modelCode = Gpu.Compile(source, "modelVertex", "vs_5_0");
        var waterCode = Gpu.Compile(source, "waterVertex", "vs_5_0");
        var lineCode = Gpu.Compile(source, "lineVertex", "vs_5_0");
        var screenLineCode = Gpu.Compile(source, "screenLineVertex", "vs_5_0");
        var device = gpu.Device;
        terrainVs = device.CreateVertexShader(terrainCode.Span);
        modelVs = device.CreateVertexShader(modelCode.Span);
        waterVs = device.CreateVertexShader(waterCode.Span);
        lineVs = device.CreateVertexShader(lineCode.Span);
        screenLineVs = device.CreateVertexShader(screenLineCode.Span);
        labelVs = device.CreateVertexShader(Gpu.Compile(source, "labelVertex", "vs_5_0").Span);
        terrainPs = device.CreatePixelShader(Gpu.Compile(source, "terrainPixel", "ps_5_0").Span);
        modelPs = device.CreatePixelShader(Gpu.Compile(source, "modelPixel", "ps_5_0").Span);
        waterPs = device.CreatePixelShader(Gpu.Compile(source, "waterPixel", "ps_5_0").Span);
        linePs = device.CreatePixelShader(Gpu.Compile(source, "linePixel", "ps_5_0").Span);
        labelPs = device.CreatePixelShader(Gpu.Compile(source, "labelPixel", "ps_5_0").Span);
        fillPs = device.CreatePixelShader(Gpu.Compile(source, "fillPixel", "ps_5_0").Span);
        terrainLayout = device.CreateInputLayout(
        [
            new InputElementDescription("POSITION", 0, Format.R32G32B32_Float, 0, 0),
            new InputElementDescription("NORMAL", 0, Format.R32G32B32_Float, 12, 0),
            new InputElementDescription("TEXCOORD", 0, Format.R32G32_Float, 24, 0),
            new InputElementDescription("TEXCOORD", 1, Format.R32G32_Float, 32, 0),
            new InputElementDescription("TEXCOORD", 2, Format.R32G32_Float, 40, 0),
            new InputElementDescription("TEXCOORD", 3, Format.R32G32_Float, 48, 0),
        ], terrainCode.Span);
        modelLayout = device.CreateInputLayout(
        [
            new InputElementDescription("POSITION", 0, Format.R32G32B32_Float, 0, 0),
            new InputElementDescription("NORMAL", 0, Format.R32G32B32_Float, 12, 0),
            new InputElementDescription("TEXCOORD", 0, Format.R32G32_Float, 24, 0),
            new InputElementDescription("INSTANCE", 0, Format.R32G32B32A32_Float, 0, 1, InputClassification.PerInstanceData, 1),
            new InputElementDescription("INSTANCE", 1, Format.R32G32B32A32_Float, 16, 1, InputClassification.PerInstanceData, 1),
            new InputElementDescription("INSTANCE", 2, Format.R32G32B32A32_Float, 32, 1, InputClassification.PerInstanceData, 1),
            new InputElementDescription("INSTANCE", 3, Format.R32G32B32A32_Float, 48, 1, InputClassification.PerInstanceData, 1),
            new InputElementDescription("COLOR", 0, Format.R32G32B32A32_Float, 64, 1, InputClassification.PerInstanceData, 1),
        ], modelCode.Span);
        pointLayout = device.CreateInputLayout([new InputElementDescription("POSITION", 0, Format.R32G32B32_Float, 0, 0)],
                                               waterCode.Span);
        screenPointLayout = device.CreateInputLayout([new InputElementDescription("POSITION", 0, Format.R32G32B32_Float, 0, 0)],
                                                     screenLineCode.Span);
        frameBuffer = gpu.ConstantBuffer<FrameUniforms>();
        partBuffer = gpu.ConstantBuffer<PartUniforms>();
        lineBuffer = gpu.DynamicVertexBuffer(lineCapacity * 12);
        opaque = gpu.Blend(Blend.One, Blend.Zero, false);
        alpha = gpu.Blend(Blend.SourceAlpha, Blend.InverseSourceAlpha);
        additive = gpu.Blend(Blend.One, Blend.One);
        multiply = gpu.Blend(Blend.Zero, Blend.SourceColor);
        premultiplied = gpu.Blend(Blend.One, Blend.InverseSourceAlpha);
        depthWrite = device.CreateDepthStencilState(new DepthStencilDescription(true, DepthWriteMask.All, ComparisonFunction.Less));
        depthRead = device.CreateDepthStencilState(new DepthStencilDescription(true, DepthWriteMask.Zero, ComparisonFunction.Less));
        depthAlways = device.CreateDepthStencilState(new DepthStencilDescription(false, DepthWriteMask.Zero, ComparisonFunction.Always));
        rasterizer = device.CreateRasterizerState(new RasterizerDescription(CullMode.None, FillMode.Solid) { DepthClipEnable = true });
        sampler = gpu.Sampler(TextureAddressMode.Wrap);
        // The atlas has 4 px borders between texture classes: small mips only, so they do not bleed.
        terrainSampler = gpu.Sampler(TextureAddressMode.Clamp, Filter.MinMagMipLinear, 2);
        // Labels are drawn texel for pixel.
        labelSampler = gpu.Sampler(TextureAddressMode.Clamp, Filter.MinMagMipPoint);
        marker = MakeMarkerMesh();
    }

    // MARK: Uploads

    private GpuMesh? Mesh(byte[] vertices, byte[] indices)
    {
        if (vertices.Length == 0 || indices.Length < 4) return null;
        return new GpuMesh(gpu.Buffer(vertices, BindFlags.VertexBuffer)!, gpu.Buffer(indices, BindFlags.IndexBuffer)!,
                           (uint)(indices.Length / 4));
    }

    /// Takes a newly opened map (models are dropped until Add brings them again).
    public void Show(MapScene newScene)
    {
        if (newScene.Key == sceneKey)
        {
            if (newScene.ObjectsRevision != Scene.ObjectsRevision || !ReferenceEquals(newScene.Objects, Scene.Objects))
                MarkDirty();
            Scene = newScene;
            return;
        }
        sceneKey = newScene.Key;
        Scene = newScene;
        atlas?.Dispose();
        waterTexture?.Dispose();
        atlas = newScene.Atlas is { } a ? gpu.Texture(a) : null;
        waterTexture = newScene.WaterTexture is { } w ? gpu.Texture(w) : null;
        foreach (var mesh in terrain.Concat(water)) Release(mesh);
        terrain.Clear();
        water.Clear();
        foreach (var (v, i) in newScene.Terrain)
            if (Mesh(v, i) is { } mesh) terrain.Add(mesh);
        foreach (var (v, i) in newScene.Water)
            if (Mesh(v, i) is { } mesh) water.Add(mesh);
        foreach (var model in models.Values)
            foreach (var part in model.Parts) Release(part.Mesh);
        models.Clear();
        failedModels.Clear();
        foreach (var texture in modelTextures.Values) texture.Dispose();
        modelTextures.Clear();
        MarkDirty();
        Camera.Frame(newScene);
    }

    private static void Release(GpuMesh mesh)
    {
        mesh.Vertices.Dispose();
        mesh.Indices.Dispose();
    }

    private void MarkDirty()
    {
        batchesDirty = true;
        ++contentGeneration;
    }

    /// Adds models (and the textures they use) that finished loading in the core.
    public void Add(IEnumerable<MapModelData> newModels, Dictionary<string, TextureData> textures)
    {
        foreach (var (name, image) in textures)
        {
            if (!modelTextures.ContainsKey(name) && gpu.Texture(image) is { } uploaded) modelTextures[name] = uploaded;
        }
        foreach (var model in newModels)
        {
            if (models.ContainsKey(model.Id) || failedModels.Contains(model.Id)) continue;
            var parts = new List<GpuPart>();
            foreach (var part in model.Parts)
            {
                if (Mesh(part.Vertices, part.Indices) is { } mesh)
                    parts.Add(new GpuPart(mesh, modelTextures.GetValueOrDefault(part.Texture), part.Blend, part.HouseColor));
            }
            if (model.Ready && parts.Count > 0) models[model.Id] = new GpuModel(parts, model.BoundsMin, model.BoundsMax);
            else failedModels.Add(model.Id);
            MarkDirty();
        }
    }

    public int LoadedModelCount => models.Count + failedModels.Count;

    // MARK: Instances

    /// The object as drawn: a selected object being dragged shows where it goes, at its height
    /// above the ground there.
    private MapObjectItem Displayed(MapObjectItem item)
    {
        if (overlay.Gesture.Kind != "move" || !selectionSet.Contains(item.Id)) return item;
        var xy = new Vector2(item.Position.X, item.Position.Y) + overlay.Gesture.Drag;
        return item.At(new Vector3(xy.X, xy.Y, Scene.GroundHeight(xy.X, xy.Y) + item.Height));
    }

    public MapObjectItem? Object(long id) => Scene.Objects.FirstOrDefault(o => o.Id == id) is { } found ? Displayed(found) : null;

    private static Matrix4x4 Transform(MapObjectItem item) =>
        Matrix4x4.CreateRotationZ(item.Angle) * Matrix4x4.CreateTranslation(item.Position);

    private static Vector4 PlayerColor(uint color) =>
        new(((color >> 16) & 0xFF) / 255f, ((color >> 8) & 0xFF) / 255f, (color & 0xFF) / 255f, 1);

    /// Markers: objects without a (loaded) model, colored by kind.
    private Vector4 MarkerColor(MapObjectItem item) => item.Kind switch
    {
        "waypoint" => item.Start > 0 ? PlayerColor(item.Color) : new Vector4(1, 0.85f, 0.2f, 1),
        "road" => new Vector4(0.55f, 0.55f, 0.55f, 1),
        "bridge" => new Vector4(0.65f, 0.45f, 0.3f, 1),
        "model" => failedModels.Contains(item.ModelId) ? new Vector4(0.95f, 0.3f, 0.3f, 1) : new Vector4(0.85f, 0.85f, 0.85f, 1),
        _ => new Vector4(0.3f, 0.8f, 0.95f, 1),
    };

    public bool DrawsModel(MapObjectItem item) => item.Kind == "model" && models.ContainsKey(item.ModelId);

    private void RebuildBatches()
    {
        var options = (ShowModels, ShowMarkers);
        if (!batchesDirty && options == batchOptions) return;
        batchesDirty = false;
        batchOptions = options;
        var byModel = new SortedDictionary<long, List<Instance>>();
        var markers = new List<Instance>();
        foreach (var stored in Scene.Objects)
        {
            var item = Displayed(stored);
            if (DrawsModel(item))
            {
                if (!options.ShowModels) continue;
                if (!byModel.TryGetValue(item.ModelId, out var list)) byModel[item.ModelId] = list = [];
                list.Add(new Instance { Transform = Transform(item), Color = PlayerColor(item.Color) });
            }
            else if (options.ShowMarkers || (item.Kind == "model" && options.ShowModels))
            {
                markers.Add(new Instance { Transform = Transform(item), Color = MarkerColor(item) });
            }
        }
        foreach (var batch in batches) batch.Instances.Dispose();
        batches.Clear();
        foreach (var (id, instances) in byModel)
            batches.Add((id, gpu.Buffer<Instance>(CollectionsMarshal.AsSpan(instances), BindFlags.VertexBuffer), (uint)instances.Count));
        markerInstances?.Dispose();
        markerCount = (uint)markers.Count;
        markerInstances = markers.Count == 0 ? null : gpu.Buffer<Instance>(CollectionsMarshal.AsSpan(markers), BindFlags.VertexBuffer);
    }

    /// A small upright diamond (model vertex layout), 12 units tall.
    private GpuMesh MakeMarkerMesh()
    {
        Vector3[] points = [new(0, 0, 12), new(4, 0, 6), new(0, 4, 6), new(-4, 0, 6), new(0, -4, 6), new(0, 0, 0)];
        (int, int, int)[] faces = [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 1), (5, 2, 1), (5, 3, 2), (5, 4, 3), (5, 1, 4)];
        var vertices = new List<float>();
        var indices = new List<uint>();
        foreach (var (a, b, c) in faces)
        {
            var normal = Vector3.Normalize(Vector3.Cross(points[b] - points[a], points[c] - points[a]));
            foreach (var index in new[] { a, b, c })
            {
                vertices.AddRange([points[index].X, points[index].Y, points[index].Z, normal.X, normal.Y, normal.Z, 0, 0, 0]);
                indices.Add((uint)indices.Count);
            }
        }
        return new GpuMesh(gpu.Buffer<float>(vertices.ToArray(), BindFlags.VertexBuffer),
                           gpu.Buffer<uint>(indices.ToArray(), BindFlags.IndexBuffer), (uint)indices.Count);
    }

    /// Bounds of an object in its own space (rotated with it): the model's, or the marker's.
    private (Vector3 Low, Vector3 High) LocalBounds(MapObjectItem item)
    {
        if (DrawsModel(item) && models.TryGetValue(item.ModelId, out var model))
        {
            // Thin models (signs, fences) stay clickable.
            var pad = Vector3.Max(new Vector3(2) - (model.BoundsMax - model.BoundsMin) / 2, Vector3.Zero);
            return (model.BoundsMin - pad, model.BoundsMax + pad);
        }
        return (new Vector3(-5, -5, 0), new Vector3(5, 5, 13));
    }

    private bool IsShown(MapObjectItem item) =>
        DrawsModel(item) ? ShowModels : ShowMarkers || (item.Kind == "model" && ShowModels);

    /// Where a ray enters an object's box (in its own space), null when it misses.
    private float? HitDistance(MapObjectItem item, (Vector3 Origin, Vector3 Direction) ray)
    {
        var (low, high) = LocalBounds(item);
        float c = MathF.Cos(-item.Angle), s = MathF.Sin(-item.Angle);
        Vector3 Local(Vector3 v) => new(c * v.X - s * v.Y, s * v.X + c * v.Y, v.Z);
        var origin = Local(ray.Origin - item.Position);
        var direction = Local(ray.Direction);
        float near = 0, far = float.MaxValue;
        for (int axis = 0; axis < 3; ++axis)
        {
            float o = origin[axis], d = direction[axis];
            if (MathF.Abs(d) < 1e-6f)
            {
                if (o < low[axis] || o > high[axis]) return null;
                continue;
            }
            var t0 = (low[axis] - o) / d;
            var t1 = (high[axis] - o) / d;
            if (t0 > t1) (t0, t1) = (t1, t0);
            near = MathF.Max(near, t0);
            far = MathF.Min(far, t1);
            if (near > far) return null;
        }
        return near;
    }

    /// The objects under a point of the view, nearest to the camera first.
    public List<long> Hits(Vector2 point, Vector2 size)
    {
        var ray = Camera.Ray(point, size);
        var found = new List<(long Id, float Near)>();
        foreach (var stored in Scene.Objects)
        {
            if (!IsShown(stored)) continue;
            if (HitDistance(Displayed(stored), ray) is { } near) found.Add((stored.Id, near));
        }
        return found.OrderBy(f => f.Near).Select(f => f.Id).ToList();
    }

    public long? Pick(Vector2 point, Vector2 size) => Hits(point, size) is { Count: > 0 } hits ? hits[0] : null;

    /// Shown objects whose middle is inside a rectangle of the view.
    public List<long> ObjectsIn(Rect rect, Vector2 size)
    {
        var result = new List<long>();
        foreach (var stored in Scene.Objects)
        {
            if (!IsShown(stored)) continue;
            var item = Displayed(stored);
            var (low, high) = LocalBounds(item);
            if (Camera.ViewPoint(item.Position + new Vector3(0, 0, (low.Z + high.Z) / 2), size) is { } p && rect.Contains(p.X, p.Y))
                result.Add(stored.Id);
        }
        return result;
    }

    // MARK: Drawing

    private FrameKey Key(Vector2 size) =>
        new(contentGeneration, overlay, ShowModels, ShowMarkers, ShowWater, Camera.Target, Camera.Yaw, Camera.Pitch,
            Camera.Distance, size);

    private void RefreshOverlay()
    {
        if (OverlaySource?.Invoke() is not { } fresh || fresh.Same(overlay)) return;
        if (!fresh.Selection.SequenceEqual(overlay.Selection) || fresh.Gesture != overlay.Gesture) MarkDirty();
        overlay = fresh;
        selectionSet = [.. fresh.Selection];
    }

    public bool NeedsFrame(Vector2 size)
    {
        RefreshOverlay();
        if (presented is not { } last) return true;
        var key = Key(size);
        return !(last.Generation == key.Generation && last.Overlay.Same(key.Overlay) && last with { Overlay = key.Overlay } == key);
    }

    public void Render(Target target, Vector2 size, float scale) => Render(target, size, scale, true);

    /// Draws the current view into `target`; `record`: keep the start labels (the view's frames).
    public void Render(Target target, Vector2 size, float scale, bool record)
    {
        RefreshOverlay();
        target.Begin(new Color4(0.12f, 0.13f, 0.15f, 1));
        if (record) presented = Key(size);
        if (terrain.Count == 0)
        {
            if (record) StartLabels = [];
            return;
        }
        RebuildBatches();
        var context = gpu.Context;
        gpu.Write(frameBuffer, Uniforms(size.X / MathF.Max(size.Y, 1)));
        context.RSSetState(rasterizer);
        context.VSSetConstantBuffer(0, frameBuffer);
        context.PSSetConstantBuffer(0, frameBuffer);
        context.VSSetConstantBuffer(1, partBuffer);
        context.PSSetConstantBuffer(1, partBuffer);
        context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);

        // Terrain.
        context.IASetInputLayout(terrainLayout);
        context.VSSetShader(terrainVs);
        context.PSSetShader(terrainPs);
        context.OMSetBlendState(opaque);
        context.OMSetDepthStencilState(depthWrite);
        context.PSSetShaderResource(0, atlas!);
        context.PSSetSampler(0, terrainSampler);
        foreach (var chunk in terrain)
        {
            context.IASetVertexBuffer(0, chunk.Vertices, 56);
            context.IASetIndexBuffer(chunk.Indices, Format.R32_UInt, 0);
            context.DrawIndexed(chunk.IndexCount, 0, 0);
        }

        // Models: solid parts, then markers; see-through parts after the water.
        context.PSSetSampler(0, sampler);
        context.IASetInputLayout(modelLayout);
        context.VSSetShader(modelVs);
        context.PSSetShader(modelPs);
        void DrawParts(bool solid)
        {
            foreach (var batch in batches)
            {
                if (!models.TryGetValue(batch.Model, out var model)) continue;
                foreach (var part in model.Parts)
                {
                    if ((part.Blend <= 1) != solid) continue;
                    context.OMSetBlendState(part.Blend switch { 2 => alpha, 3 => additive, 4 => multiply, _ => opaque });
                    context.OMSetDepthStencilState(part.Blend > 1 ? depthRead : depthWrite);
                    gpu.Write(partBuffer, new PartUniforms
                    {
                        FlagX = part.Texture == null ? 0u : 1u, FlagY = part.Blend is 1 or 2 ? 1u : 0u,
                        FlagZ = part.Blend <= 2 ? 1u : 0u, FlagW = (uint)part.HouseColor, BaseColor = new Vector4(0.62f, 0.64f, 0.66f, 1),
                    });
                    context.IASetVertexBuffer(0, part.Mesh.Vertices, 36);
                    context.IASetVertexBuffer(1, batch.Instances, (uint)Marshal.SizeOf<Instance>());
                    context.IASetIndexBuffer(part.Mesh.Indices, Format.R32_UInt, 0);
                    context.PSSetShaderResource(0, part.Texture!);
                    context.DrawIndexedInstanced(part.Mesh.IndexCount, batch.Count, 0, 0, 0);
                }
            }
        }
        DrawParts(true);
        if (markerInstances != null && markerCount > 0)
        {
            context.OMSetBlendState(opaque);
            context.OMSetDepthStencilState(depthWrite);
            gpu.Write(partBuffer, new PartUniforms { FlagZ = 1, FlagW = 2, BaseColor = Vector4.One });
            context.IASetVertexBuffer(0, marker.Vertices, 36);
            context.IASetVertexBuffer(1, markerInstances, (uint)Marshal.SizeOf<Instance>());
            context.IASetIndexBuffer(marker.Indices, Format.R32_UInt, 0);
            context.DrawIndexedInstanced(marker.IndexCount, markerCount, 0, 0, 0);
        }

        // Water: flat see-through surfaces.
        if (ShowWater && water.Count > 0)
        {
            context.IASetInputLayout(pointLayout);
            context.VSSetShader(waterVs);
            context.PSSetShader(waterPs);
            context.OMSetBlendState(alpha);
            context.OMSetDepthStencilState(depthRead);
            gpu.Write(partBuffer, new PartUniforms { FlagX = waterTexture == null ? 0u : 1u, BaseColor = Vector4.One });
            context.PSSetShaderResource(0, waterTexture!);
            foreach (var area in water)
            {
                context.IASetVertexBuffer(0, area.Vertices, 12);
                context.IASetIndexBuffer(area.Indices, Format.R32_UInt, 0);
                context.DrawIndexed(area.IndexCount, 0, 0);
            }
        }
        context.IASetInputLayout(modelLayout);
        context.VSSetShader(modelVs);
        context.PSSetShader(modelPs);
        DrawParts(false);

        // Waypoint links and road segments, then the selected objects' boxes and the points
        // waiting for input, over everything.
        context.IASetInputLayout(pointLayout);
        context.VSSetShader(lineVs);
        context.PSSetShader(linePs);
        context.OMSetBlendState(opaque);
        context.OMSetDepthStencilState(depthAlways);
        context.IASetPrimitiveTopology(PrimitiveTopology.LineList);
        if (ShowMarkers && Scene.Links.Count > 0)
        {
            var positions = Scene.Objects.ToDictionary(o => o.Id);
            var byKind = new Dictionary<string, List<Vector3>>();
            foreach (var link in Scene.Links)
            {
                if (!positions.TryGetValue(link.From, out var a) || !positions.TryGetValue(link.To, out var b)) continue;
                if (!byKind.TryGetValue(link.Kind, out var list)) byKind[link.Kind] = list = [];
                list.Add(Displayed(a).Position + new Vector3(0, 0, 3));
                list.Add(Displayed(b).Position + new Vector3(0, 0, 3));
            }
            Lines(byKind.GetValueOrDefault("waypoint"), new Vector4(1, 0.85f, 0.2f, 1));
            Lines(byKind.GetValueOrDefault("road"), new Vector4(0.8f, 0.8f, 0.8f, 1));
            Lines(byKind.GetValueOrDefault("bridge"), new Vector4(0.75f, 0.5f, 0.3f, 1));
        }
        var boxes = new List<Vector3>();
        int[] edges = [0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7, 7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7];
        foreach (var id in overlay.Selection)
        {
            if (Object(id) is not { } item) continue;
            var (low, high) = LocalBounds(item);
            var transform = Transform(item);
            var corners = new Vector3[8];
            for (int i = 0; i < 8; ++i)
            {
                var p = new Vector3((i & 1) == 0 ? low.X : high.X, (i & 2) == 0 ? low.Y : high.Y, (i & 4) == 0 ? low.Z : high.Z);
                corners[i] = Vector3.Transform(p, transform);
            }
            foreach (var e in edges) boxes.Add(corners[e]);
        }
        Lines(boxes, new Vector4(1, 0.9f, 0.2f, 1));
        // A road's first point and a right click waiting for an object: a post with a cross.
        var posts = new List<Vector3>();
        foreach (var point in new[] { overlay.RoadStart, overlay.Pending })
        {
            if (point is not { } p) continue;
            var ground = new Vector3(p.X, p.Y, Scene.GroundHeight(p.X, p.Y));
            posts.AddRange([ground, ground + new Vector3(0, 0, 20), ground + new Vector3(-6, 0, 1), ground + new Vector3(6, 0, 1),
                            ground + new Vector3(0, -6, 1), ground + new Vector3(0, 6, 1)]);
        }
        Lines(posts, new Vector4(0.3f, 1, 0.5f, 1));

        labelScale = scale;
        var labels = ShowMarkers ? PlaceStartLabels(size, scale) : [];
        if (record) StartLabels = labels;
        DrawStartLabels(labels, size);
        DrawBand(size);
    }

    private void Lines(List<Vector3>? points, Vector4 color, bool screen = false)
    {
        if (points == null || points.Count == 0) return;
        var context = gpu.Context;
        if (points.Count > lineCapacity)
        {
            lineBuffer.Dispose();
            lineCapacity = points.Count * 2;
            lineBuffer = gpu.DynamicVertexBuffer(lineCapacity * 12);
        }
        gpu.Write<Vector3>(lineBuffer, CollectionsMarshal.AsSpan(points));
        gpu.Write(partBuffer, new PartUniforms { BaseColor = color });
        if (screen)
        {
            context.IASetInputLayout(screenPointLayout);
            context.VSSetShader(screenLineVs);
        }
        context.IASetVertexBuffer(0, lineBuffer, 12);
        context.Draw((uint)points.Count, 0);
        if (screen)
        {
            context.IASetInputLayout(pointLayout);
            context.VSSetShader(lineVs);
        }
    }

    private FrameUniforms Uniforms(float aspect)
    {
        var l = Scene.Lighting.Length >= 18 ? Scene.Lighting : Enumerable.Repeat(0.5f, 18).ToArray();
        // The map stores where the light shines to; shading needs the way to the light.
        Vector4 ToLight(int i)
        {
            var direction = new Vector3(l[i], l[i + 1], l[i + 2]);
            var length = direction.Length();
            return length > 0 ? new Vector4(-direction / length, 0) : new Vector4(0, 0, 1, 0);
        }
        Vector4 Color(int i) => new(l[i], l[i + 1], l[i + 2], 1);
        return new FrameUniforms
        {
            ViewProjection = Camera.ViewProjection(aspect),
            TerrainToLight = ToLight(6), TerrainAmbient = Color(0), TerrainDiffuse = Color(3),
            ObjectToLight = ToLight(15), ObjectAmbient = Color(9), ObjectDiffuse = Color(12),
            Playable = Scene.Playable, Water = Scene.WaterColor,
            Params = new Vector4(MathF.Max(Scene.WaterRepeat, 1) * 10, 0, 0, 0),
        };
    }

    private static Vector4 ClipRect(Rect f, Vector2 size) =>
        new((float)(f.Left / size.X) * 2 - 1, 1 - (float)(f.Top / size.Y) * 2, (float)(f.Right / size.X) * 2 - 1,
            1 - (float)(f.Bottom / size.Y) * 2);

    /// The rubber band: a see-through rectangle with an outline in the accent color.
    private void DrawBand(Vector2 size)
    {
        if (overlay.Band is not var (a, b) || overlay.Gesture.Kind != "band") return;
        var context = gpu.Context;
        var rect = new Rect(new Point(a.X, a.Y), new Point(b.X, b.Y));
        var clip = ClipRect(rect, size);
        context.IASetPrimitiveTopology(PrimitiveTopology.TriangleStrip);
        context.VSSetShader(labelVs);
        context.PSSetShader(fillPs);
        context.OMSetBlendState(alpha);
        context.OMSetDepthStencilState(depthAlways);
        gpu.Write(partBuffer, new PartUniforms { BaseColor = new Vector4(0.0f, 0.47f, 0.84f, 0.15f), Rect = clip });
        context.Draw(4, 0);
        context.IASetPrimitiveTopology(PrimitiveTopology.LineList);
        context.OMSetBlendState(opaque);
        Lines([new(clip.X, clip.Y, 0), new(clip.Z, clip.Y, 0), new(clip.Z, clip.Y, 0), new(clip.Z, clip.W, 0),
               new(clip.Z, clip.W, 0), new(clip.X, clip.W, 0), new(clip.X, clip.W, 0), new(clip.X, clip.Y, 0)],
              new Vector4(0.0f, 0.47f, 0.84f, 1), screen: true);
    }

    // MARK: Start labels

    /// Where each start waypoint's label goes this frame: over the waypoint's marker, from the same
    /// camera as the frame; none behind the camera. Farther ones first, so nearer labels cover them.
    private List<StartLabel> PlaceStartLabels(Vector2 size, float scale)
    {
        var placed = new List<(float Depth, StartLabel Label)>();
        var eye = Camera.Eye;
        foreach (var stored in Scene.Objects)
        {
            if (stored.Start <= 0) continue;
            var item = Displayed(stored);
            var top = item.Position + new Vector3(0, 0, 13);
            if (Camera.ViewPoint(top, size) is not { } point) continue;
            var labelSize = LabelTexture(item.Start, item.Color, scale).Size;
            // Whole pixels: the texture is drawn texel for pixel.
            var anchor = new Vector2(MathF.Round(point.X), MathF.Round(point.Y));
            var frame = new Rect(anchor.X - MathF.Floor(labelSize.X / 2), anchor.Y - labelSize.Y, labelSize.X, labelSize.Y);
            placed.Add((Vector3.Distance(eye, top), new StartLabel(item.Start, item.Id, item.Color, anchor, frame)));
        }
        return placed.OrderByDescending(p => p.Depth).Select(p => p.Label).ToList();
    }

    private float labelScale = 1;

    private void DrawStartLabels(List<StartLabel> labels, Vector2 size)
    {
        if (labels.Count == 0 || size.X <= 0 || size.Y <= 0) return;
        var context = gpu.Context;
        context.IASetPrimitiveTopology(PrimitiveTopology.TriangleStrip);
        context.VSSetShader(labelVs);
        context.PSSetShader(labelPs);
        context.OMSetBlendState(premultiplied);
        context.OMSetDepthStencilState(depthAlways);
        context.PSSetSampler(0, labelSampler);
        foreach (var label in labels)
        {
            var texture = LabelTexture(label.Start, label.Color, labelScale);
            gpu.Write(partBuffer, new PartUniforms { Rect = ClipRect(label.Frame, size) });
            context.PSSetShaderResource(0, texture.Texture!);
            context.Draw(4, 0);
        }
        context.PSSetSampler(0, sampler);
    }

    /// The label of start position `start`: a pill in its color with "Start" and a big number, and a
    /// pointer down to the waypoint. Drawn once per start, color and scale; size in pixels.
    private (ID3D11ShaderResourceView Texture, Vector2 Size) LabelTexture(int start, uint color, float scale)
    {
        var key = (start, color, scale);
        if (labelTextures.TryGetValue(key, out var cached)) return cached;
        var (pixels, width, height) = DrawLabel(start, color, scale);
        var texture = gpu.Texture(new TextureData(width, height, pixels), false, Format.B8G8R8A8_UNorm)!;
        var result = (texture, new Vector2(width, height));
        labelTextures[key] = result;
        return result;
    }

    /// The label's picture: premultiplied BGRA, `scale` pixels per WPF unit.
    public static (byte[] Pixels, int Width, int Height) DrawLabel(int start, uint color, float scale)
    {
        var fill = System.Windows.Media.Color.FromRgb((byte)(color >> 16), (byte)(color >> 8), (byte)color);
        var luminance = (0.299 * fill.R + 0.587 * fill.G + 0.114 * fill.B) / 255;
        var ink = luminance > 0.6 ? new SolidColorBrush(System.Windows.Media.Color.FromRgb(20, 20, 20)) : Brushes.White;
        var family = new FontFamily("Segoe UI");
        var word = new FormattedText("Start ", CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                                     new Typeface(family, FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal), 12, ink, scale);
        var number = new FormattedText(start.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture,
                                       FlowDirection.LeftToRight,
                                       new Typeface(family, FontStyles.Normal, FontWeights.Black, FontStretches.Normal), 19, ink, scale);
        var textWidth = word.WidthIncludingTrailingWhitespace + number.Width;
        const double pillHeight = 26, pointer = 7, outline = 1.5;
        var pillWidth = Math.Ceiling(textWidth) + 18;
        var size = new Size(pillWidth + 2 * outline, pillHeight + pointer + 2 * outline);
        int width = (int)Math.Ceiling(size.Width * scale), height = (int)Math.Ceiling(size.Height * scale);
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            // Origin top left: the pill on top, the pointer's tip at the bottom middle.
            var body = new Rect(outline, outline, pillWidth, pillHeight);
            var shape = new PathGeometry();
            shape.AddGeometry(new RectangleGeometry(body, pillHeight / 2, pillHeight / 2));
            var tip = new PathFigure { StartPoint = new Point(size.Width / 2 - 6, body.Bottom - 1), IsClosed = true };
            tip.Segments.Add(new LineSegment(new Point(size.Width / 2, size.Height - outline), true));
            tip.Segments.Add(new LineSegment(new Point(size.Width / 2 + 6, body.Bottom - 1), true));
            shape.Figures.Add(tip);
            var flat = shape.GetFlattenedPathGeometry();
            var merged = Geometry.Combine(flat, Geometry.Empty, GeometryCombineMode.Union, null);
            dc.DrawGeometry(null, new Pen(new SolidColorBrush(System.Windows.Media.Color.FromArgb(191, 0, 0, 0)), outline * 2)
            {
                LineJoin = PenLineJoin.Round
            }, merged);
            dc.DrawGeometry(new SolidColorBrush(fill), null, merged);
            var origin = new Point(body.Left + (body.Width - textWidth) / 2, body.Top + (body.Height - number.Height) / 2);
            dc.DrawText(word, new Point(origin.X, origin.Y + number.Baseline - word.Baseline));
            dc.DrawText(number, new Point(origin.X + word.WidthIncludingTrailingWhitespace, origin.Y));
        }
        var bitmap = new RenderTargetBitmap(width, height, 96 * scale, 96 * scale, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        var pixels = new byte[width * height * 4];
        bitmap.CopyPixels(pixels, width * 4, 0);
        return (pixels, width, height);
    }

    /// Renders the current view into pixels (BGRA rows) without the window: the UI check's frame.
    public byte[] RenderPixels(int width, int height, float scale)
    {
        using var target = Target.Offscreen(width, height);
        Render(target, new Vector2(width, height), scale, false);
        return target.ReadPixels();
    }
}
