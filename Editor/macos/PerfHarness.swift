// `END Editor --perf <zh> <base> <map path> <out.txt>` measures how smooth the real window is:
// it opens the game and the map in the normal UI, waits for every model, then drives the window
// from inside the app (mouse events through its own event queue, list scrolling through the
// clip view; no Accessibility permission, nothing outside the app is touched) and records, per
// scenario, the display frame intervals seen by a display link on the window, the map view's
// frame intervals, EditorModel changes and how long the main thread was busy.
// Scenarios: idle (nothing happens), pan (a left-button drag over the map), map list and object
// table (scrolling them down and up). For a machine nobody is watching (the gui-sandbox VM).
import AppKit
import Combine
import QuartzCore

/// Collects frame and main-thread timings while a scenario runs. `shared` is nil unless --perf.
final class PerfRecorder: NSObject {
    static var shared: PerfRecorder?

    private var recording = false
    private var vsyncs: [CFTimeInterval] = []
    private var mapFrames: [CFTimeInterval] = []
    private var mapEncode: [CFTimeInterval] = []
    private var busy: [CFTimeInterval] = []
    private var busyStart: CFTimeInterval?
    private var observer: CFRunLoopObserver?
    private var displayLink: CADisplayLink?
    /// EditorModel.objectWillChange events (each one re-evaluates every view that observes it).
    var modelChanges = 0
    /// CPU time of the main thread (does not count time the machine gave to other processes).
    private var cpuStart: UInt64 = 0

    func attach(to view: NSView) {
        guard displayLink == nil else { return }
        let link = view.displayLink(target: self, selector: #selector(vsync(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
        // Main thread busy time: from waking up to going back to sleep, every run loop pass.
        observer = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.afterWaiting.rawValue
                                                      | CFRunLoopActivity.beforeWaiting.rawValue, true, 0) {
            [weak self] _, activity in
            guard let self, self.recording else { return }
            let now = CACurrentMediaTime()
            if activity == .afterWaiting {
                self.busyStart = now
            } else if let start = self.busyStart {
                self.busy.append(now - start)
                self.busyStart = nil
            }
        }
        CFRunLoopAddObserver(CFRunLoopGetMain(), observer, .commonModes)
    }

    @objc private func vsync(_ link: CADisplayLink) {
        if recording { vsyncs.append(CACurrentMediaTime()) }
    }

    /// Called by the map renderer after each frame it encoded (`encode`: seconds spent).
    func mapFrame(encode: CFTimeInterval) {
        guard recording else { return }
        mapFrames.append(CACurrentMediaTime())
        mapEncode.append(encode)
    }

    func start() {
        vsyncs = []; mapFrames = []; mapEncode = []; busy = []; busyStart = nil; modelChanges = 0
        cpuStart = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        recording = true
    }

    /// Stops recording and describes what was recorded over `seconds`.
    func stop(_ name: String, seconds: Double, refresh: Double) -> String {
        recording = false
        let cpu = Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - cpuStart) / 1e6
        func intervals(_ times: [CFTimeInterval]) -> [Double] {
            zip(times.dropFirst(), times).map { ($0 - $1) * 1000 }
        }
        func summary(_ values: [Double]) -> String {
            guard !values.isEmpty else { return "none" }
            let sorted = values.sorted()
            func at(_ q: Double) -> Double { sorted[min(sorted.count - 1, Int(Double(sorted.count - 1) * q))] }
            return String(format: "n=%d p50=%.1f p95=%.1f p99=%.1f max=%.1f ms", sorted.count, at(0.5), at(0.95), at(0.99),
                          sorted.last!)
        }
        let frame = 1000 / refresh
        let vsync = intervals(vsyncs)
        let map = intervals(mapFrames)
        let late = vsync.filter { $0 > frame * 1.5 }
        let missed = late.reduce(0) { $0 + Int(($1 / frame).rounded()) - 1 }
        var lines = ["[\(name)] \(String(format: "%.1f", seconds)) s, display \(String(format: "%.0f", refresh)) Hz"]
        lines.append("  display frames: \(summary(vsync)); late \(late.count), missed vsyncs \(missed)")
        lines.append("  map frames:     \(summary(map)), \(String(format: "%.1f", Double(map.count + (map.isEmpty ? 0 : 1)) / seconds)) fps")
        lines.append("  map encode:     \(summary(mapEncode.map { $0 * 1000 }))")
        lines.append("  model changes:  \(String(format: "%.1f", Double(modelChanges) / seconds))/s")
        let total = busy.reduce(0, +) * 1000
        lines.append("  main thread busy: \(String(format: "%.0f", total / seconds)) ms/s; passes \(summary(busy.map { $0 * 1000 }))")
        lines.append("  main thread CPU: \(String(format: "%.0f", cpu / seconds)) ms/s")
        return lines.joined(separator: "\n")
    }
}

@MainActor
enum PerfHarness {
    struct Request {
        let zh: String, base: String, map: String, out: String
    }
    static var request: Request?

    /// Reads --perf from the command line (before the UI exists) and starts recording.
    static func parseArguments() {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--perf") else { return }
        let values = Array(arguments[(at + 1)...])
        guard values.count == 4 else {
            FileHandle.standardError.write("usage: --perf <zh> <base> <map path> <out.txt>\n".data(using: .utf8)!)
            exit(2)
        }
        request = Request(zh: values[0], base: values[1], map: values[2], out: values[3])
        PerfRecorder.shared = PerfRecorder()
        UserDefaults.standard.set(EditorMode.maps.rawValue, forKey: "editorMode")
        UserDefaults.standard.set(values[1], forKey: "baseGameDir")
        UserDefaults.standard.set(values[0], forKey: "lastGameDir")
    }

    static func run(_ editor: EditorModel) {
        guard let request, let recorder = PerfRecorder.shared else { return }
        let changes = editor.objectWillChange.sink { _ in recorder.modelChanges += 1 }
        Task { @MainActor in
            _ = changes
            var report: [String] = []
            func log(_ line: String) {
                report.append(line)
                print(line)
                fflush(stdout)
                try? report.joined(separator: "\n").appending("\n").write(toFile: request.out, atomically: true, encoding: .utf8)
            }
            func finish(_ code: Int32) -> Never {
                try? report.joined(separator: "\n").appending("\n").write(toFile: request.out, atomically: true, encoding: .utf8)
                exit(code)
            }
            func wait(_ what: String, timeout: Double, until done: () -> Bool) async {
                let start = Date()
                while !done() {
                    if Date().timeIntervalSince(start) > timeout { log("timeout: \(what)"); finish(1) }
                    try? await Task.sleep(nanoseconds: 50_000_000)
                }
            }
            await wait("game", timeout: 120) { editor.status.state == "ready" }
            editor.openMap(request.map)
            await wait("map", timeout: 120) { editor.mapState == "ready" }
            await wait("models", timeout: 300) { editor.mapModelCount == 0 || editor.mapModelsLoaded >= editor.mapModelCount }
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            guard let window = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil }),
                  let content = window.contentView,
                  let mapView = find(MapMTKView.self, in: content),
                  let list = find(NSTableView.self, in: content)?.enclosingScrollView else {
                log("no map view or list in the window")
                finish(1)
            }
            recorder.attach(to: content)
            let refresh = Double(window.screen?.maximumFramesPerSecond ?? 60)
            log("map: \(editor.mapScene.name), \(editor.mapScene.objects.count) objects, \(editor.mapModelCount) models; "
                + "window \(Int(window.frame.width))x\(Int(window.frame.height)), map view \(Int(mapView.bounds.width))x\(Int(mapView.bounds.height))")
            try? await Task.sleep(nanoseconds: 500_000_000)

            // Input comes from a run loop timer at about 120 Hz, like a trackpad (AppKit's scroll
            // handling must run from the run loop, not from a task).
            @MainActor func measure(_ name: String, seconds: Double, step: @escaping @MainActor (Int) -> Void) async {
                recorder.start()
                let start = CACurrentMediaTime()
                await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
                    var tick = 0
                    let timer = Timer(timeInterval: 1.0 / 120, repeats: true) { timer in
                        guard CACurrentMediaTime() - start < seconds else {
                            timer.invalidate()
                            done.resume()
                            return
                        }
                        MainActor.assumeIsolated { step(tick) }
                        tick += 1
                    }
                    RunLoop.main.add(timer, forMode: .common)
                }
                log(recorder.stop(name, seconds: CACurrentMediaTime() - start, refresh: refresh))
            }

            await measure("idle", seconds: 3) { _ in }

            // Pan: press in the middle of the map, drag in a slow circle, release.
            let center = mapView.convert(NSPoint(x: mapView.bounds.midX, y: mapView.bounds.midY), to: nil)
            post(mouse: .leftMouseDown, at: center, window: window)
            var last = center
            await measure("pan", seconds: 4) { tick in
                let angle = Double(tick) * 0.05
                last = NSPoint(x: center.x + 150 * cos(angle), y: center.y + 100 * sin(angle))
                post(mouse: .leftMouseDragged, at: last, window: window)
            }
            post(mouse: .leftMouseUp, at: last, window: window)
            try? await Task.sleep(nanoseconds: 500_000_000)

            // Lists: scrolled down for 2 s and back up for 2 s, 12 pt per input step (1440 pt/s).
            // The clip view is moved directly: synthetic scroll-wheel events have no window and
            // AppKit's concurrent scrolling ignores ones not taken from the queue.
            @MainActor func scroll(_ name: String, _ list: NSScrollView) async {
                let clip = list.contentView
                let before = clip.bounds.origin.y
                var furthest = before
                await measure(name, seconds: 4) { tick in
                    let y = clip.bounds.origin.y + (tick < 240 ? 12 : -12)
                    let limit = max((list.documentView?.frame.height ?? 0) - clip.bounds.height, 0)
                    clip.scroll(to: NSPoint(x: clip.bounds.origin.x, y: min(max(y, 0), limit)))
                    list.reflectScrolledClipView(clip)
                    furthest = max(furthest, clip.bounds.origin.y)
                }
                log(String(format: "  scrolled %.0f pt (of %.0f)", furthest - before,
                           (list.documentView?.frame.height ?? 0) - clip.bounds.height))
            }
            await scroll("map list", list)

            // The object table of the Objects mode, with the map still open.
            UserDefaults.standard.set(EditorMode.objects.rawValue, forKey: "editorMode")
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            guard let table = find(NSTableView.self, in: content)?.enclosingScrollView else {
                log("no object table")
                finish(1)
            }
            await scroll("object table", table)
            finish(0)
        }
    }

    private static func find<T: NSView>(_ type: T.Type, in view: NSView) -> T? {
        if let match = view as? T { return match }
        for child in view.subviews {
            if let match = find(type, in: child) { return match }
        }
        return nil
    }

    private static func post(mouse type: NSEvent.EventType, at point: NSPoint, window: NSWindow) {
        guard let event = NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                             timestamp: ProcessInfo.processInfo.systemUptime,
                                             windowNumber: window.windowNumber, context: nil, eventNumber: 0,
                                             clickCount: 1, pressure: type == .leftMouseUp ? 0 : 1) else { return }
        NSApp.postEvent(event, atStart: false)
    }

}
