// `END Editor --ui-check <zh> <base> <map path> <template> <save name> <out.txt>` checks map
// editing in the real window the way a user does it, for a machine nobody is watching (the
// gui-sandbox VM). Every step is input: mouse and key events posted to the app's own event queue
// (like --perf, no Accessibility permission needed) at the places where the views show things, and
// clicks on the real controls, found in the window's views (the map list's search field and row,
// the object picker's search field and row, the inspector's fields, the tool buttons, the Edit
// menu). Nothing sets a model property. Each step is then checked against the store (what
// EditorModel read from it) and the renderer, and written to <out.txt>; the exit code is the
// number of failed checks.
//
// The steps: open the map from the map list; right-click with no object chosen (the picker opens),
// type the template's name into the picker and click its row (the object goes where the right
// click was); ⌃-click a second one; ⇧-click to select both, drag them, turn them
// with ]; ⌘C, ⌘V at the cursor, ⌘D; a ⇧-drag rubber band; Delete and ⌘Z; type a height and a
// health into the inspector and drag the object; place three linked waypoints and a road with the
// tool buttons; undo and redo; save (the system save panel is not driven: it runs out of process),
// then the saved map opened from the list again. Not driven: the owner pop-ups (SwiftUI's pop-up
// here is no NSPopUpButton and its open menu runs its own event loop) and the save panel.
import AppKit
import QuartzCore
import simd

@MainActor
enum UICheck {
    struct Request {
        let zh: String, base: String, map: String, template: String, save: String, out: String
    }
    static var request: Request?
    static var diagnostic = ""

    static func parseArguments() {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--ui-check") else { return }
        let values = Array(arguments[(at + 1)...])
        guard values.count == 6 else {
            FileHandle.standardError.write("usage: --ui-check <zh> <base> <map> <template> <save name> <out.txt>\n"
                .data(using: .utf8)!)
            exit(2)
        }
        request = Request(zh: values[0], base: values[1], map: values[2], template: values[3], save: values[4],
                          out: values[5])
        // Launch settings, like a user who used the app before: the Maps mode and the folders.
        UserDefaults.standard.set(EditorMode.maps.rawValue, forKey: "editorMode")
        UserDefaults.standard.set(values[1], forKey: "baseGameDir")
        UserDefaults.standard.set(values[0], forKey: "lastGameDir")
    }

    // MARK: Finding and using controls

    /// Every accessibility element under `root` (the app's own tree, no permission needed), each
    /// once (SwiftUI's tree can reach a node along several paths), at most `limit` of them.
    static func elements(under root: AnyObject, limit: Int = 5000) -> [AnyObject] {
        var result: [AnyObject] = []
        var seen: Set<ObjectIdentifier> = [ObjectIdentifier(root)]
        var queue: [AnyObject] = [root]
        while !queue.isEmpty && result.count < limit {
            let node = queue.removeFirst()
            guard let children = attribute("accessibilityChildren", of: node) as? [Any] else { continue }
            for child in children {
                let object = child as AnyObject
                guard seen.insert(ObjectIdentifier(object)).inserted else { continue }
                result.append(object)
                queue.append(object)
            }
        }
        return result
    }

    /// Clicks a row of the list (a SwiftUI List: an outline view) that shows `rows` rows; SwiftUI's
    /// rows have no accessibility identifiers of their own, so the row is found by its index.
    static func clickRow(_ index: Int, rows: Int, in window: NSWindow) -> Bool {
        guard let content = window.contentView,
              let table = allViews(content).compactMap({ $0 as? NSTableView }).first(where: { $0.numberOfRows == rows }),
              index >= 0, index < rows else { return false }
        table.scrollRowToVisible(index)
        let rect = table.convert(table.rect(ofRow: index), to: nil)
        window.makeKeyAndOrderFront(nil)
        click(at: NSPoint(x: rect.midX, y: rect.midY), window: window)
        return true
    }

    /// An accessibility attribute through its getter, nil when the element has none (KVC on a
    /// missing key raises an Objective-C exception, which would end the check's task).
    static func attribute(_ name: String, of element: AnyObject) -> Any? {
        guard let object = element as? NSObject, object.responds(to: NSSelectorFromString(name)) else { return nil }
        return object.value(forKey: name)
    }

    static func identifier(of element: AnyObject) -> String {
        (attribute("accessibilityIdentifier", of: element) as? String) ?? ""
    }
    static func label(of element: AnyObject) -> String {
        ["accessibilityLabel", "accessibilityTitle", "accessibilityValue"]
            .compactMap { attribute($0, of: element) as? String }.joined(separator: " ")
    }

    /// The first visible window with an element that matches.
    static func find(_ match: @MainActor (AnyObject) -> Bool) -> (AnyObject, NSWindow)? {
        for window in NSApp.windows where window.isVisible {
            if let element = elements(under: window).first(where: match) { return (element, window) }
        }
        return nil
    }

    static func find(id: String) -> (AnyObject, NSWindow)? { find { identifier(of: $0) == id } }

    /// The element's middle in its window's coordinates.
    static func middle(of element: AnyObject, in window: NSWindow) -> NSPoint? {
        guard let frame = (attribute("accessibilityFrame", of: element) as? NSValue)?.rectValue,
              frame.width > 0 else { return nil }
        let rect = window.convertFromScreen(frame)
        return NSPoint(x: rect.midX, y: rect.midY)
    }

    static func click(at point: NSPoint, window: NSWindow, modifiers: NSEvent.ModifierFlags = []) {
        PerfHarness.post(mouse: .leftMouseDown, at: point, window: window, modifiers: modifiers)
        PerfHarness.post(mouse: .leftMouseUp, at: point, window: window, modifiers: modifiers)
    }

    @discardableResult
    static func click(_ element: AnyObject, window: NSWindow) -> Bool {
        guard let point = middle(of: element, in: window) else { return false }
        window.makeKeyAndOrderFront(nil)
        click(at: point, window: window)
        return true
    }

    static func post(key: String, code: UInt16, modifiers: NSEvent.ModifierFlags = [], window: NSWindow) {
        for type in [NSEvent.EventType.keyDown, .keyUp] {
            guard let event = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: modifiers,
                                               timestamp: ProcessInfo.processInfo.systemUptime,
                                               windowNumber: window.windowNumber, context: nil, characters: key,
                                               charactersIgnoringModifiers: key, isARepeat: false, keyCode: code)
            else { continue }
            NSApp.postEvent(event, atStart: false)
        }
    }

    /// US layout key codes: the text system reads the key code, not only the characters.
    static let keyCodes: [Character: UInt16] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11, "q": 12, "w": 13,
        "e": 14, "r": 15, "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "9": 25, "7": 26,
        "8": 28, "0": 29, "o": 31, "u": 32, "i": 34, "p": 35, "l": 37, "j": 38, "k": 40, "n": 45, "m": 46, ".": 47,
        " ": 49, "-": 27,
    ]

    /// Types text into the focused field, key by key, then Return when asked.
    static func typeText(_ text: String, window: NSWindow, returnKey: Bool = false) {
        for character in text {
            let lower = Character(character.lowercased())
            post(key: String(character), code: keyCodes[lower] ?? 0, modifiers: character.isUppercase ? .shift : [],
                 window: window)
        }
        if returnKey { post(key: "\r", code: 36, window: window) }
    }

    /// The views of every visible window (the popover's too), with their window.
    static func views() -> [(NSView, NSWindow)] {
        NSApp.windows.filter(\.isVisible).flatMap { window in
            (window.contentView.map { allViews($0.superview ?? $0) } ?? []).map { ($0, window) }
        }
    }

    /// A text field by its placeholder (a SwiftUI TextField's title), or the first editable field
    /// after the label with that text (a Form's LabeledContent hides the placeholder).
    static func field(_ title: String) -> (NSTextField, NSWindow)? {
        var labelSeen = false
        for (view, window) in views() {
            guard let field = view as? NSTextField else { continue }
            if field.isEditable && (field.placeholderString == title || labelSeen) { return (field, window) }
            if !field.isEditable && field.stringValue == title { labelSeen = true }
        }
        return nil
    }

    static func windowFrame(of view: NSView) -> NSRect { view.convert(view.bounds, to: nil) }

    /// Clicks a field, selects what it holds (⌘A) and types over it.
    static func fill(_ placeholder: String, with text: String, returnKey: Bool = true) async -> Bool {
        guard let (field, window) = field(placeholder) else {
            diagnostic = "  text fields: " + views().compactMap { $0.0 as? NSTextField }.map {
                "\(type(of: $0)) '\($0.placeholderString ?? "-")' '\($0.stringValue)' editable \($0.isEditable)"
            }.joined(separator: "; ")
            return false
        }
        let rect = windowFrame(of: field)
        window.makeKeyAndOrderFront(nil)
        click(at: NSPoint(x: rect.midX, y: rect.midY), window: window)
        try? await Task.sleep(nanoseconds: 200_000_000)
        if (window.firstResponder as? NSTextView)?.delegate as? NSTextField !== field {
            // The click did not reach the field (SwiftUI's hosting of Form rows): focus it the way
            // Tab does.
            window.makeFirstResponder(field)
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        guard (window.firstResponder as? NSTextView)?.delegate as? NSTextField === field else { return false }
        post(key: "a", code: 0, modifiers: .command, window: window)
        typeText(text, window: window, returnKey: returnKey)
        return true
    }

    /// Clicks the segment of a segmented control that has a segment labelled `label`.
    static func clickSegment(_ label: String) -> Bool {
        for (view, window) in views() {
            guard let control = view as? NSSegmentedControl,
                  let index = (0..<control.segmentCount).first(where: { control.label(forSegment: $0) == label })
            else { continue }
            let rect = windowFrame(of: control)
            let width = rect.width / CGFloat(control.segmentCount)
            window.makeKeyAndOrderFront(nil)
            click(at: NSPoint(x: rect.minX + width * (CGFloat(index) + 0.5), y: rect.midY), window: window)
            return true
        }
        return false
    }

    static func frame(of element: AnyObject) -> NSRect? {
        (attribute("accessibilityFrame", of: element) as? NSValue)?.rectValue
    }

    static func allViews(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(allViews) }

    // MARK: The check

    static func run(_ editor: EditorModel) {
        guard let request else { return }
        Task { @MainActor in
            var report: [String] = []
            var failures: Int32 = 0
            @MainActor func log(_ line: String) {
                report.append(line)
                print(line)
                fflush(stdout)
                try? report.joined(separator: "\n").appending("\n").write(toFile: request.out, atomically: true,
                                                                          encoding: .utf8)
            }
            @MainActor func check(_ what: String, _ ok: Bool) {
                log((ok ? "ok    " : "FAIL  ") + what)
                if !ok { failures += 1 }
            }
            @MainActor func wait(_ what: String, timeout: Double = 120, until done: () -> Bool) async {
                let start = Date()
                while !done() {
                    if Date().timeIntervalSince(start) > timeout { check("timeout: \(what)", false); break }
                    try? await Task.sleep(nanoseconds: 20_000_000)
                }
            }
            @MainActor func settle(_ seconds: Double = 0.3) async { try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9)) }
            @MainActor func finish() -> Never {
                log(failures == 0 ? "all checks passed" : "\(failures) checks failed")
                exit(failures)
            }

            await wait("game") { editor.status.state == "ready" }
            await wait("the window", timeout: 20) { NSApp.windows.contains { $0.isVisible && $0.contentView != nil } }
            guard let window = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil }),
                  let content = window.contentView else { log("no window"); exit(1) }
            NSApp.activate(ignoringOtherApps: true)
            window.makeKeyAndOrderFront(nil)
            await settle()

            // Opens a map from the list: its name typed into the search field, its row clicked.
            @MainActor func openFromList(_ name: String) async -> Bool {
                guard let search = PerfHarness.find(NSSearchField.self, in: content) else { return false }
                click(at: search.convert(NSPoint(x: search.bounds.midX, y: search.bounds.midY), to: nil), window: window)
                await settle(0.2)
                post(key: "a", code: 0, modifiers: .command, window: window)
                typeText(name, window: window)
                await settle(0.8)
                // The list shows the maps whose name has the search text, in the store's order.
                let shown = editor.maps.filter { $0.name.localizedCaseInsensitiveContains(name) }
                guard let index = shown.firstIndex(where: { $0.name.localizedCaseInsensitiveCompare(name) == .orderedSame })
                else { log("  no map named \(name)"); return false }
                return clickRow(index, rows: shown.count, in: window)
            }
            let mapName = URL(fileURLWithPath: request.map).deletingPathExtension().lastPathComponent
            check("the map \(mapName) is clicked in the map list", await openFromList(mapName))
            await wait("map") { editor.mapState == "ready" }
            await wait("models") { editor.mapModelCount == 0 || editor.mapModelsLoaded >= editor.mapModelCount }
            guard let view = PerfHarness.find(MapMTKView.self, in: content), let renderer = view.renderer,
                  let state = view.state else { log("no map view"); exit(1) }
            await settle()
            // The inspector is to the right of the map view, the placement bar under it.
            let mapRight = windowFrame(of: view).maxX
            let before = editor.mapScene.objects.count
            log("map \(editor.mapScene.name): \(before) objects; players: "
                + editor.mapPlayers.map { "\($0.label)=\($0.team)" }.joined(separator: ", "))

            // Points of the map view: window coordinates of a view point (origin top left), and back.
            @MainActor func windowPoint(_ p: CGPoint) -> NSPoint { view.convert(NSPoint(x: p.x, y: view.bounds.height - p.y), to: nil) }
            @MainActor func ground(_ p: CGPoint) -> SIMD3<Float>? { state.groundPoint(at: p, size: view.bounds.size, scene: renderer.scene) }
            @MainActor func shown(_ id: Int64) -> CGPoint? {
                guard let object = editor.mapScene.objects.first(where: { $0.id == id }) else { return nil }
                return state.viewPoint(of: object.position + SIMD3(0, 0, 3), size: view.bounds.size)
            }
            /// A point of the view where `id` is the nearest object under the cursor (others may be in
            /// front of its middle).
            @MainActor func pointOn(_ id: Int64) -> CGPoint? {
                guard let middle = shown(id) else { return nil }
                for radius in stride(from: 0, through: 40, by: 4) {
                    for dx in stride(from: -radius, through: radius, by: 4) {
                        for dy in stride(from: -radius, through: radius, by: 4) where abs(dx) == radius || abs(dy) == radius {
                            let p = CGPoint(x: middle.x + CGFloat(dx), y: middle.y + CGFloat(dy))
                            if renderer.hits(at: p, size: view.bounds.size).first == id { return p }
                        }
                    }
                }
                return nil
            }
            @MainActor func object(_ id: Int64) -> MapObjectItem? { editor.mapScene.objects.first { $0.id == id } }
            @MainActor func drag(from start: NSPoint, by offset: CGFloat, modifiers: NSEvent.ModifierFlags = []) async {
                PerfHarness.post(mouse: .leftMouseDown, at: start, window: window, modifiers: modifiers)
                for step in 1...16 {
                    PerfHarness.post(mouse: .leftMouseDragged, at: NSPoint(x: start.x + offset * CGFloat(step) / 16,
                                                                          y: start.y - offset * CGFloat(step) / 32),
                                     window: window, modifiers: modifiers)
                    try? await Task.sleep(nanoseconds: 16_000_000)
                }
                PerfHarness.post(mouse: .leftMouseUp, at: NSPoint(x: start.x + offset, y: start.y - offset / 2),
                                 window: window, modifiers: modifiers)
                await settle()
            }

            // 1. A right click with no object chosen: the picker opens, the object goes there once chosen.
            check("no object is chosen at start", editor.mapEditor.templateName.isEmpty)
            let center = CGPoint(x: view.bounds.midX, y: view.bounds.midY)
            guard let clickedGround = ground(center) else { check("the middle of the view is on the ground", false); finish() }
            PerfHarness.post(mouse: .rightMouseDown, at: windowPoint(center), window: window)
            PerfHarness.post(mouse: .rightMouseUp, at: windowPoint(center), window: window)
            await settle(0.8)
            check("the store asks for the picker and holds the right click",
                  editor.mapEditor.picker && editor.mapEditor.pending.map {
                      simd_distance($0, SIMD2(clickedGround.x, clickedGround.y)) < 1 } == true)
            check("the store's note says what to do: \(editor.mapEditor.note)", !editor.mapEditor.note.isEmpty)
            let searchShown = field("Name or side")
            check("the picker shows", searchShown != nil)
            if searchShown != nil {
                _ = await fill("Name or side", with: request.template, returnKey: false)
                await settle(1.0)
            }
            await settle(1.0)
            check("the note stays while the picker is open (not overwritten by store reads)", !editor.mapEditor.note.isEmpty)
            let pickerWindow = field("Name or side")?.1
            // The picker shows the templates whose name or side has the search text, in the store's order.
            let listed = editor.objects.filter { $0.name.localizedCaseInsensitiveContains(request.template)
                || $0.side.localizedCaseInsensitiveContains(request.template) }
            check("the picker's row \(request.template) is clicked", pickerWindow.map { window in
                listed.firstIndex { $0.name == request.template }.map { clickRow($0, rows: listed.count, in: window) } ?? false
            } ?? false)
            await wait("placement", timeout: 5) { editor.mapScene.objects.count == before + 1 }
            await settle()
            let first = editor.mapScene.objects.last
            check("the click on the row placed \(request.template)", first?.templateName == request.template
                  && editor.mapScene.objects.count == before + 1)
            guard let first else { finish() }
            check("at the right-clicked ground point", simd_distance(SIMD2(first.position.x, first.position.y),
                                                                     SIMD2(clickedGround.x, clickedGround.y)) < 1)
            check("at the terrain height", abs(first.position.z - renderer.scene.groundHeight(x: first.position.x,
                                                                                          y: first.position.y)) < 0.01)
            check("selected, picker closed", editor.mapSelection == [first.id] && !editor.mapEditor.picker
                  && field("Name or side") == nil)
            await wait("its model") { editor.mapModels.contains { $0.id == first.modelId } }

            // 2. The owner menu, then a ⌃-click places a second one with that owner.
            // The owner pop-ups are not driven: SwiftUI's pop-up is no NSPopUpButton here, and its
            // open menu runs its own event loop. Owners are checked through the core (map_flow_tests).
            let secondAt = CGPoint(x: center.x + 120, y: center.y + 40)
            PerfHarness.post(mouse: .leftMouseDown, at: windowPoint(secondAt), window: window, modifiers: .control)
            PerfHarness.post(mouse: .leftMouseUp, at: windowPoint(secondAt), window: window, modifiers: .control)
            await settle()
            let second = editor.mapScene.objects.last
            check("⌃-click placed a second one", editor.mapScene.objects.count == before + 2 && second?.id != first.id)
            guard let second else { finish() }
            check("owned by the chosen team", second.owner == editor.mapEditor.team)
            check("only it is selected", editor.mapSelection == [second.id])

            // 3. ⇧-click adds the first one; dragging moves both.
            guard let firstShown = pointOn(first.id) else { check("the first object is in view", false); finish() }
            log("  before ⇧-click: hits \(renderer.hits(at: firstShown, size: view.bounds.size)), key \(window.isKeyWindow), "
                + "responder \(String(describing: window.firstResponder.map { type(of: $0) })), at \(firstShown) in \(view.bounds.size)")
            click(at: windowPoint(firstShown), window: window, modifiers: .shift)
            await settle()
            log("  after: selection \(editor.mapSelection), gesture '\(editor.mapGesture.kind)', note '\(editor.mapEditor.note)'")
            check("⇧-click selected both", Set(editor.mapSelection) == [first.id, second.id])
            let cameraBefore = state.target
            await drag(from: windowPoint(firstShown), by: 80)
            let movedFirst = object(first.id), movedSecond = object(second.id)
            if let movedFirst, let movedSecond {
                let d1 = SIMD2(movedFirst.position.x - first.position.x, movedFirst.position.y - first.position.y)
                let d2 = SIMD2(movedSecond.position.x - second.position.x, movedSecond.position.y - second.position.y)
                check("the drag moved both by the same offset (\(d1))", simd_length(d1) > 10 && simd_distance(d1, d2) < 0.01)
                check("on the ground there", abs(movedFirst.position.z - renderer.scene.groundHeight(
                    x: movedFirst.position.x, y: movedFirst.position.y)) < 0.01)
            } else {
                check("both still there after the drag", false)
            }
            check("the camera did not pan", state.target == cameraBefore)
            let undoAfterMove = editor.mapEdit.undoCount
            check("the move is one undo step (\(undoAfterMove))", undoAfterMove == 3)

            // 4. ] turns both about their middle.
            post(key: "]", code: 30, window: window)
            await settle()
            if let turnedFirst = object(first.id), let movedFirst {
                check("] turned the selection by 15°", abs(turnedFirst.angle - movedFirst.angle - .pi / 12) < 0.001
                      && simd_distance(turnedFirst.position, movedFirst.position) > 0.5)
            }

            // 5. ⌘C, the cursor elsewhere, ⌘V; ⌘D.
            post(key: "c", code: 8, modifiers: .command, window: window)
            await settle()
            check("⌘C copied 2 objects", editor.mapEditor.clipboardCount == 2)
            let pasteAt = CGPoint(x: center.x - 150, y: center.y - 60)
            PerfHarness.post(mouse: .mouseMoved, at: windowPoint(pasteAt), window: window)
            await settle()
            check("the cursor's ground point is known", editor.cursorGround != nil)
            let countBeforePaste = editor.mapScene.objects.count
            post(key: "v", code: 9, modifiers: .command, window: window)
            await settle()
            let pasted = editor.mapSelection.compactMap(object)
            check("⌘V pasted 2 objects, selected", editor.mapScene.objects.count == countBeforePaste + 2 && pasted.count == 2)
            if pasted.count == 2, let a = object(first.id), let b = object(second.id), let cursor = editor.cursorGround {
                let copied = SIMD2(b.position.x - a.position.x, b.position.y - a.position.y)
                let layout = SIMD2(pasted[1].position.x - pasted[0].position.x, pasted[1].position.y - pasted[0].position.y)
                check("with their layout kept", simd_distance(copied, layout) < 0.01 || simd_distance(copied, -layout) < 0.01)
                let middle = (SIMD2(pasted[0].position.x, pasted[0].position.y) + SIMD2(pasted[1].position.x, pasted[1].position.y)) / 2
                check("around the cursor", simd_distance(middle, cursor) < 1)
            }
            post(key: "d", code: 2, modifiers: .command, window: window)
            await settle()
            check("⌘D duplicated them", editor.mapScene.objects.count == countBeforePaste + 4 && editor.mapSelection.count == 2)
            let duplicates = editor.mapSelection

            // 6. A click on empty ground clears; a ⇧-drag rubber band selects the duplicates.
            let empty = CGPoint(x: 30, y: 30)
            click(at: windowPoint(empty), window: window)
            await settle()
            check("a click on empty ground cleared the selection", editor.mapSelection.isEmpty)
            let points = duplicates.compactMap(shown)
            if points.count == 2 {
                let low = CGPoint(x: min(points[0].x, points[1].x) - 25, y: min(points[0].y, points[1].y) - 25)
                let high = CGPoint(x: max(points[0].x, points[1].x) + 25, y: max(points[0].y, points[1].y) + 25)
                PerfHarness.post(mouse: .leftMouseDown, at: windowPoint(low), window: window, modifiers: .shift)
                for step in 1...10 {
                    let t = CGFloat(step) / 10
                    PerfHarness.post(mouse: .leftMouseDragged, at: windowPoint(CGPoint(x: low.x + (high.x - low.x) * t,
                                                                                       y: low.y + (high.y - low.y) * t)),
                                     window: window, modifiers: .shift)
                    try? await Task.sleep(nanoseconds: 16_000_000)
                }
                PerfHarness.post(mouse: .leftMouseUp, at: windowPoint(high), window: window, modifiers: .shift)
                await settle()
                check("the ⇧-drag band selected the duplicates", Set(duplicates).isSubset(of: Set(editor.mapSelection)))
            } else {
                check("the duplicates are in view", false)
            }

            // 7. Delete, ⌘Z.
            let selectedNow = editor.mapSelection
            post(key: "\u{7F}", code: 51, window: window)
            await settle()
            check("Delete removed the selection", selectedNow.allSatisfy { object($0) == nil } && !selectedNow.isEmpty)
            post(key: "z", code: 6, modifiers: .command, window: window)
            await settle()
            check("⌘Z brought it back", selectedNow.allSatisfy { object($0) != nil })

            // 8. The inspector: click the first object, type a height and a health, choose an owner.
            if let p = pointOn(first.id) {
                click(at: windowPoint(p), window: window)
            }
            await settle(0.6)
            check("a click selected only the first object", editor.mapSelection == [first.id])
            let typedHeight = await fill("Height", with: "25")
            check("height typed into the inspector", typedHeight)
            if !typedHeight { log(diagnostic) }
            await settle()
            log("  height field now '\(field("Height")?.0.stringValue ?? "?")', responder "
                + "\(String(describing: window.firstResponder.map { type(of: $0) })), note '\(editor.mapEditor.note)'")
            await settle()
            check("the object is 25 above the ground", object(first.id).map { abs($0.height - 25) < 0.01 } == true)
            check("health typed into the inspector", await fill("Health", with: "40"))
            await settle()
            check("its objectInitialHealth is 40",
                  editor.mapObjectProperties(of: first.id).contains { $0.key == "objectInitialHealth" && $0.value == "40" })
            if let p = pointOn(first.id) {
                await drag(from: windowPoint(p), by: -60)
            }
            if let raised = object(first.id) {
                check("dragged, it keeps its height", abs(raised.position.z - renderer.scene.groundHeight(
                    x: raised.position.x, y: raised.position.y) - 25) < 0.01 && abs(raised.height - 25) < 0.01)
            }

            // 9. Waypoints and a road with the tool buttons.
            @MainActor func tool(_ index: Int, _ name: String) async -> Bool {
                guard clickSegment(["Object", "Waypoint", "Road", "Bridge"][index]) else { return false }
                await settle()
                return editor.mapEditor.tool == name
            }
            click(at: windowPoint(empty), window: window)
            await settle()
            check("the Waypoint button chose the waypoint tool", await tool(1, "waypoint"))
            let linksBefore = editor.mapScene.links.filter { $0.kind == "waypoint" }.count
            let objectsBeforeWaypoints = editor.mapScene.objects.count
            for offset in [-90.0, -30.0, 30.0] {
                let p = CGPoint(x: center.x + offset, y: center.y + 120)
                PerfHarness.post(mouse: .rightMouseDown, at: windowPoint(p), window: window)
                PerfHarness.post(mouse: .rightMouseUp, at: windowPoint(p), window: window)
                await settle()
            }
            check("three right clicks placed three waypoints",
                  editor.mapScene.objects.count == objectsBeforeWaypoints + 3
                  && editor.mapScene.objects.suffix(3).allSatisfy { $0.kind == "waypoint" })
            check("linked into a path", editor.mapScene.links.filter { $0.kind == "waypoint" }.count == linksBefore + 2)
            check("the Road button chose the road tool (\(editor.mapEditor.roadType))", await tool(2, "road")
                  && !editor.mapEditor.roadType.isEmpty)
            let objectsBeforeRoad = editor.mapScene.objects.count
            for p in [CGPoint(x: center.x - 120, y: center.y - 120), CGPoint(x: center.x + 60, y: center.y - 140)] {
                PerfHarness.post(mouse: .rightMouseDown, at: windowPoint(p), window: window)
                PerfHarness.post(mouse: .rightMouseUp, at: windowPoint(p), window: window)
                await settle()
                if p.x < center.x { check("the first right click started the road", editor.mapEditor.roadStart != nil) }
            }
            check("the second right click placed the road segment", editor.mapScene.objects.count == objectsBeforeRoad + 2
                  && editor.mapScene.objects.suffix(2).allSatisfy { $0.kind == "road" }
                  && editor.mapScene.links.contains { $0.kind == "road" })
            post(key: "z", code: 6, modifiers: .command, window: window)
            await settle()
            check("⌘Z took the road away", editor.mapScene.objects.count == objectsBeforeRoad)
            // Redo from the Edit menu (synthetic ⇧⌘Z events do not match key equivalents with Shift).
            if let edit = NSApp.mainMenu?.items.first(where: { $0.submenu?.items.contains { $0.title == "Redo" } ?? false })?.submenu,
               let index = edit.items.firstIndex(where: { $0.title == "Redo" }) {
                edit.performActionForItem(at: index)
            }
            await settle()
            check("Edit > Redo put it back", editor.mapScene.objects.count == objectsBeforeRoad + 2)
            let expected = editor.mapScene.objects.count

            // 10. Save. The system save panel is not driven: its view runs in the save panel
            // service, this check's task does not run while it is up, and keys posted to it in the
            // VM opened the character palette. The map is saved where the panel puts it
            // (<user Maps>/<name>/<name>.map) with the command its Save button ends in.
            let saveName = request.save
            let maps = EditorModel.userMapsFolder(forGame: request.zh)
            editor.saveMap(to: maps.appendingPathComponent(saveName, isDirectory: true).appendingPathComponent(saveName + ".map"))
            await wait("save", timeout: 20) { editor.mapEdit.savedPath.hasSuffix("/\(saveName)/\(saveName).map") }
            let savedPath = editor.mapEdit.savedPath
            check("saved: \(editor.mapEditor.note)", savedPath.hasSuffix("/\(saveName).map")
                  && !editor.mapEdit.edited)
            let preview = URL(fileURLWithPath: savedPath).deletingPathExtension().appendingPathExtension("tga")
            let previewSize = (try? FileManager.default.attributesOfItem(atPath: preview.path)[.size] as? Int) ?? 0
            check("with its preview \(preview.lastPathComponent) (\(previewSize) bytes)", previewSize == 18 + 128 * 128 * 4 + 26)
            await wait("the map list", timeout: 5) { editor.maps.contains { $0.path == savedPath } }

            // 11. The saved map, opened from the list.
            check("the saved map is clicked in the map list", await openFromList(saveName))
            await wait("saved map") { editor.mapState == "ready" && editor.mapScene.key.hasPrefix(savedPath) }
            check("it has \(expected) objects (\(editor.mapScene.objects.count))", editor.mapScene.objects.count == expected)
            check("the raised object is 25 above the ground",
                  editor.mapScene.objects.contains { $0.templateName == request.template && abs($0.height - 25) < 0.01 })
            check("the waypoint path and the road are there",
                  editor.mapScene.links.filter { $0.kind == "waypoint" }.count == linksBefore + 2
                  && editor.mapScene.links.contains { $0.kind == "road" })
            finish()
        }
    }
}
