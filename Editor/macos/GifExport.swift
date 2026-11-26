// GIF export of the preview: one full cycle of the current animation, seen from the preview's
// current camera, for posting outside the editor.
import AppKit
import ImageIO
import UniformTypeIdentifiers

enum GifFormat: String, CaseIterable, Identifiable {
    case square, wide

    var id: String { rawValue }
    var size: (width: Int, height: Int) { self == .square ? (800, 800) : (1280, 720) }
    var title: String { self == .square ? "Square 800×800" : "Wide 16:9 1280×720" }
}

enum GifExport {
    enum Failure: Error, CustomStringConvertible {
        case noAnimation, noMetal, renderFailed(frame: Int), writeFailed

        var description: String {
            switch self {
            case .noAnimation: return "the model has no animation to export"
            case .noMetal: return "Metal is not available"
            case .renderFailed(let frame): return "frame \(frame) could not be rendered"
            case .writeFailed: return "the GIF file could not be written"
            }
        }
    }

    /// Every frame 0..<frameCount (the preview loops the same way), looping forever.
    static func write(_ model: PreviewModel, camera: PreviewPlayback, format: GifFormat, to url: URL) throws {
        guard model.frameCount > 1 else { throw Failure.noAnimation }
        guard let renderer = PreviewRenderer(playback: camera) else { throw Failure.noMetal }
        renderer.show(model)
        guard let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.gif.identifier as CFString,
                                                                model.frameCount, nil) else { throw Failure.writeFailed }
        CGImageDestinationSetProperties(destination, [
            kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0],
        ] as CFDictionary)
        let rate = Double(max(model.frameRate, 1))
        for frame in 0..<model.frameCount {
            guard let image = renderer.renderImage(width: format.size.width, height: format.size.height,
                                                   frame: frame, supersample: 2) else {
                throw Failure.renderFailed(frame: frame)
            }
            // GIF delays are whole hundredths of a second: round the frame's end time, not its
            // length, so the cycle keeps the animation's real duration (30 fps -> 3, 3, 4, ...).
            let start = (Double(frame) * 100 / rate).rounded()
            let end = (Double(frame + 1) * 100 / rate).rounded()
            let delay = max(end - start, 2) / 100
            CGImageDestinationAddImage(destination, image, [
                kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: delay,
                                                kCGImagePropertyGIFUnclampedDelayTime: delay],
            ] as CFDictionary)
        }
        guard CGImageDestinationFinalize(destination) else { throw Failure.writeFailed }
    }

    /// File name like "AmericaTankCrusader_ATCRSR_SKN.ATCRSR_STAND.gif" -> without characters Finder dislikes.
    static func suggestedName(object: String, animation: String) -> String {
        let raw = [object, animation].filter { !$0.isEmpty }.joined(separator: "_")
        let cleaned = raw.map { $0.isLetter || $0.isNumber || $0 == "_" || $0 == "-" ? $0 : "_" }
        return String(cleaned) + ".gif"
    }
}

extension PreviewPlayback {
    /// A paused copy with the same camera and display options, so an export running in the
    /// background is not affected by the user turning the live preview meanwhile.
    func exportCamera() -> PreviewPlayback {
        let copy = PreviewPlayback()
        copy.playing = false
        copy.showHidden = showHidden
        copy.yaw = yaw
        copy.pitch = pitch
        copy.zoom = zoom
        copy.houseColor = houseColor
        return copy
    }
}
