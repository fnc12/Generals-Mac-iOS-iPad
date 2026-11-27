// `END Editor --ui-check <zh> <base> <map path> <template> <save name> <game app> <out.txt>` checks the
// editor in the real window the way a user does it, for a machine nobody is watching (the
// gui-sandbox VM). Every step is input: mouse and key events posted to the app's own event queue
// (like --perf, no Accessibility permission needed) at the places where the views show things, and
// clicks on the real controls, found in the window's views and accessibility elements (the map
// list's search field and row, the object picker, the tool buttons, the inspector's fields, pop-ups
// and buttons, the toolbar's Play and Play settings, the Save Map As sheet, the menus). A pop-up or
// a menu opens from a click like a user's; once AppKit tracks its menu, the item is chosen the way
// AppKit does after a click on it (MenuPick). Nothing sets a model property. Each step is then
// checked against the store (what EditorModel read from it), the renderer, the files written and
// the started game process, and written to <out.txt>; the exit code is the number of failed checks.
//
// The steps: open the map from the map list; right-click with no object chosen (the picker opens),
// type the template's name into the picker and click its row (the object goes where the right
// click was); choose another owner in the placement bar's pop-up and ⌃-click a second one; ⇧-click
// to select both, drag them, turn them with ] and }; ⌘C, ⌘V at the cursor, ⌘D; a ⇧-drag rubber
// band; Delete, ⌘Z, the inspector's Delete; ⌘A; type a height, a health, an angle and a name into
// the inspector, choose an owner, click a flag, add and remove a property, drag the object; place
// three linked waypoints, link two with the inspector's button, undo and redo with the bar's
// buttons; a road, Esc on a started road, ⌘Z and Edit > Redo; the map view's Models / Markers /
// Water checkboxes, a pan, an orbit and a scroll zoom; Save Map As: the sheet from File > Save Map
// As… and Cancel, then the bar's Save As…, a name typed, Save (the .map and its 128×128 .tga), a
// name another map has (the sheet asks, Replace); the saved map opened from the list; the Play
// settings (faction and start pop-ups, the AI stepper, the difficulty buttons, the game app typed),
// an unsaved edit, the toolbar's Play (the game process runs with the right arguments on the test
// copy), ⌘R (the first game closed, a second started), Map > Stop Game; the Objects mode: search,
// a row, its model, an animation from the pop-up, play/pause, Hidden parts, Export GIF (its save
// panel opens with the suggested name); Game Folders' items open their panels. Not driven (they
// run out of process): what is chosen in the system open and save panels; the check closes them.
import AppKit
import ApplicationServices
import QuartzCore
import simd

@MainActor
enum UICheck {
    struct Request {
        let zh: String, base: String, map: String, template: String, save: String, app: String, out: String
    }
    static var request: Request?
    static var diagnostic = ""

    static func parseArguments() {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--ui-check") else { return }
        let values = Array(arguments[(at + 1)...])
        guard values.count == 7 else {
            FileHandle.standardError.write("usage: --ui-check <zh> <base> <map> <template> <save name> <game app> <out.txt>\n"
                .data(using: .utf8)!)
            exit(2)
        }
        request = Request(zh: values[0], base: values[1], map: values[2], template: values[3], save: values[4],
                          app: values[5], out: values[6])
        // The user Maps folder as another session left it: no map of this check's names, and one
        // map someone else saved under the name the check then types (Save As asks before it
        // replaces it).
        let maps = EditorModel.userMapsFolder(forGame: values[0])
        for name in [values[4], values[4] + "Taken", "__EditorTest"] {
            try? FileManager.default.removeItem(at: maps.appendingPathComponent(name, isDirectory: true))
        }
        let taken = maps.appendingPathComponent(values[4] + "Taken", isDirectory: true)
        try? FileManager.default.createDirectory(at: taken, withIntermediateDirectories: true)
        try? Data("not a map".utf8).write(to: taken.appendingPathComponent(values[4] + "Taken.map"))
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
    /// The text field the accessibility element `id` is (SwiftUI's fields have no placeholder string
    /// on the view).
    static func field(id: String) -> (NSTextField, NSWindow)? {
        guard let element = axFind(id), let rect = axFrame(element) else { return nil }
        let point = NSPoint(x: rect.midX, y: rect.midY)
        for (view, window) in views() {
            guard let field = view as? NSTextField, field.isEditable else { continue }
            if window.convertToScreen(windowFrame(of: field)).contains(point) { return (field, window) }
        }
        return nil
    }

    static func fill(_ placeholder: String, id: String? = nil, with text: String, returnKey: Bool = true) async -> Bool {
        guard let (field, window) = id.flatMap({ field(id: $0) }) ?? field(placeholder) else {
            diagnostic = "  text fields: " + views().compactMap { $0.0 as? NSTextField }.map {
                "\(type(of: $0)) '\($0.placeholderString ?? "-")' '\($0.stringValue)' editable \($0.isEditable)"
            }.joined(separator: "; ")
            return false
        }
        // Scrolled into view first, as a user scrolls a form to a field below the fold.
        field.scrollToVisible(field.bounds)
        try? await Task.sleep(nanoseconds: 100_000_000)
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

    /// Chooses the item titled `title` in the menu a control opens (a SwiftUI Picker's pop-up, a
    /// Menu): a click on the control, like a user's; once AppKit tracks its menu, the menu is closed
    /// and the item performed, what AppKit does after a click on the item.
    final class MenuPick: NSObject {
        nonisolated(unsafe) static let shared = MenuPick()
        var wanted: String?
        var titles: [String] = []
        var picked = false
        private weak var menu: NSMenu?
        private var index = -1

        override init() {
            super.init()
            NotificationCenter.default.addObserver(self, selector: #selector(began(_:)),
                                                   name: NSMenu.didBeginTrackingNotification, object: nil)
            NotificationCenter.default.addObserver(self, selector: #selector(ended(_:)),
                                                   name: NSMenu.didEndTrackingNotification, object: nil)
        }

        @objc func began(_ note: Notification) {
            guard let wanted, let menu = note.object as? NSMenu else { return }
            titles = menu.items.map(\.title)
            self.wanted = nil
            if let at = menu.items.firstIndex(where: { $0.title == wanted }) {
                self.menu = menu
                index = at
            }
            // Once the tracking loop runs (cancelling it while it is being set up leaves it running).
            RunLoop.main.perform(inModes: [.eventTracking, .default, .modalPanel]) { menu.cancelTracking() }
        }

        @objc func ended(_ note: Notification) {
            guard let menu = note.object as? NSMenu, menu === self.menu, index >= 0 else { return }
            let at = index
            index = -1
            self.menu = nil
            picked = true
            menu.performActionForItem(at: at)
        }
    }

    // SwiftUI builds its accessibility tree only for an accessibility client, so controls are
    // found the way VoiceOver finds them: through the Accessibility API on the app's own process
    // (AXUIElement; requests to the own process are answered in place, on the main thread here).

    static func axValue(_ element: AXUIElement, _ name: String) -> CFTypeRef? {
        var value: CFTypeRef?
        return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
    }

    /// The element whose AXIdentifier is `id` (a SwiftUI .accessibilityIdentifier), in any window.
    static func axFind(_ id: String) -> AXUIElement? {
        var queue: [AXUIElement] = [AXUIElementCreateApplication(getpid())]
        var visited = 0
        while !queue.isEmpty && visited < 300000 {
            let node = queue.removeFirst()
            visited += 1
            if (axValue(node, "AXIdentifier") as? String) == id { return node }
            if let children = axValue(node, kAXChildrenAttribute) as? [AXUIElement] { queue.append(contentsOf: children) }
        }
        return nil
    }

    /// An element's frame in screen coordinates (AppKit's: origin bottom left).
    static func axFrame(_ element: AXUIElement) -> NSRect? {
        guard let position = axValue(element, kAXPositionAttribute), let size = axValue(element, kAXSizeAttribute) else { return nil }
        var origin = CGPoint.zero, extent = CGSize.zero
        guard AXValueGetValue(position as! AXValue, .cgPoint, &origin), AXValueGetValue(size as! AXValue, .cgSize, &extent),
              extent.width > 0, extent.height > 0 else { return nil }
        let top = NSScreen.screens.first?.frame.maxY ?? 0
        return NSRect(x: origin.x, y: top - origin.y - extent.height, width: extent.width, height: extent.height)
    }

    /// The frontmost window at a screen point.
    static func window(at point: NSPoint) -> NSWindow? {
        NSApp.orderedWindows.first { $0.isVisible && $0.frame.contains(point) }
    }

    /// Clicks the middle of a screen rectangle with posted mouse events (the real event path).
    @discardableResult
    static func click(screen rect: NSRect, at fraction: CGPoint = CGPoint(x: 0.5, y: 0.5)) -> Bool {
        let point = NSPoint(x: rect.minX + rect.width * fraction.x, y: rect.minY + rect.height * fraction.y)
        guard let window = window(at: point) else { return false }
        window.makeKeyAndOrderFront(nil)
        click(at: window.convertPoint(fromScreen: point), window: window)
        return true
    }

    /// Opens the pop-up or menu `id` with the Accessibility API's press (what VoiceOver does; a
    /// posted click would leave the control's own mouse tracking waiting for a button release that
    /// the menu took) and chooses `title` in it.
    static func pick(_ title: String, from id: String, titled label: String? = nil) async -> Bool {
        let app = AXUIElementCreateApplication(getpid())
        guard let element = axFind(id) ?? label.flatMap({ label in axFirst(under: app, { node in
            let role = axValue(node, kAXRoleAttribute) as? String
            return (role == (kAXMenuButtonRole as String) || role == (kAXPopUpButtonRole as String) || role == (kAXButtonRole as String))
                && axText(node).contains(label) }) }) else {
            diagnostic = "  no control \(id)"
            return false
        }
        let menu = MenuPick.shared
        menu.picked = false
        menu.titles = []
        menu.wanted = title
        let pressed = AXUIElementPerformAction(element, kAXPressAction as CFString)
        if pressed != .success && !menu.picked {
            // A Menu's button may only offer AXShowMenu.
            _ = AXUIElementPerformAction(element, kAXShowMenuAction as CFString)
        }
        let start = Date()
        while !menu.picked && Date().timeIntervalSince(start) < 3 {
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        menu.wanted = nil
        diagnostic = "  \(id)'s menu: " + menu.titles.joined(separator: ", ")
        return menu.picked
    }

    /// A menu bar item, performed the way a click on it is.
    static func menuBar(_ menu: String, _ item: String) -> Bool {
        guard let submenu = NSApp.mainMenu?.items.first(where: { $0.title == menu })?.submenu,
              let index = submenu.items.firstIndex(where: { $0.title == item }) else { return false }
        submenu.performActionForItem(at: index)
        return true
    }

    /// Scrolls the element into view, as a user scrolls a form to a control below the fold.
    static func scrollToVisible(_ element: AXUIElement) {
        _ = AXUIElementPerformAction(element, "AXScrollToVisible" as CFString)
    }

    /// Clicks the control `id` (a button, a checkbox) at its middle, scrolled into view; the
    /// Accessibility press when it cannot be shown (an overflowing toolbar).
    @discardableResult
    static func press(_ id: String) -> Bool {
        guard let element = axFind(id) else {
            diagnostic = "  no control \(id)"
            return false
        }
        scrollToVisible(element)
        if AXUIElementPerformAction(element, kAXPressAction as CFString) == .success { return true }
        guard let rect = axFrame(element) else { return false }
        return click(screen: rect)
    }

    /// The first element under `root` (itself included) that matches.
    static func axFirst(under root: AXUIElement, limit: Int = 300000, _ match: (AXUIElement) -> Bool) -> AXUIElement? {
        var queue = [root]
        var visited = 0
        while !queue.isEmpty && visited < limit {
            let node = queue.removeFirst()
            visited += 1
            if match(node) { return node }
            queue.append(contentsOf: (axValue(node, kAXChildrenAttribute) as? [AXUIElement]) ?? [])
        }
        return nil
    }

    static func axText(_ node: AXUIElement) -> [String] {
        [kAXValueAttribute, kAXTitleAttribute, kAXDescriptionAttribute].compactMap { axValue(node, $0) as? String }
    }

    /// Presses the segment titled `title` of the segmented picker `id`.
    static func pressSegment(_ title: String, in id: String) -> Bool {
        guard let picker = axFind(id),
              let segment = axFirst(under: picker, limit: 200, { $0 != picker && axText($0).contains(title) }) else {
            diagnostic = "  no segment \(title) in \(id)"
            return false
        }
        return AXUIElementPerformAction(segment, kAXPressAction as CFString) == .success
    }

    /// Selects the row of a list or table that shows `text` with a click on it.
    static func clickRow(showing text: String) -> Bool {
        let app = AXUIElementCreateApplication(getpid())
        guard let row = axFirst(under: app, { node in
            (axValue(node, kAXRoleAttribute) as? String) == (kAXRowRole as String)
                && axFirst(under: node, limit: 50, { axText($0).contains(text) }) != nil
        }) else {
            diagnostic = "  no row showing \(text)"
            return false
        }
        scrollToVisible(row)
        guard let rect = axFrame(row) else { return false }
        return click(screen: rect)
    }

    static func exists(_ id: String) -> Bool { axFind(id) != nil }

    /// Presses `id` from the run loop rather than from this check's task: a button that runs a
    /// modal panel does not return until the panel closes, and the check's task (on the main actor)
    /// could not run meanwhile to look at the panel.
    static func pressFromRunLoop(_ id: String) -> Bool {
        guard let element = axFind(id) else {
            diagnostic = "  no control \(id)"
            return false
        }
        scrollToVisible(element)
        RunLoop.main.perform(inModes: [.default]) { _ = AXUIElementPerformAction(element, kAXPressAction as CFString) }
        return true
    }

    /// Whether some element shows `text` (a Text's value or title).
    static func exists(value text: String) -> Bool {
        var queue: [AXUIElement] = [AXUIElementCreateApplication(getpid())]
        var visited = 0
        while !queue.isEmpty && visited < 300000 {
            let node = queue.removeFirst()
            visited += 1
            if (axValue(node, kAXValueAttribute) as? String) == text || (axValue(node, kAXTitleAttribute) as? String) == text
                || (axValue(node, kAXDescriptionAttribute) as? String) == text { return true }
            if let children = axValue(node, kAXChildrenAttribute) as? [AXUIElement] { queue.append(contentsOf: children) }
        }
        return false
    }

    /// One step up (or down) of a stepper, with its Accessibility increment (decrement) action.
    static func step(_ id: String, up: Bool = true) -> Bool {
        guard let element = axFind(id) else {
            diagnostic = "  no control \(id)"
            return false
        }
        let stepper = axFirst(under: element, limit: 100, {
            (axValue($0, kAXRoleAttribute) as? String) == (kAXIncrementorRole as String) }) ?? element
        return AXUIElementPerformAction(stepper, (up ? kAXIncrementAction : kAXDecrementAction) as CFString) == .success
    }

    /// The system panel the app runs modally (an open or save panel), once it is up.
    static func modalPanel(timeout: Double = 20) async -> NSSavePanel? {
        let start = Date()
        while Date().timeIntervalSince(start) < timeout {
            if let panel = NSApp.modalWindow as? NSSavePanel { return panel }
            // A panel shown as a sheet on a window (Save As's Other Location…).
            if let panel = NSApp.windows.compactMap({ $0.attachedSheet as? NSSavePanel }).first(where: \.isVisible) { return panel }
            if let panel = NSApp.windows.first(where: { $0.isVisible && $0 is NSSavePanel }) as? NSSavePanel { return panel }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return nil
    }

    /// Closes a system panel the way its Cancel button does (its view runs out of process).
    static func closePanel(_ panel: NSSavePanel) async {
        if NSApp.modalWindow === panel { NSApp.abortModal() } else { panel.cancel(nil) }
        let start = Date()
        while (panel.isVisible || NSApp.modalWindow === panel) && Date().timeIntervalSince(start) < 10 {
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
    }

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

            log("accessibility: this process is \(AXIsProcessTrusted() ? "" : "not ")trusted (own-process requests need no trust)")
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
            /// Until the map view keeps its size (the inspector column opens and closes animated, and a
            /// point computed before that is somewhere else after it).
            @MainActor func layoutSettled() async {
                var last = view.bounds.size, stable = 0
                let start = Date()
                while stable < 3 && Date().timeIntervalSince(start) < 5 {
                    try? await Task.sleep(nanoseconds: 150_000_000)
                    stable = view.bounds.size == last ? stable + 1 : 0
                    last = view.bounds.size
                }
            }
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

            // 2. The owner pop-up, then a ⌃-click places a second one with that owner.
            let ownerBefore = editor.mapEditor.team
            if let other = editor.mapPlayers.first(where: { $0.team != ownerBefore }) {
                let picked = await pick(other.label, from: "placement.owner")
                await settle()
                check("the placement bar's Owner pop-up chose \(other.label)", picked && editor.mapEditor.team == other.team)
                if !picked { log(diagnostic) }
            } else {
                check("the map has a second player to own objects", false)
            }
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

            if let turned = object(first.id) {
                post(key: "}", code: 30, modifiers: .shift, window: window)
                await settle()
                check("} turned it by 1° more", object(first.id).map { abs($0.angle - turned.angle - .pi / 180) < 0.001 } == true)
                post(key: "{", code: 33, modifiers: .shift, window: window)
                post(key: "[", code: 33, window: window)
                await settle()
                check("{ and [ turned it back by 16°", object(first.id).map { abs($0.angle - turned.angle + .pi * 15 / 180) < 0.001 } == true)
                post(key: "]", code: 30, window: window)
                post(key: "}", code: 30, modifiers: .shift, window: window)
                await settle()
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
            // ⌘A selects all (the inspector of every object takes a while to show).
            post(key: "a", code: 0, modifiers: .command, window: window)
            await wait("the selection of all", timeout: 10) { editor.mapSelection.count == editor.mapScene.objects.count }
            check("⌘A selected all \(editor.mapScene.objects.count) objects", editor.mapSelection.count == editor.mapScene.objects.count)
            await settle(1.5)
            click(at: windowPoint(empty), window: window)
            await wait("the selection to clear", timeout: 10) { editor.mapSelection.isEmpty }
            await settle(1)  // the inspector closes, the map view widens
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
            await layoutSettled()
            if let one = selectedNow.first, let p = pointOn(one) {
                click(at: windowPoint(p), window: window)
                await settle(0.6)
                let pressed = press("inspector.delete")
                await settle()
                check("the inspector's Delete removed the clicked object", pressed && object(one) == nil)
                if object(one) == nil {
                    post(key: "z", code: 6, modifiers: .command, window: window)
                    await settle()
                }
                check("⌘Z brought it back", object(one) != nil)
            }

            // 8. The inspector: click the first object, type a height and a health, choose an owner.
            await layoutSettled()
            if let p = pointOn(first.id) {
                click(at: windowPoint(p), window: window)
            }
            await wait("the click's selection", timeout: 10) { editor.mapSelection == [first.id] }
            await settle(0.6)
            if editor.mapSelection != [first.id] {
                log("  first object at \(String(describing: pointOn(first.id))), selection \(editor.mapSelection.prefix(5)) (\(editor.mapSelection.count)), gesture '\(editor.mapGesture.kind)', note '\(editor.mapEditor.note)', windows \(NSApp.windows.filter(\.isVisible).map { "\(type(of: $0))" }), key \(String(describing: NSApp.keyWindow.map { type(of: $0) }))")
            }
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
            @MainActor func property(_ key: String) -> MapObjectProperty? {
                editor.mapObjectProperties(of: first.id).first { $0.key == key }
            }
            check("angle typed into the inspector", await fill("Angle", with: "30"))
            await settle()
            check("the object is turned to 30°", object(first.id).map { abs($0.angle - .pi / 6) < 0.001 } == true)
            check("name typed into the inspector", await fill("Name", with: "UICheck"))
            await settle()
            check("its objectName is UICheck", property("objectName")?.value == "UICheck")
            check("script typed into the inspector", await fill("Script", with: "UICheckScript"))
            await settle()
            check("its objectScriptAttachment is UICheckScript", property("objectScriptAttachment")?.value == "UICheckScript")
            let angleTyped = object(first.id)?.angle ?? 0
            check("the inspector's angle stepper is clicked up", step("inspector.angleStep"))
            await settle()
            check("it turned the object by 15°", object(first.id).map { abs($0.angle - angleTyped - .pi / 12) < 0.001 } == true)
            _ = step("inspector.angleStep", up: false)
            await settle()
            let indestructible = property("objectIndestructible")?.value == "true"
            check("the Indestructible checkbox is clicked", press("inspector.flag.objectIndestructible"))
            await settle()
            check("objectIndestructible is \(!indestructible)", (property("objectIndestructible")?.value == "true") == !indestructible)
            if let player = editor.mapPlayers.first(where: { $0.team != object(first.id)?.owner }) {
                let picked = await pick(player.label, from: "inspector.owner")
                await settle()
                check("the inspector's Owner pop-up chose \(player.label)", picked && object(first.id)?.owner == player.team)
                if !picked { log(diagnostic) }
            }
            // A new property: its key and value typed, its type chosen in the pop-up, Add.
            // The form's last rows exist once they are scrolled into view (a lazy list).
            @MainActor func reveal(_ id: String) async {
                if let element = axFind(id) { scrollToVisible(element) }
                await settle()
            }
            check("its type chosen in the pop-up", await pick("int", from: "inspector.newType"))
            await reveal("inspector.newKey")
            var keyTyped = await fill("key", id: "inspector.newKey", with: "uiCheckKey", returnKey: false)
            await settle()
            if field(id: "inspector.newKey")?.0.stringValue != "uiCheckKey" {
                keyTyped = await fill("key", id: "inspector.newKey", with: "uiCheckKey", returnKey: false)  // once more
                await settle()
            }
            check("a new property's key typed", keyTyped && field(id: "inspector.newKey")?.0.stringValue == "uiCheckKey")
            if field(id: "inspector.newKey") == nil { log(diagnostic) }
            await reveal("inspector.newValue")
            check("its value typed", await fill("value", id: "inspector.newValue", with: "7", returnKey: false))
            await settle()
            log("  new property fields: key '\(field(id: "inspector.newKey")?.0.stringValue ?? "?")' value '\(field(id: "inspector.newValue")?.0.stringValue ?? "?")'")
            check("Add is clicked", press("inspector.add"))
            await settle()
            check("the property uiCheckKey is an int 7", property("uiCheckKey").map { $0.type == "int" && $0.value == "7" } == true)
            await reveal("inspector.property.uiCheckKey")
            check("its value field is typed into", await fill("uiCheckKey", id: "inspector.property.uiCheckKey", with: "8"))
            await settle()
            check("uiCheckKey is 8", property("uiCheckKey")?.value == "8")
            check("its remove button is clicked", press("inspector.remove.uiCheckKey"))
            await settle()
            check("uiCheckKey is removed", property("uiCheckKey") == nil)
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
            let path = editor.mapScene.objects.suffix(3).map(\.id)
            if path.count == 3, let a = pointOn(path[0]), let c = pointOn(path[2]) {
                click(at: windowPoint(a), window: window)
                await settle()
                click(at: windowPoint(c), window: window, modifiers: .shift)
                await settle(0.6)
                check("the inspector's Link Waypoints is clicked", press("inspector.link"))
                await settle()
                let links = { editor.mapScene.links.filter { $0.kind == "waypoint" }.count }
                check("the first and the last are linked too", links() == linksBefore + 3)
                check("the bar's Undo is clicked", press("placement.undo"))
                await settle()
                check("Undo took the link away", links() == linksBefore + 2)
                check("the bar's Redo is clicked", press("placement.redo"))
                await settle()
                check("Redo put it back", links() == linksBefore + 3)
                press("placement.undo")
                await settle()
                check("Undo again: the path as placed", links() == linksBefore + 2)
            } else {
                check("the waypoints are in view", false)
            }
            check("the Road button chose the road tool (\(editor.mapEditor.roadType))", await tool(2, "road")
                  && !editor.mapEditor.roadType.isEmpty)
            let escAt = CGPoint(x: center.x - 40, y: center.y - 160)
            PerfHarness.post(mouse: .rightMouseDown, at: windowPoint(escAt), window: window)
            PerfHarness.post(mouse: .rightMouseUp, at: windowPoint(escAt), window: window)
            await settle()
            let started = editor.mapEditor.roadStart != nil
            post(key: "\u{1B}", code: 53, window: window)
            await settle()
            check("Esc cancelled a started road", started && editor.mapEditor.roadStart == nil)
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
            check("the Bridge button chose the bridge tool (\(editor.mapEditor.roadType))", clickSegment("Bridge"))
            await settle()
            check("with a bridge type", editor.mapEditor.tool == "bridge"
                  && editor.roadTypes.contains { $0.bridge && $0.name == editor.mapEditor.roadType })
            _ = clickSegment("Road")
            await settle()
            let roads = editor.roadTypes.filter { !$0.bridge }
            if roads.count > 1, let other = roads.first(where: { $0.name != editor.mapEditor.roadType }) {
                check("the road type pop-up chose \(other.name)", await pick(other.name, from: "placement.road"))
                await settle()
                check("the store's road type is \(other.name)", editor.mapEditor.roadType == other.name)
            }
            _ = clickSegment("Object")
            await settle()
            let expected = editor.mapScene.objects.count

            // 10. The map view's checkboxes and camera.
            for (id, flag) in [("map.showModels", \MapViewState.showModels), ("map.showMarkers", \MapViewState.showMarkers),
                               ("map.showWater", \MapViewState.showWater)] {
                let before = state[keyPath: flag]
                press(id)
                await settle()
                let toggled = state[keyPath: flag] == !before
                press(id)
                await settle()
                check("the \(id) checkbox turns it off and on", toggled && state[keyPath: flag] == before)
            }
            let targetBefore = state.target, yawBefore = state.yaw, distanceBefore = state.distance
            await drag(from: windowPoint(empty), by: 60)
            check("a drag on empty ground pans the camera", state.target != targetBefore)
            await drag(from: windowPoint(empty), by: 60, modifiers: .option)
            check("⌥-drag turns it", state.yaw != yawBefore)
            // Scroll events as the devices make them (CGEvent, dispatched by the window): a trackpad's
            // two-finger swipe (pixel units, continuous: precise deltas) pans; ⌘ with it, or a mouse
            // wheel (line units), zooms around the ground under the cursor.
            @discardableResult @MainActor func scroll(units: CGScrollEventUnit, continuous: Bool, by delta: Int32, flags: CGEventFlags = [],
                                   at viewPoint: CGPoint? = nil) -> CGPoint? {
                guard let event = CGEvent(scrollWheelEvent2Source: nil, units: units, wheelCount: 2, wheel1: delta,
                                          wheel2: delta / 2, wheel3: 0) else { return nil }
                event.setIntegerValueField(.scrollWheelEventIsContinuous, value: continuous ? 1 : 0)
                event.flags = flags
                let at = window.convertPoint(toScreen: windowPoint(viewPoint ?? center))
                event.location = CGPoint(x: at.x, y: (NSScreen.screens.first?.frame.height ?? 0) - at.y)
                guard let ns = NSEvent(cgEvent: event) else { return nil }
                window.sendEvent(ns)
                // Where the event really is (the posted location can land a few points off the
                // asked one): the view point the app saw.
                let local = view.convert(ns.locationInWindow, from: nil)
                return CGPoint(x: local.x, y: view.bounds.height - local.y)
            }
            var camTarget = state.target, camDistance = state.distance
            scroll(units: .pixel, continuous: true, by: -40)
            await settle()
            check("a two-finger swipe pans (target \(camTarget) -> \(state.target)) and does not zoom",
                  state.target != camTarget && state.distance == camDistance)
            // Off the middle of the view, so a zoom around the middle would move the ground under it.
            let aside = CGPoint(x: view.bounds.width * 0.3, y: view.bounds.height * 0.75)
            @MainActor func zoomKeepsGround(_ what: String, _ zoom: () -> CGPoint?) async {
                // A wheel event of 0 lines first: it changes nothing and tells where events posted
                // there land (a few points off the asked place).
                guard let at = scroll(units: .line, continuous: false, by: 0, at: aside) else {
                    return check("\(what): the scroll event was posted", false)
                }
                await settle()
                let distance = state.distance, groundBefore = ground(at)
                let landed = zoom()
                await settle()
                let groundAfter = ground(landed ?? at)
                let drift = groundBefore.flatMap { b in groundAfter.map { simd_distance(SIMD2(b.x, b.y), SIMD2($0.x, $0.y)) } }
                check("\(what) zooms (distance \(distance) -> \(state.distance)) around the ground under the cursor "
                      + "(it moved \(drift.map { String(format: "%.2f", $0) } ?? "?") of \(String(format: "%.0f", distance)))",
                      state.distance != distance && (drift ?? .infinity) < max(distance * 0.002, 0.5))
            }
            // Back to where the view looked before the swipe, so the point aside is on the map.
            state.target = camTarget
            await settle()
            await zoomKeepsGround("a mouse wheel") { scroll(units: .line, continuous: false, by: -3, at: aside) }
            await zoomKeepsGround("⌘ with a two-finger swipe") {
                scroll(units: .pixel, continuous: true, by: 40, flags: .maskCommand, at: aside)
            }
            _ = distanceBefore

            // Save Map As: File > Save Map As… opens the sheet, Cancel closes it.
            let saveName = request.save
            let maps = EditorModel.userMapsFolder(forGame: request.zh)
            @MainActor func sheetUp() -> Bool { window.attachedSheet?.isVisible == true }
            check("File > Save Map As… is chosen", menuBar("File", "Save Map As…"))
            await wait("the sheet", timeout: 5) { sheetUp() }
            check("the Save Map As sheet shows, with the map's name", sheetUp() && field("Name of the map")?.0.stringValue == editor.mapScene.name)
            check("its Cancel is clicked", press("save.cancel"))
            await wait("the sheet to close", timeout: 5) { !sheetUp() }
            check("Cancel closed it and saved nothing", !sheetUp() && editor.mapEdit.savedPath.isEmpty && editor.mapEdit.edited)

            // The bar's Save As…, a name typed, Save.
            @MainActor func saveAs(_ name: String) async -> Bool {
                guard press("placement.saveAs") else { return false }
                await wait("the sheet", timeout: 5) { sheetUp() }
                guard sheetUp(), await fill("Name of the map", with: name, returnKey: false) else { return false }
                await settle()
                return press("save.save")
            }
            check("Save As… opened the sheet, \(saveName) typed, Save clicked", await saveAs(saveName))
            let target = maps.appendingPathComponent(saveName, isDirectory: true).appendingPathComponent(saveName + ".map").path
            await wait("save", timeout: 20) { editor.mapEdit.savedPath == target }
            await wait("the sheet to close", timeout: 5) { !sheetUp() }
            let savedPath = editor.mapEdit.savedPath
            check("saved as \(target): \(editor.mapEditor.note)", savedPath == target && !editor.mapEdit.edited && !sheetUp())
            @MainActor func previewSize(_ map: String) -> Int {
                let preview = URL(fileURLWithPath: map).deletingPathExtension().appendingPathExtension("tga")
                return (try? FileManager.default.attributesOfItem(atPath: preview.path)[.size] as? Int) ?? 0
            }
            check("with its 128×128 preview \(saveName).tga (\(previewSize(target)) bytes)",
                  previewSize(target) == 18 + 128 * 128 * 4 + 26)
            check("the .map is the edited map (\((try? Data(contentsOf: URL(fileURLWithPath: target)).count) ?? 0) bytes)",
                  (try? Data(contentsOf: URL(fileURLWithPath: target)).prefix(4)) == Data("CkMp".utf8))

            // A name another map has: the sheet asks, Replace replaces it.
            let taken = saveName + "Taken"
            let takenPath = maps.appendingPathComponent(taken, isDirectory: true).appendingPathComponent(taken + ".map").path
            check("Save As… with the name \(taken) another map has", await saveAs(taken))
            await wait("the question", timeout: 10) { editor.saveConflict == takenPath }
            check("the sheet asks before it replaces \(taken)", sheetUp() && exists("save.conflict")
                  && (try? String(contentsOfFile: takenPath, encoding: .utf8)) == "not a map")
            check("its Replace is clicked", press("save.save"))
            await wait("replace", timeout: 20) { editor.mapEdit.savedPath == takenPath }
            await wait("the sheet to close", timeout: 5) { !sheetUp() }
            check("Replace saved over it", editor.mapEdit.savedPath == takenPath && !sheetUp()
                  && (try? Data(contentsOf: URL(fileURLWithPath: takenPath)).prefix(4)) == Data("CkMp".utf8))
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

            // 11b. Game Folders (before Play: the game's full screen can leave the window narrower, its toolbar overflowing): its items open the system panels (closed again, see Export GIF).
            for item in ["Zero Hour Folder…", "Base Generals Folder…", "Open Map File…"] {
                let menuTask = Task { @MainActor in await pick(item, from: "toolbar.gameFolders", titled: "Game Folders") }
                let panel = await modalPanel()
                let folders = item != "Open Map File…"
                check("Game Folders > \(item) opened \(folders ? "a folder" : "a .map file") panel",
                      (panel as? NSOpenPanel).map { $0.canChooseDirectories == folders } == true)
                if panel == nil { log(diagnostic) }
                if let panel { await closePanel(panel) }
                _ = await menuTask.value
                await settle()
            }
            check("Open Map File… switched to the Maps mode", UserDefaults.standard.string(forKey: "editorMode") == EditorMode.maps.rawValue)
            // 12. Play settings: the AI stepper, the difficulty buttons, the game app typed, the Default
            // button, the faction and start pop-ups.
            @MainActor func settingsShown() async -> Bool {
                // The button toggles the popover: pressed again when the first press did not show it.
                for _ in 0..<3 {
                    if exists("play.faction") { return true }
                    window.makeKeyAndOrderFront(nil)
                    press("toolbar.playSettings")
                    let start = Date()
                    while !exists("play.faction") && Date().timeIntervalSince(start) < 4 {
                        try? await Task.sleep(nanoseconds: 100_000_000)
                    }
                }
                return exists("play.faction")
            }
            check("the toolbar's Play Settings shows the settings", await settingsShown())
            if editor.play.aiCount >= 7 { _ = step("play.ai", up: false); await settle() }
            let aiStart = editor.play.aiCount
            check("the AI stepper's up arrow is clicked", step("play.ai"))
            await settle()
            check("AI opponents \(aiStart) -> \(aiStart + 1)", editor.play.aiCount == aiStart + 1)
            _ = await settingsShown()
            check("the difficulty's Hard is pressed", pressSegment("Hard", in: "play.difficulty"))
            await settle()
            check("the store's AI difficulty is hard", editor.play.aiDifficulty == "hard")
            _ = await settingsShown()
            check("the game app typed", await fill("Game app", with: request.app))
            await settle()
            check("the store's game app is \(request.app)", editor.play.app == request.app && editor.play.appResolved == request.app)
            _ = await settingsShown()
            check("Default is clicked", press("play.default"))
            await settle()
            check("Default: the app is ~/GeneralsX/Apps/GeneralsXZH.app", editor.play.app.isEmpty
                  && editor.play.appResolved.hasSuffix("/GeneralsX/Apps/GeneralsXZH.app"))
            _ = await settingsShown()
            check("the game app typed again", await fill("Game app", with: request.app))
            await settle()
            _ = await settingsShown()
            check("Faction: FactionChina chosen", await pick("FactionChina", from: "play.faction"))
            await settle()
            check("the store's faction is FactionChina", editor.play.faction == "FactionChina")
            _ = await settingsShown()
            check("Start position: 2 chosen", await pick("2", from: "play.start"))
            await settle()
            check("the store's start position is 2", editor.play.startPosition == 2)
            check("the settings are kept: \(editor.play.app), FactionChina, \(aiStart + 1) AI, hard",
                  editor.play.app == request.app && editor.play.faction == "FactionChina" && editor.play.aiCount == aiStart + 1
                  && editor.play.aiDifficulty == "hard")
            if exists("play.faction") {
                post(key: "\u{1B}", code: 53, window: NSApp.keyWindow ?? window)  // closes the popover
                await settle()
            }
            window.makeKeyAndOrderFront(nil)

            // 13. An unsaved edit, then the toolbar's Play: the test copy and the game started on it.
            _ = clickSegment("Object")
            await settle()
            let playAt = CGPoint(x: center.x + 60, y: center.y - 30)
            PerfHarness.post(mouse: .leftMouseDown, at: windowPoint(playAt), window: window, modifiers: .control)
            PerfHarness.post(mouse: .leftMouseUp, at: windowPoint(playAt), window: window, modifiers: .control)
            await settle()
            let objectsToPlay = editor.mapScene.objects.count
            check("an unsaved edit before Play", editor.mapEdit.edited && objectsToPlay == expected + 1)
            let runsBefore = editor.play.runs
            let savedBeforePlay = editor.mapEdit.savedPath
            check("the toolbar's Play is clicked", press("toolbar.play"))
            await wait("the game", timeout: 20) { editor.play.runs == runsBefore + 1 && editor.play.state != "idle" }
            let play = editor.play
            let testCopy = maps.appendingPathComponent("__EditorTest", isDirectory: true)
                .appendingPathComponent("__EditorTest.map").path
            let arguments = play.command.components(separatedBy: "\t")
            log("  started: " + arguments.joined(separator: " ") + " (pid \(play.pid)), log " + play.logPath)
            check("the game runs (\(play.state): \(play.message))", play.state == "running" && play.pid > 0 && kill(pid_t(play.pid), 0) == 0)
            check("started from the app's run.sh", arguments.first == URL(fileURLWithPath: request.app)
                  .appendingPathComponent("Contents/MacOS/run.sh").path)
            check("with -testMap on the test copy and the settings", Array(arguments.dropFirst()) == [
                "-testMap", testCopy, "-testMapFaction", "FactionChina", "-testMapAI", "\(aiStart + 1)",
                "-testMapAIDifficulty", "hard", "-testMapStart", "2"])
            check("the test copy holds the unsaved edit, the map stays unsaved",
                  FileManager.default.fileExists(atPath: testCopy) && editor.mapEdit.edited && editor.mapEdit.savedPath == savedBeforePlay)
            // What the game says once its client runs (it prints this line when the map starts).
            await wait("the game to start the map", timeout: 90) {
                (try? String(contentsOfFile: play.logPath, encoding: .utf8))?.contains("TEST_MAP start") == true
                    || editor.play.state != "running"
            }
            let gameLog = (try? String(contentsOfFile: play.logPath, encoding: .utf8)) ?? ""
            let startLine = gameLog.components(separatedBy: "\n").first { $0.contains("TEST_MAP start") }
            let lastLines = gameLog.components(separatedBy: "\n").suffix(3).joined(separator: " | ")
            check("the game started the test copy: \(startLine ?? "no TEST_MAP line; last: " + lastLines)",
                  startLine?.contains("__editortest.map") == true && startLine?.contains("faction=FactionChina") == true)
            log("  game state \(editor.play.state), exit code \(editor.play.exitCode)")

            // ⌘R: Play again closes the first game and starts a second one.
            let firstPid = editor.play.pid
            post(key: "r", code: 15, modifiers: .command, window: window)
            await wait("the second game", timeout: 20) { editor.play.runs == runsBefore + 2 }
            check("⌘R started the game again (pid \(editor.play.pid))", editor.play.runs == runsBefore + 2
                  && editor.play.state == "running" && editor.play.pid != firstPid)
            // The whole process group: run.sh runs the game as its child.
            await wait("the first game's processes to end", timeout: 10) { kill(-pid_t(firstPid), 0) != 0 }
            check("the first game is closed (its process group too)", firstPid > 0 && kill(-pid_t(firstPid), 0) != 0)
            let secondPid = editor.play.pid
            check("Map > Play Map is chosen", menuBar("Map", "Play Map"))
            await wait("the third game", timeout: 20) { editor.play.runs == runsBefore + 3 }
            check("Map > Play Map started it again", editor.play.runs == runsBefore + 3 && editor.play.state == "running"
                  && editor.play.pid != secondPid)
            let thirdPid = editor.play.pid
            check("Map > Stop Game is chosen", menuBar("Map", "Stop Game"))
            await wait("the game to end", timeout: 20) { editor.play.state != "running" && kill(-pid_t(thirdPid), 0) != 0 }
            check("the game ended (\(editor.play.message)), its process group too", editor.play.state == "exited"
                  && thirdPid > 0 && kill(-pid_t(thirdPid), 0) != 0)

            // 14. The Play settings' Choose… and the sheet's Other Location… open their system panels.
            let settingsUp = await settingsShown()
            check("the Play settings are shown again", settingsUp)
            if settingsUp {
                check("Choose… is pressed", pressFromRunLoop("play.choose"))
                let panel = await modalPanel()
                check("Play settings > Choose… opened an app panel", panel is NSOpenPanel)
                if let panel { await closePanel(panel) }
                await settle()
            }
            if exists("play.faction") {
                post(key: "\u{1B}", code: 53, window: NSApp.keyWindow ?? window)
                await settle()
            }
            window.makeKeyAndOrderFront(nil)
            if press("placement.saveAs") {
                await wait("the sheet", timeout: 5) { window.attachedSheet?.isVisible == true }
                check("Other Location… is pressed", pressFromRunLoop("save.other"))
                let panel = await modalPanel()
                check("Save As > Other Location… opened the save panel (in \(panel?.directoryURL?.path ?? "-"))",
                      panel != nil && panel?.directoryURL?.resolvingSymlinksInPath().path == maps.resolvingSymlinksInPath().path)
                if let panel { await closePanel(panel) }
                await settle()
            } else {
                check("Save As… is shown in the Maps mode", false)
            }
            // 15. Objects mode (last: its search field takes toolbar room): a side, the table sorted by a column, a search, a row, its model and
            // details, an animation, play/pause, Hidden parts, orbit and zoom, Export GIF.
            check("the toolbar's Objects segment is clicked", clickSegment("Objects"))
            await settle(1)
            @MainActor func objectTable(rows: Int) -> NSTableView? {
                views().compactMap { $0.0 as? NSTableView }.first { $0.numberOfRows == rows && $0.tableColumns.count >= 5 }
            }
            let sideName = editor.objects.first { !$0.side.isEmpty }?.side ?? ""
            let sideCount = editor.objects.filter { $0.side == sideName }.count
            @MainActor func clickElement(_ id: String) -> Bool {
                guard let element = axFind(id) else { diagnostic = "  no control \(id)"; return false }
                scrollToVisible(element)
                return axFrame(element).map { click(screen: $0) } ?? false
            }
            check("the side \(sideName) (\(sideCount)) is clicked in the sidebar", clickElement("objects.side." + sideName))
            await settle(1)
            let ofSide = editor.objects.filter { $0.side == sideName }.count
            await wait("the table of the side", timeout: 10) { objectTable(rows: ofSide) != nil }
            check("the table shows its \(ofSide) objects", objectTable(rows: ofSide) != nil)
            check("All objects is clicked", clickElement("objects.side.all"))
            await settle(1)
            if let table = objectTable(rows: editor.objects.count),
               let column = table.tableColumns.firstIndex(where: { $0.title == "Side" }), let header = table.headerView {
                let rect = header.convert(header.headerRect(ofColumn: column), to: nil)
                click(at: NSPoint(x: rect.midX, y: rect.midY), window: window)
                await settle(1)
                let sideKey = table.tableColumns[column].sortDescriptorPrototype?.key
                check("the Side column's header sorted the table by it (\(table.sortDescriptors.first?.key ?? "-"))",
                      sideKey != nil && table.sortDescriptors.first?.key == sideKey)
                click(at: NSPoint(x: header.convert(header.headerRect(ofColumn: 0), to: nil).midX, y: rect.midY), window: window)
                await settle(1)
            } else {
                check("the object table with every object is shown", false)
            }
            let previewObject = "AmericaInfantryRanger"
            let objectSearch = views().compactMap { $0.0 as? NSSearchField }.first { $0.placeholderString == "Name, model or KindOf" }
            if let objectSearch {
                let rect = windowFrame(of: objectSearch)
                click(at: NSPoint(x: rect.midX, y: rect.midY), window: window)
                await settle(0.2)
                post(key: "a", code: 0, modifiers: .command, window: window)
                typeText(previewObject, window: window)
                await settle(1)
            }
            check("the object table's search field is typed into", objectSearch != nil)
            let listedObjects = editor.objects.filter { $0.name.localizedCaseInsensitiveContains(previewObject)
                || $0.model.localizedCaseInsensitiveContains(previewObject)
                || $0.kindOf.localizedCaseInsensitiveContains(previewObject) }.sorted { $0.name < $1.name }
            check("the search left \(listedObjects.count) rows", objectTable(rows: listedObjects.count) != nil)
            check("its row \(previewObject) is clicked", clickRow(showing: previewObject))
            await wait("the model", timeout: 30) { editor.modelState == "ready" || editor.modelState == "error" }
            check("the preview shows its model (\(editor.preview.parts.count) parts, \(editor.animations.count) animations)",
                  editor.modelState == "ready" && !editor.preview.parts.isEmpty)
            if let item = editor.objects.first(where: { $0.name == previewObject }) {
                check("the details show its model \(item.model) and source \(item.sourceFile)",
                      exists(value: item.model) && exists(value: "\(item.sourceFile):\(item.line)"))
            }
            if let animation = editor.animations.first {
                check("the animation pop-up chose \(animation)", await pick(animation, from: "preview.animation"))
                await wait("the animation", timeout: 10) { editor.preview.animation == animation }
                check("the preview plays it (\(editor.preview.frameCount) frames)", editor.preview.animation == animation
                      && editor.preview.frameCount > 1)
            } else {
                check("the model has animations", false)
            }
            if let preview = PerfHarness.find(OrbitMTKView.self, in: content), let playback = preview.playback {
                let playing = playback.playing
                check("play/pause is clicked", press("preview.playPause"))
                await settle()
                check("it paused or played (\(playing) -> \(playback.playing))", playback.playing == !playing)
                press("preview.playPause")
                await settle()
                let hidden = playback.showHidden
                check("Hidden parts is clicked", press("preview.hiddenParts"))
                await settle()
                check("hidden parts shown \(playback.showHidden)", playback.showHidden == !hidden)
                let yaw = playback.yaw, zoom = playback.zoom
                let middle = NSPoint(x: windowFrame(of: preview).midX, y: windowFrame(of: preview).midY)
                PerfHarness.post(mouse: .leftMouseDown, at: middle, window: window)
                for i in 1...8 {
                    PerfHarness.post(mouse: .leftMouseDragged, at: NSPoint(x: middle.x + CGFloat(i * 6), y: middle.y), window: window)
                    try? await Task.sleep(nanoseconds: 16_000_000)
                }
                PerfHarness.post(mouse: .leftMouseUp, at: NSPoint(x: middle.x + 48, y: middle.y), window: window)
                await settle()
                check("a drag on the preview turns the model (yaw \(yaw) -> \(playback.yaw))", playback.yaw != yaw)
                if let scroll = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 1, wheel1: 30, wheel2: 0, wheel3: 0) {
                    let at = window.convertPoint(toScreen: middle)
                    scroll.location = CGPoint(x: at.x, y: (NSScreen.screens.first?.frame.height ?? 0) - at.y)
                    if let event = NSEvent(cgEvent: scroll) { window.sendEvent(event) }
                }
                await settle()
                check("scrolling zooms the preview (\(zoom) -> \(playback.zoom))", playback.zoom != zoom)
                // Export GIF: the menu's item opens the save panel with the suggested name; what is
                // chosen in it is not driven (it runs out of process), so the check closes it.
                let suggested = GifExport.suggestedName(object: editor.preview.name, animation: editor.preview.animation)
                let menuTask = Task { @MainActor in await pick(GifFormat.square.title, from: "preview.exportGif") }
                let panel = await modalPanel()
                check("Export GIF > \(GifFormat.square.title) opened the save panel for \(panel?.nameFieldStringValue ?? "-")",
                      panel?.nameFieldStringValue == suggested)
                if let panel { await closePanel(panel) }
                _ = await menuTask.value
            } else {
                check("the model preview is shown", false)
            }

            check("the game folder is still the one opened", editor.status.gameDir == request.zh)
            finish()
        }
    }
}
