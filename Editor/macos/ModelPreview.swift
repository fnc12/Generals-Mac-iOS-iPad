// Model preview: draws what the core put into the model_* tables with Metal. The core already
// resolved the skeleton, so the renderer only multiplies each vertex by its bone's matrix for the
// current frame (the `pose` blob holds them for every frame).
import AppKit
import MetalKit
import SwiftUI
import UniformTypeIdentifiers
import simd

struct PreviewPart {
    let mesh: String
    let texture: String
    let bone: Int
    let skinned: Bool
    let hidden: Bool
    let blend: Int      // 0 opaque, 1 alpha test, 2 alpha blend, 3 additive, 4 multiply
    let houseColor: Int // 0 none, 1 texture alpha marks player color, 2 whole part tinted
    let vertices: Data  // 36-byte records
    let indices: Data   // uint32
}

struct PreviewTexture {
    let width: Int
    let height: Int
    let rgba: Data
}

/// Everything the renderer needs for one model and one animation.
struct PreviewModel {
    var name = ""
    var revision: Int64 = -1
    var boneCount = 0
    var animation = ""
    var frameCount = 1
    var frameRate = 30
    var pose = Data()     // frameCount * boneCount * 16 floats
    var visible = Data()  // frameCount * boneCount bytes
    var boundsMin = SIMD3<Float>(-1, -1, -1)
    var boundsMax = SIMD3<Float>(1, 1, 1)
    var parts: [PreviewPart] = []
    var textures: [String: PreviewTexture] = [:]
    /// Changes when parts/textures change (not when only the animation does).
    var geometryKey = ""
}

/// Playback state shared by the SwiftUI controls and the renderer.
final class PreviewPlayback: ObservableObject {
    @Published var playing = true
    @Published var showHidden = false
    var startTime = CACurrentMediaTime()
    var pausedFrame = 0
    var yaw: Float = .pi * 0.75
    var pitch: Float = 0.45
    var zoom: Float = 1
    var houseColor = SIMD4<Float>(0.16, 0.38, 0.95, 1)  // the preview's player color

    func restart() {
        startTime = CACurrentMediaTime()
        pausedFrame = 0
    }

    func frame(count: Int, rate: Int) -> Int {
        guard count > 1 else { return 0 }
        if !playing { return min(pausedFrame, count - 1) }
        let elapsed = CACurrentMediaTime() - startTime
        return Int(elapsed * Double(max(rate, 1))) % count
    }
}

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    uint bone [[attribute(3)]];
};

struct Uniforms {
    float4x4 viewProjection;
    float4 lightDirection;
    float4 baseColor;
    uint4 flags;  // x: textured, y: alpha test, z: lit, w: player color (1 by texture alpha, 2 tint)
    float4 houseColor;
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
};

vertex VertexOut previewVertex(VertexIn in [[stage_in]],
                               constant Uniforms &uniforms [[buffer(1)]],
                               device const float4x4 *bones [[buffer(2)]]) {
    float4x4 bone = bones[in.bone];
    VertexOut out;
    out.position = uniforms.viewProjection * (bone * float4(in.position, 1.0));
    out.normal = (bone * float4(in.normal, 0.0)).xyz;
    out.uv = in.uv;
    return out;
}

fragment float4 previewFragment(VertexOut in [[stage_in]],
                                bool frontFacing [[front_facing]],
                                constant Uniforms &uniforms [[buffer(1)]],
                                texture2d<float> colorTexture [[texture(0)]],
                                sampler colorSampler [[sampler(0)]]) {
    float4 color = uniforms.baseColor;
    if (uniforms.flags.x != 0) {
        color = colorTexture.sample(colorSampler, in.uv);
    }
    // In unit textures alpha marks the team color area, so it only cuts holes when the
    // mesh's shader asks for alpha testing.
    if (uniforms.flags.w == 1) {
        color = float4(mix(uniforms.houseColor.rgb, color.rgb, color.a), 1.0);
    } else if (uniforms.flags.w == 2) {
        color.rgb *= uniforms.houseColor.rgb;
    }
    if (uniforms.flags.y != 0 && color.a < 0.375) discard_fragment();
    float light = 1.0;
    if (uniforms.flags.z != 0) {
        light = 0.45 + 0.55 * abs(dot(normalize(in.normal), normalize(uniforms.lightDirection.xyz)));
    }
    return float4(color.rgb * light, color.a);
}
"""

private struct Uniforms {
    var viewProjection: simd_float4x4
    var lightDirection: SIMD4<Float>
    var baseColor: SIMD4<Float>
    var flags: SIMD4<UInt32>  // textured, alpha test, lit, player color
    var houseColor: SIMD4<Float>
}

final class PreviewRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipelines: [Int: MTLRenderPipelineState] = [:]  // by blend mode
    private let depthState: MTLDepthStencilState
    private let blendDepthState: MTLDepthStencilState
    private let sampler: MTLSamplerState
    let playback: PreviewPlayback

    private struct GPUPart {
        let vertices: MTLBuffer
        let indices: MTLBuffer
        let indexCount: Int
        let texture: MTLTexture?
        let bone: Int
        let skinned: Bool
        let hidden: Bool
        let blend: Int
        let houseColor: Int
    }
    private var parts: [GPUPart] = []
    private var poseBuffer: MTLBuffer?
    private var model = PreviewModel()
    private var geometryKey = ""
    private var poseKey: Int64 = -2

    init?(playback: PreviewPlayback) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        self.playback = playback
        do {
            let library = try device.makeLibrary(source: shaderSource, options: nil)
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: "previewVertex")
            descriptor.fragmentFunction = library.makeFunction(name: "previewFragment")
            descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
            descriptor.depthAttachmentPixelFormat = .depth32Float
            let layout = MTLVertexDescriptor()
            let formats: [(MTLVertexFormat, Int)] = [(.float3, 0), (.float3, 12), (.float2, 24), (.uint, 32)]
            for (index, (format, offset)) in formats.enumerated() {
                layout.attributes[index].format = format
                layout.attributes[index].offset = offset
                layout.attributes[index].bufferIndex = 0
            }
            layout.layouts[0].stride = 36
            descriptor.vertexDescriptor = layout
            for blend in 0...4 {
                let attachment = descriptor.colorAttachments[0]!
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
                pipelines[blend] = try device.makeRenderPipelineState(descriptor: descriptor)
            }
        } catch {
            NSLog("END Editor: preview shaders failed: \(error)")
            return nil
        }
        let depth = MTLDepthStencilDescriptor()
        depth.depthCompareFunction = .less
        depth.isDepthWriteEnabled = true
        guard let depthState = device.makeDepthStencilState(descriptor: depth) else { return nil }
        self.depthState = depthState
        depth.isDepthWriteEnabled = false  // see-through parts are drawn last and do not hide others
        guard let blendDepthState = device.makeDepthStencilState(descriptor: depth) else { return nil }
        self.blendDepthState = blendDepthState
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.mipFilter = .linear
        samplerDescriptor.sAddressMode = .repeat
        samplerDescriptor.tAddressMode = .repeat
        guard let sampler = device.makeSamplerState(descriptor: samplerDescriptor) else { return nil }
        self.sampler = sampler
        super.init()
    }

    func show(_ newModel: PreviewModel) {
        if newModel.geometryKey != geometryKey {
            geometryKey = newModel.geometryKey
            parts = uploadParts(newModel)
        }
        if newModel.revision != poseKey {
            poseKey = newModel.revision
            poseBuffer = newModel.pose.isEmpty ? nil : newModel.pose.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
            }
            if newModel.animation != model.animation || newModel.name != model.name { playback.restart() }
        }
        model = newModel
    }

    private func uploadParts(_ model: PreviewModel) -> [GPUPart] {
        var textures: [String: MTLTexture] = [:]
        for (name, image) in model.textures where image.width > 0 && image.height > 0 {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: image.width, height: image.height, mipmapped: true)
            descriptor.usage = .shaderRead
            guard let texture = device.makeTexture(descriptor: descriptor) else { continue }
            image.rgba.withUnsafeBytes {
                texture.replace(region: MTLRegionMake2D(0, 0, image.width, image.height), mipmapLevel: 0,
                                withBytes: $0.baseAddress!, bytesPerRow: image.width * 4)
            }
            textures[name] = texture
        }
        if let commands = queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() {
            for texture in textures.values { blit.generateMipmaps(for: texture) }
            blit.endEncoding()
            commands.commit()
        }
        return model.parts.compactMap { part in
            guard !part.vertices.isEmpty, !part.indices.isEmpty,
                  let vertices = part.vertices.withUnsafeBytes({
                      device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
                  }),
                  let indices = part.indices.withUnsafeBytes({
                      device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
                  })
            else { return nil }
            return GPUPart(vertices: vertices, indices: indices, indexCount: part.indices.count / 4,
                           texture: textures[part.texture], bone: part.bone, skinned: part.skinned,
                           hidden: part.hidden, blend: part.blend, houseColor: part.houseColor)
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }
        let size = view.drawableSize
        encode(encoder, aspect: Float(size.width / max(size.height, 1)),
               frame: playback.frame(count: model.frameCount, rate: model.frameRate))
        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }

    /// Renders one frame into an image without a window (for `--snapshot` and GIF export).
    /// `supersample` > 1 draws at that many times the size and scales down, smoothing the edges.
    func renderImage(width: Int, height: Int, frame: Int, supersample: Int = 1) -> CGImage? {
        guard supersample > 1 else { return renderTexture(width: width, height: height, frame: frame) }
        guard let large = renderTexture(width: width * supersample, height: height * supersample, frame: frame),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                                          | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        context.interpolationQuality = .high
        context.draw(large, in: CGRect(x: 0, y: 0, width: width, height: height))
        return context.makeImage()
    }

    private func renderTexture(width: Int, height: Int, frame: Int) -> CGImage? {
        let colorDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        colorDescriptor.usage = [.renderTarget, .shaderRead]
        colorDescriptor.storageMode = .shared
        let depthDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .depth32Float, width: width, height: height, mipmapped: false)
        depthDescriptor.usage = .renderTarget
        depthDescriptor.storageMode = .private
        guard let color = device.makeTexture(descriptor: colorDescriptor),
              let depth = device.makeTexture(descriptor: depthDescriptor),
              let commands = queue.makeCommandBuffer() else { return nil }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = color
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.16, green: 0.17, blue: 0.19, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 1
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return nil }
        encode(encoder, aspect: Float(width) / Float(height), frame: frame)
        encoder.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        color.getBytes(&pixels, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue
                                | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: false, intent: .defaultIntent)
    }

    private func encode(_ encoder: MTLRenderCommandEncoder, aspect: Float, frame requestedFrame: Int) {
        guard !parts.isEmpty, let poseBuffer, model.boneCount > 0 else { return }

        let frame = min(max(requestedFrame, 0), model.frameCount - 1)
        let boneBytes = model.boneCount * 64
        guard (frame + 1) * boneBytes <= poseBuffer.length else { return }

        encoder.setCullMode(.none)
        encoder.setFragmentSamplerState(sampler, index: 0)
        encoder.setVertexBuffer(poseBuffer, offset: frame * boneBytes, index: 2)

        var uniforms = Uniforms(viewProjection: viewProjection(aspect: aspect),
                                lightDirection: SIMD4(0.4, -0.5, 0.75, 0),
                                baseColor: SIMD4(0.62, 0.64, 0.66, 1), flags: .zero,
                                houseColor: playback.houseColor)
        // Solid parts first, then the see-through ones (glows, smoke) over them.
        let ordered = parts.filter { $0.blend <= 1 } + parts.filter { $0.blend > 1 }
        for part in ordered {
            if part.hidden && !playback.showHidden { continue }
            if !part.skinned, isHidden(bone: part.bone, frame: frame) { continue }
            guard let pipeline = pipelines[part.blend] ?? pipelines[0] else { continue }
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(part.blend > 1 ? blendDepthState : depthState)
            uniforms.flags = SIMD4(part.texture == nil ? 0 : 1, part.blend == 1 || part.blend == 2 ? 1 : 0,
                                   part.blend <= 2 ? 1 : 0, UInt32(part.houseColor))
            encoder.setVertexBuffer(part.vertices, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentTexture(part.texture, index: 0)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: part.indexCount, indexType: .uint32,
                                          indexBuffer: part.indices, indexBufferOffset: 0)
        }
    }

    private func isHidden(bone: Int, frame: Int) -> Bool {
        let index = frame * model.boneCount + bone
        guard index < model.visible.count else { return false }
        return model.visible[model.visible.startIndex + index] == 0
    }

    /// Orbit camera around the model's bind-pose bounds; the game's world has Z up.
    private func viewProjection(aspect: Float) -> simd_float4x4 {
        let center = (model.boundsMin + model.boundsMax) * 0.5
        let radius = max(simd_length(model.boundsMax - model.boundsMin) * 0.5, 0.01)
        let distance = radius * 2.6 * playback.zoom
        let eye = center + distance * SIMD3(cos(playback.pitch) * cos(playback.yaw),
                                            cos(playback.pitch) * sin(playback.yaw),
                                            sin(playback.pitch))
        let view = lookAt(eye: eye, center: center, up: SIMD3(0, 0, 1))
        let projection = perspective(fovY: 0.75, aspect: aspect, near: max(distance - radius * 3, distance * 0.01),
                                     far: distance + radius * 3)
        return projection * view
    }
}

private func lookAt(eye: SIMD3<Float>, center: SIMD3<Float>, up: SIMD3<Float>) -> simd_float4x4 {
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
private func perspective(fovY: Float, aspect: Float, near: Float, far: Float) -> simd_float4x4 {
    let y = 1 / tan(fovY * 0.5)
    let x = y / aspect
    let z = far / (near - far)
    return simd_float4x4(columns: (
        SIMD4(x, 0, 0, 0),
        SIMD4(0, y, 0, 0),
        SIMD4(0, 0, z, -1),
        SIMD4(0, 0, z * near, 0)))
}

/// MTKView that turns drags into orbiting and scrolling into zoom.
final class OrbitMTKView: MTKView {
    var playback: PreviewPlayback?

    override var acceptsFirstResponder: Bool { true }

    private var lastPoint: NSPoint?

    override func mouseDown(with event: NSEvent) {
        lastPoint = event.locationInWindow
    }

    // Deltas from the locations, not event.deltaX/Y (not every event source fills those).
    override func mouseDragged(with event: NSEvent) {
        guard let playback else { return }
        let point = event.locationInWindow
        let last = lastPoint ?? point
        lastPoint = point
        playback.yaw -= Float(point.x - last.x) * 0.01
        playback.pitch = min(max(playback.pitch + Float(last.y - point.y) * 0.01, -1.5), 1.5)
    }

    override func mouseUp(with event: NSEvent) {
        lastPoint = nil
    }

    override func scrollWheel(with event: NSEvent) {
        guard let playback else { return }
        playback.zoom = min(max(playback.zoom * (1 + Float(event.scrollingDeltaY) * -0.01), 0.1), 10)
    }

    override func magnify(with event: NSEvent) {
        guard let playback else { return }
        playback.zoom = min(max(playback.zoom * (1 - Float(event.magnification)), 0.1), 10)
    }
}

struct MetalPreview: NSViewRepresentable {
    let model: PreviewModel
    @ObservedObject var playback: PreviewPlayback

    func makeCoordinator() -> PreviewRenderer? { PreviewRenderer(playback: playback) }

    func makeNSView(context: Context) -> NSView {
        guard let renderer = context.coordinator else {
            let label = NSTextField(labelWithString: "Metal is not available")
            label.alignment = .center
            return label
        }
        let view = OrbitMTKView(frame: .zero, device: renderer.device)
        view.playback = playback
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.clearColor = MTLClearColor(red: 0.16, green: 0.17, blue: 0.19, alpha: 1)
        view.preferredFramesPerSecond = 60
        view.delegate = renderer
        renderer.show(model)
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        context.coordinator?.show(model)
    }
}

/// The preview with its controls, for the detail pane.
struct ModelPreviewPanel: View {
    @Environment(EditorModel.self) private var editor
    @StateObject private var playback = PreviewPlayback()
    @State private var exporting = false
    @State private var exportMessage = ""

    var body: some View {
        let preview = editor.preview
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                MetalPreview(model: preview, playback: playback)
                switch editor.modelState {
                case "loading": ProgressView().controlSize(.small)
                case "error": Text(editor.modelMessage).foregroundStyle(.secondary).padding()
                default: EmptyView()
                }
            }
            .frame(minHeight: 260)
            .clipShape(RoundedRectangle(cornerRadius: 6))

            HStack {
                Picker("Animation", selection: Binding(
                    get: { preview.animation },
                    set: { editor.playAnimation($0) })) {
                    Text("Bind pose").tag("")
                    ForEach(editor.animations, id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden()
                .disabled(editor.animations.isEmpty)
                .accessibilityIdentifier("preview.animation")
                Button {
                    if playback.playing {
                        playback.pausedFrame = playback.frame(count: preview.frameCount, rate: preview.frameRate)
                    } else {
                        playback.startTime = CACurrentMediaTime()
                            - Double(playback.pausedFrame) / Double(max(preview.frameRate, 1))
                    }
                    playback.playing.toggle()
                } label: {
                    Image(systemName: playback.playing ? "pause.fill" : "play.fill")
                }
                .disabled(preview.frameCount <= 1)
                .accessibilityIdentifier("preview.playPause")
                Menu {
                    ForEach(GifFormat.allCases) { format in
                        Button(format.title) { exportGif(preview, format: format) }
                    }
                } label: {
                    Label("Export GIF", systemImage: "square.and.arrow.up")
                }
                .fixedSize()
                .help("One cycle of the selected animation, from the current view")
                .disabled(preview.frameCount <= 1 || exporting)
                .accessibilityIdentifier("preview.exportGif")
                if exporting { ProgressView().controlSize(.small) }
            }
            if !exportMessage.isEmpty {
                Text(exportMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            HStack {
                Toggle("Hidden parts", isOn: $playback.showHidden).toggleStyle(.checkbox)
                    .accessibilityIdentifier("preview.hiddenParts")
                Spacer()
                Text(editor.modelMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Text("Drag to turn, scroll to zoom").font(.caption2).foregroundStyle(.tertiary)
        }
        .onChange(of: preview.animation) { exportMessage = "" }
    }

    private func exportGif(_ model: PreviewModel, format: GifFormat) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.gif]
        panel.nameFieldStringValue = GifExport.suggestedName(object: model.name, animation: model.animation)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let camera = playback.exportCamera()
        exporting = true
        exportMessage = "Exporting \(model.frameCount) frames…"
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try GifExport.write(model, camera: camera, format: format, to: url) }
            DispatchQueue.main.async {
                exporting = false
                switch result {
                case .success:
                    let bytes = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
                    exportMessage = "Saved \(url.lastPathComponent), "
                        + ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
                case .failure(let error):
                    exportMessage = "GIF export failed: \(error)"
                }
            }
        }
    }
}
