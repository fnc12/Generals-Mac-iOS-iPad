// Map view: draws the open map from the core's map_* tables with Metal. The core already built
// the terrain mesh (atlas coordinates and blend weights per corner), the water polygons and the
// models in their bind pose, so the renderer uploads and instances them.
import AppKit
import MetalKit
import SwiftUI
import simd

struct MapListItem: Identifiable, Hashable {
    let id: Int64
    let path: String
    let name: String
    let source: String
}

struct MapObjectItem: Identifiable, Hashable {
    let id: Int64
    let templateName: String
    let kind: String        // "model", "marker", "waypoint", "road", "bridge"
    let position: SIMD3<Float>
    let height: Float       // above the ground
    let angle: Float
    let flags: Int
    let modelId: Int64
    let owner: String
    let color: UInt32       // 0xRRGGBB

    func at(_ position: SIMD3<Float>) -> MapObjectItem {
        MapObjectItem(id: id, templateName: templateName, kind: kind, position: position, height: height, angle: angle,
                      flags: flags, modelId: modelId, owner: owner, color: color)
    }
}

/// A line the map draws between two objects (map_links): a waypoint link, a road or bridge segment.
struct MapLink: Hashable {
    let kind: String
    let from: Int64
    let to: Int64
}

/// The open map's editing state (map row columns).
struct MapEditState: Equatable {
    var objectRevision: Int64 = -1
    var undoCount = 0
    var redoCount = 0
    var edited = false
    var lastObject: Int64 = 0
    var message = ""
    var savedPath = ""
}

/// The core's map_editor row: the placement tool, what waits for input, the note. The client
/// shows it and sends commands; it decides nothing itself.
struct MapEditorState: Equatable {
    var tool = "object"
    var templateName = ""
    var roadType = ""
    var team = ""
    var angle: Double = 0
    var picker = false
    var pending: SIMD2<Float>?
    var roadStart: SIMD2<Float>?
    var note = ""
    var noteKind = ""
    var noteSerial: Int64 = 0
    var selectionRevision: Int64 = -1
    var selectionCount = 0
    var clipboardCount = 0
    var revision: Int64 = -1
}

/// The mouse gesture in progress (map_editor's gesture, drag_dx, drag_dy): kept apart from
/// MapEditorState so a drag does not re-render the bars and the inspector.
struct MapGesture: Equatable {
    var kind = ""
    var drag = SIMD2<Float>(0, 0)
}

/// What the renderer draws over the map besides the store's objects.
struct MapOverlay: Equatable {
    var selection: [Int64] = []
    var gesture = MapGesture()
    var roadStart: SIMD2<Float>?
    var pending: SIMD2<Float>?
}

struct RoadTypeItem: Hashable, Identifiable {
    let name: String
    let bridge: Bool
    var id: String { name }
}

/// A player of the map's SidesList; placed objects are owned by its default team.
struct MapPlayer: Hashable, Identifiable {
    let name: String
    let displayName: String
    let team: String
    let color: UInt32
    let neutral: Bool
    var id: String { team }
    var label: String {
        if neutral { return "Neutral" }
        return displayName.isEmpty || displayName == name ? name : "\(displayName) (\(name))"
    }
}

struct MapObjectProperty: Hashable {
    let key: String
    let type: String
    let value: String
}

struct MapModelPart {
    let texture: String
    let blend: Int
    let houseColor: Int
    let vertices: Data  // 36-byte records, bind pose
    let indices: Data
}

struct MapModelData {
    let id: Int64
    let name: String
    let ready: Bool
    let message: String
    let boundsMin: SIMD3<Float>
    let boundsMax: SIMD3<Float>
    let parts: [MapModelPart]
}

/// Everything of the open map but its models (those come later, see MapModelData).
struct MapScene {
    var key = ""  // changes when another map (or the same one again) is opened
    var name = ""
    var width = 0
    var height = 0
    var border = 0
    var playable = SIMD4<Float>(0, 0, 0, 0)
    var lighting = [Float](repeating: 0, count: 18)
    var heights = Data()
    var atlas: PreviewTexture?
    var waterTexture: PreviewTexture?
    var waterColor = SIMD4<Float>(0.5, 0.6, 1, 0.5)
    var waterRepeat: Float = 32
    var terrain: [(vertices: Data, indices: Data)] = []
    var water: [(vertices: Data, indices: Data)] = []
    var objects: [MapObjectItem] = []
    var links: [MapLink] = []
    var objectsRevision: Int64 = -1  // grows when edits change `objects` or `links`

    var isEmpty: Bool { heights.isEmpty }

    /// World x/y range of the whole height map (border included).
    var extent: SIMD4<Float> {
        SIMD4(Float(-border) * 10, Float(-border) * 10, Float(width - 1 - border) * 10, Float(height - 1 - border) * 10)
    }

    /// Ground height at a world position (bilinear, like the core's groundHeight).
    func groundHeight(x: Float, y: Float) -> Float {
        guard width > 1, height > 1, heights.count >= width * height else { return 0 }
        let fx = min(max(x / 10 + Float(border), 0), Float(width - 1))
        let fy = min(max(y / 10 + Float(border), 0), Float(height - 1))
        let x0 = min(Int(fx), width - 2), y0 = min(Int(fy), height - 2)
        let tx = fx - Float(x0), ty = fy - Float(y0)
        func at(_ i: Int, _ j: Int) -> Float { Float(heights[heights.startIndex + j * width + i]) * 0.625 }
        let south = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx
        let north = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx
        return south * (1 - ty) + north * ty
    }
}

/// Camera and display options shared by the SwiftUI controls, the view and the renderer.
final class MapViewState: ObservableObject {
    @Published var showModels = true
    @Published var showMarkers = true
    @Published var showWater = true
    var target = SIMD3<Float>(0, 0, 0)
    var yaw: Float = -.pi / 2     // camera south of the target, looking north
    var pitch: Float = 0.85
    var distance: Float = 1000
    let fovY: Float = 0.8
    var eye: SIMD3<Float> {
        target + distance * SIMD3(cos(pitch) * cos(yaw), cos(pitch) * sin(yaw), sin(pitch))
    }

    func viewProjection(aspect: Float) -> simd_float4x4 {
        let view = mapLookAt(eye: eye, center: target, up: SIMD3(0, 0, 1))
        let projection = mapPerspective(fovY: fovY, aspect: aspect, near: max(distance * 0.02, 1),
                                        far: distance * 6 + 5000)
        return projection * view
    }

    /// Frames the playable area from the south.
    func frame(_ scene: MapScene, top: Bool = false) {
        var rect = scene.playable
        if rect.z <= rect.x || rect.w <= rect.y {
            rect = SIMD4(0, 0, Float(max(scene.width - 1 - 2 * scene.border, 1)) * 10,
                         Float(max(scene.height - 1 - 2 * scene.border, 1)) * 10)
        }
        let center = SIMD2((rect.x + rect.z) / 2, (rect.y + rect.w) / 2)
        target = SIMD3(center.x, center.y, scene.groundHeight(x: center.x, y: center.y))
        yaw = -.pi / 2
        pitch = top ? 1.5 : 0.85
        let size = max(rect.z - rect.x, rect.w - rect.y)
        distance = size * 0.5 / tan(fovY / 2) * (top ? 1.05 : 1.15)
    }

    /// Drag in view points: the ground under the cursor follows it.
    func pan(dx: Float, dy: Float, viewHeight: Float) {
        let scale = 2 * distance * tan(fovY / 2) / max(viewHeight, 1)
        let forward = SIMD3(-cos(yaw), -sin(yaw), 0)
        let right = SIMD3(forward.y, -forward.x, 0)
        target -= right * dx * scale
        target += forward * dy * scale / max(sin(pitch), 0.35)
    }

    func rotate(dx: Float, dy: Float) {
        yaw -= dx * 0.01
        pitch = min(max(pitch + dy * 0.01, 0.15), 1.53)
    }

    func zoom(by factor: Float) {
        distance = min(max(distance * factor, 40), 30000)
    }

    /// Where the ray through a point of the view (origin top left) meets the terrain: marched in
    /// steps over the height map, then refined by bisection. Nil when it misses the map.
    func groundPoint(at point: CGPoint, size: CGSize, scene: MapScene) -> SIMD3<Float>? {
        guard !scene.isEmpty else { return nil }
        let (origin, direction) = ray(at: point, size: size)
        func above(_ t: Float) -> Bool {
            let p = origin + direction * t
            return p.z > scene.groundHeight(x: p.x, y: p.y)
        }
        let step = max(distance / 400, 2)
        var t: Float = 0
        var hit: Float?
        while t < distance * 8 + 5000 {
            if !above(t + step) {
                hit = t + step
                break
            }
            t += step
        }
        guard var high = hit else { return nil }
        var low = t
        for _ in 0..<24 {
            let middle = (low + high) / 2
            if above(middle) { low = middle } else { high = middle }
        }
        let p = origin + direction * high
        let extent = scene.extent
        guard p.x >= extent.x, p.y >= extent.y, p.x <= extent.z, p.y <= extent.w else { return nil }
        return SIMD3(p.x, p.y, scene.groundHeight(x: p.x, y: p.y))
    }

    /// Where a world point shows in the view (origin top left); nil behind the camera.
    func viewPoint(of world: SIMD3<Float>, size: CGSize) -> CGPoint? {
        let p = viewProjection(aspect: Float(size.width / max(size.height, 1))) * SIMD4(world, 1)
        guard p.w > 0 else { return nil }
        return CGPoint(x: CGFloat((p.x / p.w + 1) / 2) * size.width, y: CGFloat((1 - p.y / p.w) / 2) * size.height)
    }

    /// World ray through a point of the view (origin top left).
    func ray(at point: CGPoint, size: CGSize) -> (origin: SIMD3<Float>, direction: SIMD3<Float>) {
        let x = Float(point.x / max(size.width, 1)) * 2 - 1
        let y = 1 - Float(point.y / max(size.height, 1)) * 2
        let inverse = viewProjection(aspect: Float(size.width / max(size.height, 1))).inverse
        func unproject(_ z: Float) -> SIMD3<Float> {
            let p = inverse * SIMD4(x, y, z, 1)
            return SIMD3(p.x, p.y, p.z) / p.w
        }
        let near = unproject(0), far = unproject(1)
        return (near, simd_normalize(far - near))
    }
}

private let mapShaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 viewProjection;
    float4 terrainToLight;
    float4 terrainAmbient;
    float4 terrainDiffuse;
    float4 objectToLight;
    float4 objectAmbient;
    float4 objectDiffuse;
    float4 playable;  // x0 y0 x1 y1
    float4 water;     // rgb tint, a opacity
    float4 params;    // x: world units per water texture repeat
};

struct PartUniforms {
    uint4 flags;      // x: textured, y: alpha test, z: lit, w: player color (1 texture alpha, 2 tint)
    float4 baseColor;
};

struct Instance {
    float4x4 transform;
    float4 color;
};

struct TerrainIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float2 blendUv [[attribute(3)]];
    float2 extraUv [[attribute(4)]];
    float2 alpha [[attribute(5)]];
};

struct TerrainOut {
    float4 position [[position]];
    float3 world;
    float3 normal;
    float2 uv;
    float2 blendUv;
    float2 extraUv;
    float2 alpha;
};

vertex TerrainOut terrainVertex(TerrainIn in [[stage_in]], constant Uniforms &u [[buffer(1)]]) {
    TerrainOut out;
    out.position = u.viewProjection * float4(in.position, 1.0);
    out.world = in.position;
    out.normal = in.normal;
    out.uv = in.uv;
    out.blendUv = in.blendUv;
    out.extraUv = in.extraUv;
    out.alpha = in.alpha;
    return out;
}

fragment float4 terrainFragment(TerrainOut in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                                texture2d<float> atlas [[texture(0)]], sampler s [[sampler(0)]]) {
    float3 color = atlas.sample(s, in.uv).rgb;
    color = mix(color, atlas.sample(s, in.blendUv).rgb, in.alpha.x);
    color = mix(color, atlas.sample(s, in.extraUv).rgb, in.alpha.y);
    float light = max(dot(normalize(in.normal), u.terrainToLight.xyz), 0.0);
    color *= u.terrainAmbient.rgb + u.terrainDiffuse.rgb * light;
    // Outside the playable area, as WorldBuilder shades the border.
    if (in.world.x < u.playable.x || in.world.y < u.playable.y || in.world.x > u.playable.z || in.world.y > u.playable.w) {
        color *= 0.55;
    }
    return float4(color, 1.0);
}

struct ModelIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
};

struct ModelOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float4 color;
};

vertex ModelOut modelVertex(ModelIn in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                            device const Instance *instances [[buffer(2)]], uint instance [[instance_id]]) {
    Instance item = instances[instance];
    ModelOut out;
    out.position = u.viewProjection * (item.transform * float4(in.position, 1.0));
    out.normal = (item.transform * float4(in.normal, 0.0)).xyz;
    out.uv = in.uv;
    out.color = item.color;
    return out;
}

fragment float4 modelFragment(ModelOut in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                              constant PartUniforms &part [[buffer(3)]],
                              texture2d<float> colorTexture [[texture(0)]], sampler s [[sampler(0)]]) {
    float4 color = part.baseColor;
    if (part.flags.x != 0) {
        color = colorTexture.sample(s, in.uv);
    }
    if (part.flags.w == 1) {
        color = float4(mix(in.color.rgb, color.rgb, color.a), 1.0);
    } else if (part.flags.w == 2) {
        color.rgb *= in.color.rgb;
    }
    if (part.flags.y != 0 && color.a < 0.375) discard_fragment();
    if (part.flags.z != 0) {
        float light = abs(dot(normalize(in.normal), u.objectToLight.xyz));
        color.rgb *= u.objectAmbient.rgb + u.objectDiffuse.rgb * light;
    }
    return color;
}

struct WaterOut {
    float4 position [[position]];
    float2 uv;
};

vertex WaterOut waterVertex(uint id [[vertex_id]], device const packed_float3 *points [[buffer(0)]],
                            constant Uniforms &u [[buffer(1)]]) {
    float3 p = float3(points[id]);
    WaterOut out;
    out.position = u.viewProjection * float4(p, 1.0);
    out.uv = p.xy / u.params.x;
    return out;
}

fragment float4 waterFragment(WaterOut in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                              constant PartUniforms &part [[buffer(3)]],
                              texture2d<float> waterTexture [[texture(0)]], sampler s [[sampler(0)]]) {
    float3 color = part.flags.x != 0 ? waterTexture.sample(s, in.uv).rgb : float3(0.25, 0.4, 0.6);
    color *= u.water.rgb * (u.terrainAmbient.rgb + u.terrainDiffuse.rgb);
    return float4(color, u.water.a);
}

vertex float4 lineVertex(uint id [[vertex_id]], device const packed_float3 *points [[buffer(0)]],
                         constant Uniforms &u [[buffer(1)]]) {
    return u.viewProjection * float4(float3(points[id]), 1.0);
}

fragment float4 lineFragment(constant PartUniforms &part [[buffer(3)]]) {
    return part.baseColor;
}
"""

private struct MapUniforms {
    var viewProjection: simd_float4x4
    var terrainToLight: SIMD4<Float>
    var terrainAmbient: SIMD4<Float>
    var terrainDiffuse: SIMD4<Float>
    var objectToLight: SIMD4<Float>
    var objectAmbient: SIMD4<Float>
    var objectDiffuse: SIMD4<Float>
    var playable: SIMD4<Float>
    var water: SIMD4<Float>
    var params: SIMD4<Float>
}

private struct PartUniforms {
    var flags: SIMD4<UInt32>
    var baseColor: SIMD4<Float>
}

private struct Instance {
    var transform: simd_float4x4
    var color: SIMD4<Float>
}

final class MapRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    let state: MapViewState
    private let queue: MTLCommandQueue
    private var terrainPipeline: MTLRenderPipelineState!
    private var waterPipeline: MTLRenderPipelineState!
    private var linePipeline: MTLRenderPipelineState!
    private var modelPipelines: [Int: MTLRenderPipelineState] = [:]  // by blend mode
    private var depthState: MTLDepthStencilState!
    private var readOnlyDepthState: MTLDepthStencilState!
    private var overlayDepthState: MTLDepthStencilState!
    private var sampler: MTLSamplerState!
    private var terrainSampler: MTLSamplerState!

    private struct GPUMesh {
        let vertices: MTLBuffer
        let indices: MTLBuffer
        let indexCount: Int
    }
    private struct GPUPart {
        let mesh: GPUMesh
        let texture: MTLTexture?
        let blend: Int
        let houseColor: Int
    }
    private struct GPUModel {
        let parts: [GPUPart]
        let boundsMin: SIMD3<Float>
        let boundsMax: SIMD3<Float>
    }
    private var sceneKey = ""
    private(set) var scene = MapScene()
    private var atlas: MTLTexture?
    private var waterTexture: MTLTexture?
    private var terrain: [GPUMesh] = []
    private var water: [GPUMesh] = []
    private var models: [Int64: GPUModel] = [:]
    private var failedModels: Set<Int64> = []
    private var modelTextures: [String: MTLTexture] = [:]
    private var batches: [(model: Int64, instances: MTLBuffer, count: Int)] = []
    private var markerInstances: MTLBuffer?
    private var markerCount = 0
    private var marker: GPUMesh?
    private var batchesDirty = true {
        didSet { if batchesDirty { contentGeneration += 1 } }
    }
    private var batchOptions = (models: true, markers: true)
    /// Selection, gesture and points waiting for input, from the store (the editor model);
    /// asked for every frame. Without a source (snapshots), `overlay` is used as it is.
    var overlaySource: (@MainActor () -> MapOverlay)?
    var overlay = MapOverlay() {
        didSet {
            if overlay.selection != oldValue.selection || overlay.gesture != oldValue.gesture { batchesDirty = true }
            selectionSet = Set(overlay.selection)
        }
    }
    private var selectionSet: Set<Int64> = []
    /// Grows whenever what is drawn changes other than the camera (scene, models, batches).
    private var contentGeneration = 0
    /// What the last presented frame showed; a frame that would look the same is not drawn.
    private var presented: FrameKey?

    private struct FrameKey: Equatable {
        var generation: Int
        var overlay: MapOverlay
        var showModels: Bool, showMarkers: Bool, showWater: Bool
        var target: SIMD3<Float>
        var yaw: Float, pitch: Float, distance: Float
        var size: CGSize
    }

    init?(state: MapViewState) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        self.state = state
        super.init()
        do {
            try makePipelines()
        } catch {
            NSLog("END Editor: map shaders failed: \(error)")
            return nil
        }
        marker = makeMarkerMesh()
    }

    private func makePipelines() throws {
        let library = try device.makeLibrary(source: mapShaderSource, options: nil)
        func descriptor(_ vertex: String, _ fragment: String, layout: MTLVertexDescriptor?) -> MTLRenderPipelineDescriptor {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: vertex)
            descriptor.fragmentFunction = library.makeFunction(name: fragment)
            descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
            descriptor.depthAttachmentPixelFormat = .depth32Float
            descriptor.vertexDescriptor = layout
            return descriptor
        }
        func layout(_ formats: [(MTLVertexFormat, Int)], stride: Int) -> MTLVertexDescriptor {
            let layout = MTLVertexDescriptor()
            for (index, (format, offset)) in formats.enumerated() {
                layout.attributes[index].format = format
                layout.attributes[index].offset = offset
                layout.attributes[index].bufferIndex = 0
            }
            layout.layouts[0].stride = stride
            return layout
        }
        func alphaBlend(_ descriptor: MTLRenderPipelineDescriptor) -> MTLRenderPipelineDescriptor {
            let attachment = descriptor.colorAttachments[0]!
            attachment.isBlendingEnabled = true
            attachment.sourceRGBBlendFactor = .sourceAlpha
            attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
            attachment.sourceAlphaBlendFactor = .zero
            attachment.destinationAlphaBlendFactor = .one
            return descriptor
        }
        terrainPipeline = try device.makeRenderPipelineState(descriptor: descriptor(
            "terrainVertex", "terrainFragment",
            layout: layout([(.float3, 0), (.float3, 12), (.float2, 24), (.float2, 32), (.float2, 40), (.float2, 48)],
                           stride: 56)))
        waterPipeline = try device.makeRenderPipelineState(descriptor: alphaBlend(descriptor("waterVertex", "waterFragment",
                                                                                             layout: nil)))
        linePipeline = try device.makeRenderPipelineState(descriptor: descriptor("lineVertex", "lineFragment", layout: nil))
        let modelLayout = layout([(.float3, 0), (.float3, 12), (.float2, 24)], stride: 36)
        for blend in 0...4 {
            let model = descriptor("modelVertex", "modelFragment", layout: modelLayout)
            let attachment = model.colorAttachments[0]!
            attachment.isBlendingEnabled = blend >= 2
            switch blend {
            case 2:
                attachment.sourceRGBBlendFactor = .sourceAlpha
                attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
            case 3:
                attachment.sourceRGBBlendFactor = .one
                attachment.destinationRGBBlendFactor = .one
            case 4:
                attachment.sourceRGBBlendFactor = .zero
                attachment.destinationRGBBlendFactor = .sourceColor
            default:
                break
            }
            attachment.sourceAlphaBlendFactor = .zero
            attachment.destinationAlphaBlendFactor = .one
            modelPipelines[blend] = try device.makeRenderPipelineState(descriptor: model)
        }
        let depth = MTLDepthStencilDescriptor()
        depth.depthCompareFunction = .less
        depth.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: depth)
        depth.isDepthWriteEnabled = false
        readOnlyDepthState = device.makeDepthStencilState(descriptor: depth)
        depth.depthCompareFunction = .always
        overlayDepthState = device.makeDepthStencilState(descriptor: depth)

        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.mipFilter = .linear
        samplerDescriptor.sAddressMode = .repeat
        samplerDescriptor.tAddressMode = .repeat
        sampler = device.makeSamplerState(descriptor: samplerDescriptor)
        // The atlas has 4 px borders between texture classes: small mips only, so they do not bleed.
        samplerDescriptor.sAddressMode = .clampToEdge
        samplerDescriptor.tAddressMode = .clampToEdge
        samplerDescriptor.lodMaxClamp = 2
        terrainSampler = device.makeSamplerState(descriptor: samplerDescriptor)
    }

    // MARK: Uploads

    private func buffer(_ data: Data) -> MTLBuffer? {
        guard !data.isEmpty else { return nil }
        return data.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) }
    }

    private func mesh(_ vertices: Data, _ indices: Data) -> GPUMesh? {
        guard let vertexBuffer = buffer(vertices), let indexBuffer = buffer(indices) else { return nil }
        return GPUMesh(vertices: vertexBuffer, indices: indexBuffer, indexCount: indices.count / 4)
    }

    private func texture(_ image: PreviewTexture) -> MTLTexture? {
        guard image.width > 0, image.height > 0, image.rgba.count >= image.width * image.height * 4 else { return nil }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: image.width,
                                                                  height: image.height, mipmapped: true)
        descriptor.usage = .shaderRead
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        image.rgba.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(0, 0, image.width, image.height), mipmapLevel: 0,
                            withBytes: $0.baseAddress!, bytesPerRow: image.width * 4)
        }
        return texture
    }

    private func generateMipmaps(_ textures: [MTLTexture]) {
        guard !textures.isEmpty, let commands = queue.makeCommandBuffer(),
              let blit = commands.makeBlitCommandEncoder() else { return }
        for texture in textures { blit.generateMipmaps(for: texture) }
        blit.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
    }

    /// Takes a newly opened map (models are dropped until `add(models:)` brings them again).
    func show(_ newScene: MapScene) {
        guard newScene.key != sceneKey else {
            if newScene.objectsRevision != scene.objectsRevision {
                batchesDirty = true
            }
            scene = newScene
            return
        }
        sceneKey = newScene.key
        scene = newScene
        atlas = newScene.atlas.flatMap(texture)
        waterTexture = newScene.waterTexture.flatMap(texture)
        generateMipmaps([atlas, waterTexture].compactMap { $0 })
        terrain = newScene.terrain.compactMap { mesh($0.vertices, $0.indices) }
        water = newScene.water.compactMap { mesh($0.vertices, $0.indices) }
        models = [:]
        failedModels = []
        modelTextures = [:]
        batchesDirty = true
        state.frame(newScene)
    }

    /// Adds models (and the textures they use) that finished loading in the core.
    func add(models newModels: [MapModelData], textures: [String: PreviewTexture]) {
        var fresh: [MTLTexture] = []
        for (name, image) in textures where modelTextures[name] == nil {
            if let uploaded = texture(image) {
                modelTextures[name] = uploaded
                fresh.append(uploaded)
            }
        }
        generateMipmaps(fresh)
        for model in newModels where models[model.id] == nil && !failedModels.contains(model.id) {
            let parts = model.parts.compactMap { part -> GPUPart? in
                guard let gpu = mesh(part.vertices, part.indices) else { return nil }
                return GPUPart(mesh: gpu, texture: modelTextures[part.texture], blend: part.blend,
                               houseColor: part.houseColor)
            }
            if model.ready && !parts.isEmpty {
                models[model.id] = GPUModel(parts: parts, boundsMin: model.boundsMin, boundsMax: model.boundsMax)
            } else {
                failedModels.insert(model.id)
            }
            batchesDirty = true
        }
    }

    var loadedModelCount: Int { models.count + failedModels.count }

    // MARK: Instances

    /// The object as drawn: a selected object being dragged shows where it goes, at its height
    /// above the ground there.
    private func displayed(_ object: MapObjectItem) -> MapObjectItem {
        guard overlay.gesture.kind == "move", selectionSet.contains(object.id) else { return object }
        let xy = SIMD2(object.position.x, object.position.y) + overlay.gesture.drag
        return object.at(SIMD3(xy.x, xy.y, scene.groundHeight(x: xy.x, y: xy.y) + object.height))
    }

    func object(_ id: Int64) -> MapObjectItem? {
        scene.objects.first { $0.id == id }.map(displayed)
    }

    private static func transform(of object: MapObjectItem) -> simd_float4x4 {
        let c = cos(object.angle), s = sin(object.angle)
        return simd_float4x4(columns: (SIMD4(c, s, 0, 0), SIMD4(-s, c, 0, 0), SIMD4(0, 0, 1, 0),
                                       SIMD4(object.position.x, object.position.y, object.position.z, 1)))
    }

    private static func playerColor(_ object: MapObjectItem) -> SIMD4<Float> {
        SIMD4(Float((object.color >> 16) & 0xFF) / 255, Float((object.color >> 8) & 0xFF) / 255,
              Float(object.color & 0xFF) / 255, 1)
    }

    /// Markers: objects without a (loaded) model, colored by kind.
    private func markerColor(_ object: MapObjectItem) -> SIMD4<Float> {
        switch object.kind {
        case "waypoint": return SIMD4(1, 0.85, 0.2, 1)
        case "road": return SIMD4(0.55, 0.55, 0.55, 1)
        case "bridge": return SIMD4(0.65, 0.45, 0.3, 1)
        case "model": return failedModels.contains(object.modelId) ? SIMD4(0.95, 0.3, 0.3, 1) : SIMD4(0.85, 0.85, 0.85, 1)
        default: return SIMD4(0.3, 0.8, 0.95, 1)
        }
    }

    func drawsModel(_ object: MapObjectItem) -> Bool {
        object.kind == "model" && models[object.modelId] != nil
    }

    private func rebuildBatches() {
        let options = (models: state.showModels, markers: state.showMarkers)
        guard batchesDirty || options != batchOptions else { return }
        batchesDirty = false
        batchOptions = options
        var byModel: [Int64: [Instance]] = [:]
        var markers: [Instance] = []
        for stored in scene.objects {
            let object = displayed(stored)
            if drawsModel(object) {
                if options.models {
                    byModel[object.modelId, default: []].append(Instance(transform: Self.transform(of: object),
                                                                         color: Self.playerColor(object)))
                }
            } else if options.markers || (object.kind == "model" && options.models) {
                markers.append(Instance(transform: Self.transform(of: object), color: markerColor(object)))
            }
        }
        batches = byModel.keys.sorted().compactMap { id in
            let instances = byModel[id]!
            guard let buffer = instances.withUnsafeBytes({
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
            }) else { return nil }
            return (id, buffer, instances.count)
        }
        markerCount = markers.count
        markerInstances = markers.isEmpty ? nil : markers.withUnsafeBytes {
            device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
        }
    }

    /// A small upright diamond (ModelVertex layout), 12 units tall.
    private func makeMarkerMesh() -> GPUMesh? {
        let points: [SIMD3<Float>] = [SIMD3(0, 0, 12), SIMD3(4, 0, 6), SIMD3(0, 4, 6), SIMD3(-4, 0, 6), SIMD3(0, -4, 6),
                                      SIMD3(0, 0, 0)]
        let faces: [(Int, Int, Int)] = [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 1), (5, 2, 1), (5, 3, 2), (5, 4, 3), (5, 1, 4)]
        var vertices = Data()
        var indices = Data()
        for (a, b, c) in faces {
            let normal = simd_normalize(simd_cross(points[b] - points[a], points[c] - points[a]))
            for index in [a, b, c] {
                var record: [Float] = [points[index].x, points[index].y, points[index].z, normal.x, normal.y, normal.z, 0, 0]
                var bone: UInt32 = 0
                vertices.append(Data(bytes: &record, count: 32))
                vertices.append(Data(bytes: &bone, count: 4))
                var position = UInt32(indices.count / 4)
                indices.append(Data(bytes: &position, count: 4))
            }
        }
        return mesh(vertices, indices)
    }

    /// Bounds of an object in its own space (rotated with it): the model's, or the marker's.
    private func localBounds(_ object: MapObjectItem) -> (SIMD3<Float>, SIMD3<Float>) {
        if drawsModel(object), let model = models[object.modelId] {
            // Thin models (signs, fences) stay clickable.
            let pad = simd_max(SIMD3<Float>(repeating: 2) - (model.boundsMax - model.boundsMin) / 2, .zero)
            return (model.boundsMin - pad, model.boundsMax + pad)
        }
        return (SIMD3(-5, -5, 0), SIMD3(5, 5, 13))
    }

    private func isShown(_ object: MapObjectItem) -> Bool {
        drawsModel(object) ? state.showModels : (state.showMarkers || (object.kind == "model" && state.showModels))
    }

    /// Where a ray enters an object's box (in its own space), nil when it misses.
    private func hitDistance(_ object: MapObjectItem, _ ray: (origin: SIMD3<Float>, direction: SIMD3<Float>)) -> Float? {
        let (low, high) = localBounds(object)
        let c = cos(-object.angle), s = sin(-object.angle)
        func local(_ v: SIMD3<Float>) -> SIMD3<Float> { SIMD3(c * v.x - s * v.y, s * v.x + c * v.y, v.z) }
        let origin = local(ray.origin - object.position)
        let direction = local(ray.direction)
        var near: Float = 0, far = Float.greatestFiniteMagnitude
        for axis in 0..<3 {
            if abs(direction[axis]) < 1e-6 {
                if origin[axis] < low[axis] || origin[axis] > high[axis] { return nil }
                continue
            }
            var t0 = (low[axis] - origin[axis]) / direction[axis]
            var t1 = (high[axis] - origin[axis]) / direction[axis]
            if t0 > t1 { swap(&t0, &t1) }
            near = max(near, t0)
            far = min(far, t1)
            if near > far { return nil }
        }
        return near
    }

    /// The objects under a point of the view (origin top left), nearest to the camera first.
    func hits(at point: CGPoint, size: CGSize) -> [Int64] {
        let ray = state.ray(at: point, size: size)
        var found: [(Int64, Float)] = []
        for stored in scene.objects where isShown(stored) {
            if let near = hitDistance(displayed(stored), ray) { found.append((stored.id, near)) }
        }
        return found.sorted { $0.1 < $1.1 }.map(\.0)
    }

    /// The object under a point of the view, nearest to the camera.
    func pick(at point: CGPoint, size: CGSize) -> Int64? { hits(at: point, size: size).first }

    /// Shown objects whose middle is inside a rectangle of the view (origin top left).
    func objects(in rect: CGRect, size: CGSize) -> [Int64] {
        scene.objects.filter { stored in
            guard isShown(stored) else { return false }
            let object = displayed(stored)
            let (low, high) = localBounds(object)
            guard let point = state.viewPoint(of: object.position + SIMD3(0, 0, (low.z + high.z) / 2), size: size)
            else { return false }
            return rect.contains(point)
        }.map(\.id)
    }

    // MARK: Drawing

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        // The view's display link calls this every vsync. Drawing an unchanged frame is not only
        // wasted: asking for a drawable blocks the main thread until the GPU frees one, which
        // starves everything else on it (scrolling the map list, SwiftUI updates).
        if let overlaySource {
            let fresh = overlaySource()
            if fresh != overlay { overlay = fresh }
        }
        let key = FrameKey(generation: contentGeneration, overlay: overlay, showModels: state.showModels,
                           showMarkers: state.showMarkers, showWater: state.showWater, target: state.target,
                           yaw: state.yaw, pitch: state.pitch, distance: state.distance, size: view.drawableSize)
        guard key != presented else { return }
        guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }
        let started = CACurrentMediaTime()
        let size = view.drawableSize
        encode(encoder, aspect: Float(size.width / max(size.height, 1)))
        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
        presented = key
        PerfRecorder.shared?.mapFrame(encode: CACurrentMediaTime() - started)
    }

    /// Renders the current view into an image without a window (for `--map-snapshot`).
    func renderImage(width: Int, height: Int) -> CGImage? {
        let colorDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width,
                                                                       height: height, mipmapped: false)
        colorDescriptor.usage = [.renderTarget, .shaderRead]
        colorDescriptor.storageMode = .shared
        let depthDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: width,
                                                                       height: height, mipmapped: false)
        depthDescriptor.usage = .renderTarget
        depthDescriptor.storageMode = .private
        guard let color = device.makeTexture(descriptor: colorDescriptor),
              let depth = device.makeTexture(descriptor: depthDescriptor),
              let commands = queue.makeCommandBuffer() else { return nil }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = color
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.12, green: 0.13, blue: 0.15, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 1
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return nil }
        encode(encoder, aspect: Float(width) / Float(height))
        encoder.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        color.getBytes(&pixels, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: false, intent: .defaultIntent)
    }

    private func uniforms(aspect: Float) -> MapUniforms {
        let l = scene.lighting.count >= 18 ? scene.lighting : [Float](repeating: 0.5, count: 18)
        // The map stores where the light shines to; shading needs the way to the light.
        func toLight(_ i: Int) -> SIMD4<Float> {
            let direction = SIMD3(l[i], l[i + 1], l[i + 2])
            let length = simd_length(direction)
            return length > 0 ? SIMD4(-direction / length, 0) : SIMD4(0, 0, 1, 0)
        }
        func color(_ i: Int) -> SIMD4<Float> { SIMD4(l[i], l[i + 1], l[i + 2], 1) }
        return MapUniforms(viewProjection: state.viewProjection(aspect: aspect),
                           terrainToLight: toLight(6), terrainAmbient: color(0), terrainDiffuse: color(3),
                           objectToLight: toLight(15), objectAmbient: color(9), objectDiffuse: color(12),
                           playable: scene.playable, water: scene.waterColor,
                           params: SIMD4(max(scene.waterRepeat, 1) * 10, 0, 0, 0))
    }

    private func encode(_ encoder: MTLRenderCommandEncoder, aspect: Float) {
        guard !terrain.isEmpty else { return }
        rebuildBatches()
        var u = uniforms(aspect: aspect)
        encoder.setCullMode(.none)

        // Terrain.
        encoder.setRenderPipelineState(terrainPipeline)
        encoder.setDepthStencilState(depthState)
        encoder.setVertexBytes(&u, length: MemoryLayout<MapUniforms>.stride, index: 1)
        encoder.setFragmentBytes(&u, length: MemoryLayout<MapUniforms>.stride, index: 1)
        encoder.setFragmentTexture(atlas, index: 0)
        encoder.setFragmentSamplerState(terrainSampler, index: 0)
        for chunk in terrain {
            encoder.setVertexBuffer(chunk.vertices, offset: 0, index: 0)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: chunk.indexCount, indexType: .uint32,
                                          indexBuffer: chunk.indices, indexBufferOffset: 0)
        }

        // Models: solid parts, then markers; see-through parts after the water.
        encoder.setFragmentSamplerState(sampler, index: 0)
        func drawParts(_ solid: Bool) {
            for batch in batches {
                guard let model = models[batch.model] else { continue }
                for part in model.parts where (part.blend <= 1) == solid {
                    guard let pipeline = modelPipelines[part.blend] ?? modelPipelines[0] else { continue }
                    encoder.setRenderPipelineState(pipeline)
                    encoder.setDepthStencilState(part.blend > 1 ? readOnlyDepthState : depthState)
                    var flags = PartUniforms(flags: SIMD4(part.texture == nil ? 0 : 1, part.blend == 1 || part.blend == 2 ? 1 : 0,
                                                          part.blend <= 2 ? 1 : 0, UInt32(part.houseColor)),
                                             baseColor: SIMD4(0.62, 0.64, 0.66, 1))
                    encoder.setVertexBuffer(part.mesh.vertices, offset: 0, index: 0)
                    encoder.setVertexBuffer(batch.instances, offset: 0, index: 2)
                    encoder.setFragmentBytes(&flags, length: MemoryLayout<PartUniforms>.stride, index: 3)
                    encoder.setFragmentTexture(part.texture, index: 0)
                    encoder.drawIndexedPrimitives(type: .triangle, indexCount: part.mesh.indexCount, indexType: .uint32,
                                                  indexBuffer: part.mesh.indices, indexBufferOffset: 0,
                                                  instanceCount: batch.count)
                }
            }
        }
        drawParts(true)
        if let marker, let markerInstances, markerCount > 0 {
            encoder.setRenderPipelineState(modelPipelines[0]!)
            encoder.setDepthStencilState(depthState)
            var flags = PartUniforms(flags: SIMD4(0, 0, 1, 2), baseColor: SIMD4(1, 1, 1, 1))
            encoder.setVertexBuffer(marker.vertices, offset: 0, index: 0)
            encoder.setVertexBuffer(markerInstances, offset: 0, index: 2)
            encoder.setFragmentBytes(&flags, length: MemoryLayout<PartUniforms>.stride, index: 3)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: marker.indexCount, indexType: .uint32,
                                          indexBuffer: marker.indices, indexBufferOffset: 0, instanceCount: markerCount)
        }

        // Water: flat see-through surfaces.
        if state.showWater && !water.isEmpty {
            encoder.setRenderPipelineState(waterPipeline)
            encoder.setDepthStencilState(readOnlyDepthState)
            var flags = PartUniforms(flags: SIMD4(waterTexture == nil ? 0 : 1, 0, 0, 0), baseColor: SIMD4(1, 1, 1, 1))
            encoder.setFragmentBytes(&flags, length: MemoryLayout<PartUniforms>.stride, index: 3)
            encoder.setFragmentTexture(waterTexture, index: 0)
            for area in water {
                encoder.setVertexBuffer(area.vertices, offset: 0, index: 0)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: area.indexCount, indexType: .uint32,
                                              indexBuffer: area.indices, indexBufferOffset: 0)
            }
        }
        drawParts(false)

        // Waypoint links and road segments, then the selected objects' boxes and the points
        // waiting for input, over everything.
        encoder.setRenderPipelineState(linePipeline)
        encoder.setDepthStencilState(overlayDepthState)
        func lines(_ points: [SIMD3<Float>], _ color: SIMD4<Float>) {
            guard !points.isEmpty else { return }
            var values = points.flatMap { [$0.x, $0.y, $0.z] }
            var flags = PartUniforms(flags: .zero, baseColor: color)
            values.withUnsafeMutableBytes { bytes in
                if bytes.count <= 4096 {
                    encoder.setVertexBytes(bytes.baseAddress!, length: bytes.count, index: 0)
                } else if let buffer = device.makeBuffer(bytes: bytes.baseAddress!, length: bytes.count) {
                    encoder.setVertexBuffer(buffer, offset: 0, index: 0)
                }
            }
            encoder.setFragmentBytes(&flags, length: MemoryLayout<PartUniforms>.stride, index: 3)
            encoder.drawPrimitives(type: .line, vertexStart: 0, vertexCount: points.count)
        }
        if state.showMarkers && !scene.links.isEmpty {
            var byKind: [String: [SIMD3<Float>]] = [:]
            let positions = Dictionary(uniqueKeysWithValues: scene.objects.map { ($0.id, $0) })
            for link in scene.links {
                guard let a = positions[link.from].map(displayed), let b = positions[link.to].map(displayed) else { continue }
                byKind[link.kind, default: []] += [a.position + SIMD3(0, 0, 3), b.position + SIMD3(0, 0, 3)]
            }
            lines(byKind["waypoint"] ?? [], SIMD4(1, 0.85, 0.2, 1))
            lines(byKind["road"] ?? [], SIMD4(0.8, 0.8, 0.8, 1))
            lines(byKind["bridge"] ?? [], SIMD4(0.75, 0.5, 0.3, 1))
        }
        var boxes: [SIMD3<Float>] = []
        for id in overlay.selection {
            guard let object = object(id) else { continue }
            let (low, high) = localBounds(object)
            let transform = Self.transform(of: object)
            var corners: [SIMD3<Float>] = []
            for i in 0..<8 {
                let p = SIMD3(i & 1 == 0 ? low.x : high.x, i & 2 == 0 ? low.y : high.y, i & 4 == 0 ? low.z : high.z)
                let w = transform * SIMD4(p, 1)
                corners.append(SIMD3(w.x, w.y, w.z))
            }
            boxes += [0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7, 7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7].map { corners[$0] }
        }
        lines(boxes, SIMD4(1, 0.9, 0.2, 1))
        // A road's first point and a right click waiting for an object: a post with a cross.
        var points: [SIMD3<Float>] = []
        for point in [overlay.roadStart, overlay.pending].compactMap({ $0 }) {
            let ground = SIMD3(point.x, point.y, scene.groundHeight(x: point.x, y: point.y))
            points += [ground, ground + SIMD3(0, 0, 20), ground + SIMD3(-6, 0, 1), ground + SIMD3(6, 0, 1),
                       ground + SIMD3(0, -6, 1), ground + SIMD3(0, 6, 1)]
        }
        lines(points, SIMD4(0.3, 1, 0.5, 1))
    }
}

private func mapLookAt(eye: SIMD3<Float>, center: SIMD3<Float>, up: SIMD3<Float>) -> simd_float4x4 {
    let forward = simd_normalize(center - eye)
    let side = simd_normalize(simd_cross(forward, up))
    let upward = simd_cross(side, forward)
    return simd_float4x4(columns: (
        SIMD4(side.x, upward.x, -forward.x, 0),
        SIMD4(side.y, upward.y, -forward.y, 0),
        SIMD4(side.z, upward.z, -forward.z, 0),
        SIMD4(-simd_dot(side, eye), -simd_dot(upward, eye), simd_dot(forward, eye), 1)))
}

/// Right-handed perspective with Metal's 0...1 depth range.
private func mapPerspective(fovY: Float, aspect: Float, near: Float, far: Float) -> simd_float4x4 {
    let y = 1 / tan(fovY * 0.5)
    let x = y / aspect
    let z = far / (near - far)
    return simd_float4x4(columns: (
        SIMD4(x, 0, 0, 0),
        SIMD4(0, y, 0, 0),
        SIMD4(0, 0, z, -1),
        SIMD4(0, 0, z * near, 0)))
}

/// MTKView that turns mouse and key input into core commands (`map.press`, `map.drag`,
/// `map.release`, `map.click_right`, `edit.*`) with what needs the camera: the ground point under
/// the cursor, the objects under it and those inside a rubber band. What an input means (select,
/// move, place, pan) is the core's decision; the view moves its camera when the store's gesture
/// says "pan" or "orbit". Scroll and pinch zoom and the rotate gesture turn the camera directly.
final class MapMTKView: MTKView {
    var state: MapViewState?
    weak var renderer: MapRenderer?
    weak var editor: EditorModel?
    private var downPoint: NSPoint?
    private var lastPoint: NSPoint?
    private var dragged = false
    private let band = CAShapeLayer()
    private var tracking: NSTrackingArea?

    override var acceptsFirstResponder: Bool { true }

    /// A point of the view with the origin top left, as MapViewState takes it.
    private func viewPoint(_ point: NSPoint) -> CGPoint { CGPoint(x: point.x, y: bounds.height - point.y) }
    private func viewPoint(_ event: NSEvent) -> CGPoint { viewPoint(convert(event.locationInWindow, from: nil)) }

    private func ground(_ event: NSEvent) -> SIMD2<Float>? {
        guard let state, let renderer,
              let point = state.groundPoint(at: viewPoint(event), size: bounds.size, scene: renderer.scene) else { return nil }
        return SIMD2(point.x, point.y)
    }

    /// "x\ty" of the ground under the cursor, "\t" when it is off the map.
    private func groundArgument(_ event: NSEvent) -> String {
        guard let point = ground(event) else { return "\t" }
        return EditorModel.number(point.x) + "\t" + EditorModel.number(point.y)
    }

    private func hitsArgument(_ event: NSEvent) -> String {
        (renderer?.hits(at: viewPoint(event), size: bounds.size) ?? []).map(String.init).joined(separator: ",")
    }

    private static func modifiers(_ event: NSEvent) -> String {
        var names: [String] = []
        if event.modifierFlags.contains(.shift) { names.append("shift") }
        if event.modifierFlags.contains(.option) { names.append("alt") }
        if event.modifierFlags.contains(.command) { names.append("cmd") }
        return names.joined(separator: ",")
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        editor?.cursorGround = ground(event)  // where ⌘V pastes
    }

    override func mouseExited(with event: NSEvent) {
        editor?.cursorGround = nil
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        if event.modifierFlags.contains(.control) {
            rightMouseDown(with: event)
            return
        }
        downPoint = convert(event.locationInWindow, from: nil)
        lastPoint = downPoint
        dragged = false
        editor?.send("map.press\t" + groundArgument(event) + "\t" + Self.modifiers(event) + "\t" + hitsArgument(event))
    }

    override func mouseDragged(with event: NSEvent) {
        guard let state, let editor, let downPoint else { return }
        // Deltas from the locations, not event.deltaX/Y (not every event source fills those).
        let point = convert(event.locationInWindow, from: nil)
        let dx = Float(point.x - (lastPoint ?? point).x)
        let dy = Float((lastPoint ?? point).y - point.y)  // down is positive, like NSEvent.deltaY
        lastPoint = point
        if !dragged {
            dragged = hypot(point.x - downPoint.x, point.y - downPoint.y) > 3
            guard dragged else { return }
        }
        editor.send("map.drag\t" + groundArgument(event))
        switch editor.mapGesture.kind {
        case "pan": state.pan(dx: dx, dy: dy, viewHeight: Float(bounds.height))
        case "orbit": state.rotate(dx: dx, dy: dy)
        case "band": showBand(from: downPoint, to: point)
        default: break  // "move": the renderer draws the selection where the store says
        }
    }

    override func mouseUp(with event: NSEvent) {
        defer { downPoint = nil; lastPoint = nil; band.removeFromSuperlayer() }
        guard let editor, downPoint != nil else { return }
        var inside = ""
        if editor.mapGesture.kind == "band", let renderer, let downPoint {
            let point = convert(event.locationInWindow, from: nil)
            let a = viewPoint(downPoint), b = viewPoint(point)
            let rect = CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
            inside = renderer.objects(in: rect, size: bounds.size).map(String.init).joined(separator: ",")
        }
        editor.send("map.release\t" + inside)
    }

    private func showBand(from a: NSPoint, to b: NSPoint) {
        if band.superlayer == nil, let layer {
            band.fillColor = NSColor.controlAccentColor.withAlphaComponent(0.15).cgColor
            band.strokeColor = NSColor.controlAccentColor.cgColor
            band.lineWidth = 1
            layer.addSublayer(band)
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        band.frame = bounds
        band.path = CGPath(rect: CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y)),
                           transform: nil)
        CATransaction.commit()
    }

    override func rightMouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        editor?.send("map.click_right\t" + groundArgument(event) + "\t" + hitsArgument(event))
    }

    override func keyDown(with event: NSEvent) {
        guard let editor else { return super.keyDown(with: event) }
        switch event.keyCode {
        case 51, 117: editor.send("edit.delete")  // Delete, Forward Delete
        case 53: editor.send("edit.cancel")       // Esc
        default:
            switch event.charactersIgnoringModifiers {  // with ⇧ these are { and } on most layouts
            case "[": editor.send("edit.rotate\t-15")
            case "]": editor.send("edit.rotate\t15")
            case "{": editor.send("edit.rotate\t-1")
            case "}": editor.send("edit.rotate\t1")
            default: super.keyDown(with: event)
            }
        }
    }

    override func scrollWheel(with event: NSEvent) {
        state?.zoom(by: 1 + Float(event.scrollingDeltaY) * -0.01)
    }

    override func magnify(with event: NSEvent) {
        state?.zoom(by: 1 - Float(event.magnification))
    }

    override func rotate(with event: NSEvent) {
        state?.rotate(dx: Float(event.rotation) * 2, dy: 0)
    }
}

struct MapMetalView: NSViewRepresentable {
    let scene: MapScene
    let models: [MapModelData]
    let textures: [String: PreviewTexture]
    let editor: EditorModel
    @ObservedObject var state: MapViewState

    func makeCoordinator() -> MapRenderer? { MapRenderer(state: state) }

    func makeNSView(context: Context) -> NSView {
        guard let renderer = context.coordinator else {
            let label = NSTextField(labelWithString: "Metal is not available")
            label.alignment = .center
            return label
        }
        let view = MapMTKView(frame: .zero, device: renderer.device)
        view.state = state
        view.renderer = renderer
        view.editor = editor
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.clearColor = MTLClearColor(red: 0.12, green: 0.13, blue: 0.15, alpha: 1)
        view.preferredFramesPerSecond = 60
        view.delegate = renderer
        // Read every frame, outside SwiftUI: a drag changes it with every mouse move.
        renderer.overlaySource = { [weak editor] in editor?.mapOverlay ?? MapOverlay() }
        update(renderer)
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        if let renderer = context.coordinator { update(renderer) }
    }

    private func update(_ renderer: MapRenderer) {
        renderer.show(scene)
        if renderer.loadedModelCount < models.count { renderer.add(models: models, textures: textures) }
    }
}

/// The map with its controls, the placement bar and the selection's inspector.
struct MapScreen: View {
    @Environment(EditorModel.self) private var editor
    @StateObject private var state = MapViewState()

    var body: some View {
        VStack(spacing: 0) {
            ZStack {
                MapMetalView(scene: editor.mapScene, models: editor.mapModels, textures: editor.mapModelTextures,
                             editor: editor, state: state)
                switch editor.mapState {
                case "loading": ProgressView("Reading the map…").controlSize(.small)
                case "error": Text(editor.mapMessage).foregroundStyle(.secondary).padding()
                case "idle": Text("Choose a map").foregroundStyle(.secondary)
                default: EmptyView()
                }
            }
            PlacementBar().disabled(!editor.canEditMap)
            HStack(spacing: 12) {
                Toggle("Models", isOn: $state.showModels).toggleStyle(.checkbox)
                    .accessibilityIdentifier("map.showModels")
                Toggle("Markers", isOn: $state.showMarkers).toggleStyle(.checkbox)
                    .accessibilityIdentifier("map.showMarkers")
                    .help("Waypoints and their links, roads, sounds and objects whose model is missing")
                Toggle("Water", isOn: $state.showWater).toggleStyle(.checkbox)
                    .accessibilityIdentifier("map.showWater")
                Spacer()
                if editor.mapModelCount > 0 && editor.mapModelsLoaded < editor.mapModelCount {
                    ProgressView(value: Double(editor.mapModelsLoaded), total: Double(editor.mapModelCount))
                        .frame(width: 100)
                    Text("Models \(editor.mapModelsLoaded)/\(editor.mapModelCount)").font(.caption)
                }
                Text(editor.mapMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    .help(editor.mapWarnings.joined(separator: "\n"))
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            Text("Drag to pan, ⌥-drag to turn, scroll or pinch to zoom, click to select, ⇧-click or ⇧-drag to add. "
                 + "Right-click places, drag the selection to move it, [ ] turn it, Delete removes it, "
                 + "⌘C ⌘V ⌘D copy, paste, duplicate, ⌘Z undoes")
                .font(.caption2).foregroundStyle(.tertiary).padding(.bottom, 4)
        }
        .inspector(isPresented: Binding(get: { !editor.mapSelection.isEmpty },
                                        set: { if !$0 { editor.send("selection.set\t") } })) {
            MapObjectInspector()
                .inspectorColumnWidth(min: 280, ideal: 340)
        }
    }
}

/// The placement tool (object, waypoint, road, bridge) with what it places; undo, redo, save and
/// the core's note.
struct PlacementBar: View {
    @Environment(EditorModel.self) private var editor

    var body: some View {
        let state = editor.mapEditor
        HStack(spacing: 10) {
            Picker("Place", selection: Binding(get: { state.tool }, set: { editor.chooseTool($0) })) {
                Text("Object").tag("object")
                Text("Waypoint").tag("waypoint")
                Text("Road").tag("road")
                Text("Bridge").tag("bridge")
            }
            .pickerStyle(.segmented)
            .fixedSize()
            .accessibilityIdentifier("placement.tool")
            .help("What a right click on the map places")
            switch state.tool {
            case "object":
                Button(state.templateName.isEmpty ? "Choose an object…" : state.templateName) { editor.send("placement.pick") }
                    .accessibilityIdentifier("placement.template")
                    .popover(isPresented: Binding(get: { state.picker },
                                                  set: { if !$0 && editor.mapEditor.picker { editor.send("placement.cancel") } }),
                             arrowEdge: .top) {
                        TemplatePicker { name in editor.send("placement.template\t" + name) }
                    }
                Picker("Owner", selection: Binding(get: { state.team }, set: { editor.send("placement.owner\t" + $0) })) {
                    ForEach(editor.mapPlayers) { player in Text(player.label).tag(player.team) }
                }
                .frame(maxWidth: 200)
                .accessibilityIdentifier("placement.owner")
                .help("Placed objects belong to this player's default team")
                Stepper(value: Binding(get: { state.angle }, set: { editor.send("placement.angle\t\($0)") }),
                        in: -180...180, step: 15) {
                    Text(String(format: "%.0f°", state.angle)).monospacedDigit().frame(width: 40, alignment: .trailing)
                }
                .help("Angle of placed objects")
                .accessibilityIdentifier("placement.angle")
            case "road", "bridge":
                let bridge = state.tool == "bridge"
                Picker(bridge ? "Bridge" : "Road", selection: Binding(get: { state.roadType }, set: {
                    editor.send((bridge ? "placement.bridge\t" : "placement.road\t") + $0)
                })) {
                    ForEach(editor.roadTypes.filter { $0.bridge == bridge }) { type in Text(type.name).tag(type.name) }
                }
                .frame(maxWidth: 260)
                .accessibilityIdentifier("placement.road")
                Text(state.roadStart == nil ? "Right-click the start, then the end" : "Right-click the end, Esc cancels")
                    .font(.caption).foregroundStyle(.secondary)
            default:
                Text("Right-click places a waypoint linked from the selected one; right-click a waypoint to link it")
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            Button { editor.send("map.undo") } label: { Image(systemName: "arrow.uturn.backward") }
                .disabled(editor.mapEdit.undoCount == 0).help("Undo (⌘Z)")
                .accessibilityIdentifier("placement.undo")
            Button { editor.send("map.redo") } label: { Image(systemName: "arrow.uturn.forward") }
                .disabled(editor.mapEdit.redoCount == 0).help("Redo (⇧⌘Z)")
                .accessibilityIdentifier("placement.redo")
            Button("Save As…") { editor.saveMapAs() }.help("Save the map with its edits and preview (⇧⌘S)")
                .accessibilityIdentifier("placement.saveAs")
            if editor.mapEdit.edited { Text("Edited").font(.caption).foregroundStyle(.orange) }
            if !state.note.isEmpty {
                Text(state.note).font(.caption)
                    .foregroundStyle(state.noteKind == "error" ? .red : .secondary)
                    .lineLimit(1).truncationMode(.middle).help(state.note)
                    .accessibilityIdentifier("placement.note")
            }
        }
        .padding(.horizontal, 10)
        .padding(.top, 6)
    }
}

/// A searchable list of the object templates to place.
struct TemplatePicker: View {
    @Environment(EditorModel.self) private var editor
    @State private var search = ""
    let choose: (String) -> Void

    private var shown: [ObjectItem] {
        let all = editor.objects
        guard !search.isEmpty else { return all }
        return all.filter { $0.name.localizedCaseInsensitiveContains(search) || $0.side.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
        VStack(spacing: 6) {
            TextField("Name or side", text: $search).textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("picker.search")
            List(shown) { item in
                Button { choose(item.name) } label: {
                    HStack {
                        Text(item.name)
                        Spacer()
                        Text(item.side).foregroundStyle(.secondary).font(.caption)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("picker.item." + item.name)
            }
            .frame(width: 340, height: 420)
        }
        .padding(8)
    }
}

/// A text field that sends its value when Return is pressed and shows the store's value otherwise.
struct CommitField: View {
    let title: String
    let value: String
    let identifier: String
    let commit: (String) -> Void
    @State private var text = ""

    var body: some View {
        TextField(title, text: $text)
            .multilineTextAlignment(.trailing)
            .onAppear { text = value }
            .onChange(of: value) { _, fresh in text = fresh }
            .onSubmit { commit(text) }
            .accessibilityIdentifier(identifier)
    }
}

/// The selection: one object with every field, or several with what they share. Every change is
/// a selection.* command; the core applies it to all of them as one undo step.
struct MapObjectInspector: View {
    @Environment(EditorModel.self) private var editor

    var body: some View {
        let objects = editor.mapSelection.compactMap { id in editor.mapScene.objects.first { $0.id == id } }
        if let first = objects.first {
            SelectionForm(objects: objects, properties: editor.mapObjectProperties(of: first.id),
                          others: objects.dropFirst().map { editor.mapObjectProperties(of: $0.id) })
        } else {
            Text("No object selected").foregroundStyle(.secondary)
        }
    }
}

/// The inspector's form for the selected objects (the first one's properties, and the others').
private struct SelectionForm: View {
    @Environment(EditorModel.self) private var editor
    let objects: [MapObjectItem]
    let properties: [MapObjectProperty]
    let others: [[MapObjectProperty]]
    @State private var newKey = ""
    @State private var newType = "string"
    @State private var newValue = ""

    private static let flags: [(String, String)] = [
        ("objectEnabled", "Enabled"), ("objectIndestructible", "Indestructible"), ("objectUnsellable", "Unsellable"),
        ("objectPowered", "Powered"), ("objectTargetable", "Targetable"), ("objectRecruitableAI", "Recruitable by AI"),
    ]

    /// A property every selected object has with the same value.
    private func shared(_ key: String) -> MapObjectProperty? {
        guard let mine = properties.first(where: { $0.key == key }) else { return nil }
        return others.allSatisfy({ $0.contains(mine) }) ? mine : nil
    }

    private func set(_ key: String, _ type: String, _ value: String) {
        editor.send("selection.property\t\(key)\t\(type)\t\(value)")
    }

    var body: some View {
        let first = objects[0]
        let single = objects.count == 1
        Form {
            Section(single ? first.templateName : "\(objects.count) objects") {
                if single {
                    LabeledContent("Kind", value: first.kind)
                    LabeledContent("Position", value: String(format: "%.1f, %.1f (ground + %.1f)", first.position.x,
                                                              first.position.y, first.height))
                    if let model = editor.mapModels.first(where: { $0.id == first.modelId }) {
                        LabeledContent("Model", value: model.ready ? model.name : "\(model.name) (\(model.message))")
                    }
                    if first.flags != 0 { LabeledContent("Flags", value: String(format: "0x%X", first.flags)) }
                }
                LabeledContent("Angle") {
                    HStack(spacing: 4) {
                        CommitField(title: "Angle", value: objects.allSatisfy { $0.angle == first.angle }
                                    ? Self.degrees(first.angle) : "", identifier: "inspector.angle") {
                            editor.send("selection.angle\t" + $0.replacingOccurrences(of: ",", with: "."))
                        }
                        .frame(width: 60)
                        Text("°")
                        Stepper("", onIncrement: { editor.send("edit.rotate\t15") },
                                onDecrement: { editor.send("edit.rotate\t-15") }).labelsHidden()
                            .accessibilityIdentifier("inspector.angleStep")
                    }
                }
                LabeledContent("Height") {
                    CommitField(title: "Height", value: objects.allSatisfy { $0.height == first.height }
                                ? String(format: "%.1f", first.height) : "", identifier: "inspector.height") {
                        editor.send("selection.height\t" + $0.replacingOccurrences(of: ",", with: "."))
                    }
                    .frame(width: 80)
                }
                Picker("Owner", selection: Binding(get: { objects.allSatisfy { $0.owner == first.owner } ? first.owner : "" },
                                                   set: { editor.send("selection.owner\t" + $0) })) {
                    if !objects.allSatisfy({ $0.owner == first.owner }) { Text("Several").tag("") }
                    ForEach(editor.mapPlayers) { player in Text(player.label).tag(player.team) }
                    if !first.owner.isEmpty && !editor.mapPlayers.contains(where: { $0.team == first.owner }) {
                        Text(first.owner).tag(first.owner)
                    }
                }
                .accessibilityIdentifier("inspector.owner")
                LabeledContent("Name") {
                    CommitField(title: "Name", value: shared("objectName")?.value ?? "", identifier: "inspector.name") {
                        set("objectName", shared("objectName")?.type == "unicode" ? "unicode" : "string", $0)
                    }
                }
                LabeledContent("Script") {
                    CommitField(title: "Script", value: shared("objectScriptAttachment")?.value ?? "",
                                identifier: "inspector.script") { set("objectScriptAttachment", "string", $0) }
                }
                LabeledContent("Health %") {
                    CommitField(title: "Health", value: shared("objectInitialHealth")?.value ?? "",
                                identifier: "inspector.health") { set("objectInitialHealth", "int", $0) }
                    .frame(width: 60)
                }
                ForEach(Self.flags, id: \.0) { key, label in
                    Toggle(label, isOn: Binding(get: { shared(key)?.value == "true" },
                                                set: { set(key, "bool", $0 ? "true" : "false") }))
                        .accessibilityIdentifier("inspector.flag." + key)
                }
                HStack {
                    if objects.count == 2 && objects.allSatisfy({ $0.kind == "waypoint" }) {
                        Button("Link Waypoints") { editor.send("waypoint.link") }
                            .accessibilityIdentifier("inspector.link")
                    }
                    Button("Delete", role: .destructive) { editor.send("edit.delete") }
                        .accessibilityIdentifier("inspector.delete")
                }
            }
            Section(single ? "Properties" : "Properties of \(first.templateName)") {
                ForEach(properties, id: \.self) { property in
                    LabeledContent(property.key) {
                        HStack(spacing: 4) {
                            CommitField(title: property.type, value: property.value,
                                        identifier: "inspector.property." + property.key) {
                                set(property.key, property.type, $0)
                            }
                            .help(property.type)
                            Button { editor.send("selection.unset\t" + property.key) } label: {
                                Image(systemName: "minus.circle")
                            }
                            .buttonStyle(.borderless).help("Remove \(property.key)")
                            .accessibilityIdentifier("inspector.remove." + property.key)
                        }
                    }
                }
                HStack(spacing: 4) {
                    TextField("key", text: $newKey).frame(width: 110).accessibilityIdentifier("inspector.newKey")
                    Picker("", selection: $newType) {
                        ForEach(["string", "int", "real", "bool", "unicode"], id: \.self) { Text($0).tag($0) }
                    }
                    .labelsHidden().frame(width: 80)
                    .accessibilityIdentifier("inspector.newType")
                    TextField("value", text: $newValue).accessibilityIdentifier("inspector.newValue")
                    Button("Add") {
                        set(newKey, newType, newValue)
                        newKey = ""
                        newValue = ""
                    }
                    .disabled(newKey.isEmpty)
                    .accessibilityIdentifier("inspector.add")
                }
            }
        }
        .formStyle(.grouped)
    }

    private static func degrees(_ radians: Float) -> String { String(format: "%.1f", radians * 180 / .pi) }
}

/// Map list on the left, the map on the right.
struct MapBrowser: View {
    @Environment(EditorModel.self) private var editor
    @State private var search = ""
    @State private var selection: MapListItem.ID?

    private var shown: [MapListItem] {
        editor.maps.filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
        NavigationSplitView {
            List(shown, selection: $selection) { item in
                VStack(alignment: .leading) {
                    Text(item.name)
                    Text(item.source).font(.caption2).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                }
                .tag(item.id)
                .accessibilityIdentifier("maps.item." + item.name)
            }
            .searchable(text: $search, placement: .sidebar, prompt: "Map name")
            .navigationSplitViewColumnWidth(min: 200, ideal: 260)
            .onChange(of: selection) { _, id in
                if let item = editor.maps.first(where: { $0.id == id }) { editor.openMap(item.path) }
            }
        } detail: {
            MapScreen()
        }
    }
}
