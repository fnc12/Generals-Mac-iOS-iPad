// `END Editor --ui-check <zh> <base> <map path> <template> <save.map> <out.txt>` checks map
// editing in the real window, with input fed through the app's own event queue like --perf: a
// right click places <template> in the middle of the view, a drag moves it, ] turns it, Delete
// removes it, ⌘Z brings it back, Edit > Redo removes it again and ⌘Z restores it; then the map is saved
// to <save.map> and opened again. Every step is checked against the store and the renderer and
// written to <out.txt>; the exit code is the number of failed checks. For the gui-sandbox VM.
import AppKit
import QuartzCore
import simd

@MainActor
enum UICheck {
    struct Request {
        let zh: String, base: String, map: String, template: String, save: String, out: String
    }
    static var request: Request?

    static func parseArguments() {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--ui-check") else { return }
        let values = Array(arguments[(at + 1)...])
        guard values.count == 6 else {
            FileHandle.standardError.write("usage: --ui-check <zh> <base> <map> <template> <save.map> <out.txt>\n"
                .data(using: .utf8)!)
            exit(2)
        }
        request = Request(zh: values[0], base: values[1], map: values[2], template: values[3], save: values[4],
                          out: values[5])
        UserDefaults.standard.set(EditorMode.maps.rawValue, forKey: "editorMode")
        UserDefaults.standard.set(values[1], forKey: "baseGameDir")
        UserDefaults.standard.set(values[0], forKey: "lastGameDir")
    }

    static func run(_ editor: EditorModel) {
        guard let request else { return }
        Task { @MainActor in
            var report: [String] = []
            var failures: Int32 = 0
            func log(_ line: String) {
                report.append(line)
                print(line)
                fflush(stdout)
                try? report.joined(separator: "\n").appending("\n").write(toFile: request.out, atomically: true,
                                                                          encoding: .utf8)
            }
            func check(_ what: String, _ ok: Bool) {
                log((ok ? "ok    " : "FAIL  ") + what)
                if !ok { failures += 1 }
            }
            func wait(_ what: String, timeout: Double = 120, until done: () -> Bool) async {
                let start = Date()
                while !done() {
                    if Date().timeIntervalSince(start) > timeout { check("timeout: \(what)", false); break }
                    try? await Task.sleep(nanoseconds: 20_000_000)
                }
            }
            func settle() async { try? await Task.sleep(nanoseconds: 300_000_000) }
            func finish() {
                log(failures == 0 ? "all checks passed" : "\(failures) checks failed")
                exit(failures)
            }

            await wait("game") { editor.status.state == "ready" }
            editor.openMap(request.map)
            await wait("map") { editor.mapState == "ready" }
            await wait("models") { editor.mapModelCount == 0 || editor.mapModelsLoaded >= editor.mapModelCount }
            guard let window = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil }),
                  let content = window.contentView, let view = PerfHarness.find(MapMTKView.self, in: content),
                  let renderer = view.renderer, let state = view.state else {
                log("no map view")
                exit(1)
            }
            NSApp.activate(ignoringOtherApps: true)
            window.makeKeyAndOrderFront(nil)
            await settle()
            let before = editor.mapScene.objects.count
            log("map \(editor.mapScene.name): \(before) objects; players: "
                + editor.mapPlayers.map { "\($0.label)=\($0.team)" }.joined(separator: ", "))

            // Place: a right click in the middle of the view.
            let cameraAtPlacement = state.target
            editor.placementTemplate = request.template
            if let owner = editor.mapPlayers.first(where: { !$0.neutral }) { editor.placementTeam = owner.team }
            editor.placementAngle = 90
            let center = NSPoint(x: view.bounds.midX, y: view.bounds.midY)
            let middle = view.convert(center, to: nil)
            let viewPoint = CGPoint(x: center.x, y: view.bounds.height - center.y)
            let ground = state.groundPoint(at: viewPoint, size: view.bounds.size, scene: renderer.scene)
            check("the middle of the view is on the ground", ground != nil)
            let clicked = CACurrentMediaTime()
            PerfHarness.post(mouse: .rightMouseDown, at: middle, window: window)
            PerfHarness.post(mouse: .rightMouseUp, at: middle, window: window)
            while editor.mapScene.objects.count == before && CACurrentMediaTime() - clicked < 2 {
                try? await Task.sleep(nanoseconds: 2_000_000)
            }
            log(String(format: "  placed %.0f ms after the click", (CACurrentMediaTime() - clicked) * 1000))
            await settle()
            let placed = editor.mapScene.objects.last
            check("right click placed \(request.template)", editor.mapScene.objects.count == before + 1
                  && placed?.templateName == request.template)
            guard let placed, let ground else { finish(); return }
            check("on the clicked ground point", simd_distance(SIMD2(placed.position.x, placed.position.y),
                                                               SIMD2(ground.x, ground.y)) < 1)
            check("at the terrain height", abs(placed.position.z - renderer.scene.groundHeight(x: ground.x, y: ground.y)) < 0.01)
            check("turned by the placement angle", abs(placed.angle - .pi / 2) < 0.001)
            check("owned by \(editor.placementTeam)", placed.owner == editor.placementTeam)
            check("selected", editor.selectedMapObject == placed.id)
            await wait("its model") { editor.mapModels.contains { $0.id == placed.modelId } }
            await settle()
            check("drawn with its model", placed.kind == "model" && renderer.drawsModel(placed))
            check("undo count 1", editor.mapEdit.undoCount == 1 && editor.mapEdit.edited)

            // Move: drag the selected object 80 points right.
            let cameraBefore = state.target
            let viewNow = PerfHarness.find(MapMTKView.self, in: content)
            check("the map view is the same view after the inspector opened", viewNow === view)
            check("the camera stayed (target \(state.target))", simd_distance(state.target, cameraAtPlacement) < 0.01)
            // The inspector opened with the selection, so the view is narrower now: press where the
            // object shows.
            guard let shown = state.viewPoint(of: placed.position + SIMD3(0, 0, 3), size: view.bounds.size) else {
                check("the object is in view", false)
                finish()
                return
            }
            let start = view.convert(NSPoint(x: shown.x, y: view.bounds.height - shown.y), to: nil)
            check("the press is on the selected object", renderer.hits(placed.id, at: shown, size: view.bounds.size))
            let target = NSPoint(x: start.x + 80, y: start.y)
            PerfHarness.post(mouse: .leftMouseDown, at: start, window: window)
            for step in 1...16 {
                PerfHarness.post(mouse: .leftMouseDragged, at: NSPoint(x: start.x + CGFloat(step * 5), y: start.y),
                                 window: window)
                try? await Task.sleep(nanoseconds: 16_000_000)
            }
            PerfHarness.post(mouse: .leftMouseUp, at: target, window: window)
            await settle()
            let moved = editor.mapScene.objects.first { $0.id == placed.id }
            let targetGround = state.groundPoint(at: CGPoint(x: shown.x + 80, y: shown.y), size: view.bounds.size,
                                                 scene: renderer.scene)
            check("drag moved it", moved.map { simd_distance($0.position, placed.position) > 10 } ?? false)
            // The object keeps its offset from the ground point grabbed.
            if let moved, let targetGround,
               let grabbed = state.groundPoint(at: shown, size: view.bounds.size, scene: renderer.scene) {
                let expected = SIMD2(targetGround.x, targetGround.y) + SIMD2(placed.position.x, placed.position.y)
                    - SIMD2(grabbed.x, grabbed.y)
                check("to the ground under the cursor", simd_distance(SIMD2(moved.position.x, moved.position.y), expected) < 2)
                check("at the terrain height there", abs(moved.position.z - renderer.scene.groundHeight(
                    x: moved.position.x, y: moved.position.y)) < 0.01)
            }
            check("the camera did not pan", state.target == cameraBefore)

            // Turn: ] adds 15°.
            post(key: "]", code: 30, window: window)
            await settle()
            let turned = editor.mapScene.objects.first { $0.id == placed.id }
            check("] turned it by 15°", turned.map { abs($0.angle - (.pi / 2 + .pi / 12)) < 0.001 } ?? false)

            // Delete, undo, redo, undo.
            post(key: "\u{7F}", code: 51, window: window)
            await settle()
            check("Delete removed it", !editor.mapScene.objects.contains { $0.id == placed.id }
                  && editor.selectedMapObject == nil)
            post(key: "z", code: 6, modifiers: .command, window: window)
            await settle()
            check("⌘Z brought it back", editor.mapScene.objects.contains { $0.id == placed.id })
            // Redo from the Edit menu (synthetic ⇧⌘Z events do not match key equivalents with
            // Shift; real ones do).
            if let edit = NSApp.mainMenu?.items.first(where: { $0.submenu?.items.contains { $0.title == "Redo" } ?? false })?.submenu,
               let index = edit.items.firstIndex(where: { $0.title == "Redo" }) {
                edit.performActionForItem(at: index)
            }
            await settle()
            check("Edit > Redo removed it again", !editor.mapScene.objects.contains { $0.id == placed.id })
            post(key: "z", code: 6, modifiers: .command, window: window)
            await settle()
            check("⌘Z restored it", editor.mapScene.objects.contains { $0.id == placed.id })
            check("undo count 3 (place, move, turn), redo 1", editor.mapEdit.undoCount == 3 && editor.mapEdit.redoCount == 1)

            // Save, open the saved file.
            let final = editor.mapScene.objects.first { $0.id == placed.id }
            editor.saveMap(to: URL(fileURLWithPath: request.save))
            await wait("save") { editor.mapEdit.savedPath == request.save || editor.mapEdit.message.contains("error") }
            check("saved: \(editor.mapEdit.message)", editor.mapEdit.savedPath == request.save && !editor.mapEdit.edited)
            check("the user map is listed", editor.maps.contains { $0.path == request.save }
                  || !request.save.contains("/Maps/"))
            editor.openMap(request.save)
            await wait("saved map") { editor.mapState == "ready" && editor.mapScene.key.hasPrefix(request.save) }
            let reopened = editor.mapScene.objects.filter { $0.templateName == request.template }
            check("the saved map has \(before + 1) objects (\(editor.mapScene.objects.count))",
                  editor.mapScene.objects.count == before + 1)
            if let final {
                check("with the object where it was left", reopened.contains {
                    simd_distance($0.position, final.position) < 0.01 && abs($0.angle - final.angle) < 0.0001
                        && $0.owner == final.owner
                })
            }
            // Latency of a placement with the inspector already open (the first one also opens it).
            for round in 1...3 {
                let count = editor.mapScene.objects.count
                guard let point = state.viewPoint(of: state.target, size: view.bounds.size) else { break }
                let at = view.convert(NSPoint(x: point.x + CGFloat(round * 20), y: view.bounds.height - point.y), to: nil)
                let start = CACurrentMediaTime()
                PerfHarness.post(mouse: .rightMouseDown, at: at, window: window)
                PerfHarness.post(mouse: .rightMouseUp, at: at, window: window)
                while editor.mapScene.objects.count == count && CACurrentMediaTime() - start < 2 {
                    try? await Task.sleep(nanoseconds: 1_000_000)
                }
                log(String(format: "  placement %d shown in the model %.0f ms after the click", round,
                           (CACurrentMediaTime() - start) * 1000))
                await settle()
            }
            finish()
        }
    }

    private static func post(key: String, code: UInt16, modifiers: NSEvent.ModifierFlags = [], window: NSWindow) {
        for type in [NSEvent.EventType.keyDown, .keyUp] {
            guard let event = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: modifiers,
                                               timestamp: ProcessInfo.processInfo.systemUptime,
                                               windowNumber: window.windowNumber, context: nil, characters: key,
                                               charactersIgnoringModifiers: key, isARepeat: false, keyCode: code)
            else { continue }
            NSApp.postEvent(event, atStart: false)
        }
    }
}
