// END Editor for macOS: a thin SwiftUI client of the C++ editor core (Editor/core).
// Commands go to the core through end_editor_send(); everything the UI shows is read from the
// core's SQLite store with the client's own read-only connection.
import AppKit
import SQLite3
import SwiftUI

struct ObjectItem: Identifiable, Hashable {
    let id: Int64
    let name: String
    let definition: String
    let parent: String
    let side: String
    let editorSorting: String
    let buildable: String
    let model: String
    let kindOf: String
    let sourceFile: String
    let line: Int
}

struct CoreStatus: Equatable {
    var state = "idle"
    var message = ""
    var gameDir = ""
    var objectCount: Int64 = 0
    var revision: Int64 = -1
}

/// Read side of the core's store.
final class StoreReader {
    private var db: OpaquePointer?

    init?(path: String) {
        guard sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nil) == SQLITE_OK else { return nil }
    }

    deinit { sqlite3_close(db) }

    private func rows(_ sql: String, _ read: (OpaquePointer) -> Void) {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK, let statement else { return }
        defer { sqlite3_finalize(statement) }
        while sqlite3_step(statement) == SQLITE_ROW { read(statement) }
    }

    private static func text(_ s: OpaquePointer, _ i: Int32) -> String {
        guard let c = sqlite3_column_text(s, i) else { return "" }
        return String(cString: c)
    }

    func status() -> CoreStatus? {
        var result: CoreStatus?
        rows("SELECT state, message, game_dir, object_count, revision FROM status WHERE id = 1") { s in
            result = CoreStatus(state: Self.text(s, 0), message: Self.text(s, 1), gameDir: Self.text(s, 2),
                                objectCount: sqlite3_column_int64(s, 3), revision: sqlite3_column_int64(s, 4))
        }
        return result
    }

    func objects() -> [ObjectItem] {
        var result: [ObjectItem] = []
        rows("""
            SELECT id, name, definition, parent, side, editor_sorting, buildable, model, kind_of, source_file, line
            FROM objects ORDER BY name COLLATE NOCASE
            """) { s in
            result.append(ObjectItem(id: sqlite3_column_int64(s, 0), name: Self.text(s, 1), definition: Self.text(s, 2),
                                     parent: Self.text(s, 3), side: Self.text(s, 4), editorSorting: Self.text(s, 5),
                                     buildable: Self.text(s, 6), model: Self.text(s, 7), kindOf: Self.text(s, 8),
                                     sourceFile: Self.text(s, 9), line: Int(sqlite3_column_int(s, 10))))
        }
        return result
    }

    func warnings() -> [String] {
        var result: [String] = []
        rows("SELECT text FROM warnings ORDER BY id") { s in result.append(Self.text(s, 0)) }
        return result
    }
}

@MainActor
final class EditorModel: ObservableObject {
    @Published var status = CoreStatus()
    @Published var objects: [ObjectItem] = []
    @Published var warnings: [String] = []
    @Published var coreError: String?

    private var reader: StoreReader?
    private var timer: Timer?
    private static let lastGameDirKey = "lastGameDir"

    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("END Editor", isDirectory: true)
        try? FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        let dbPath = support.appendingPathComponent("editor.sqlite").path
        guard end_editor_initialize(dbPath) else {
            coreError = "The editor core could not open \(dbPath)"
            return
        }
        reader = StoreReader(path: String(cString: end_editor_db_path()))
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        if let last = UserDefaults.standard.string(forKey: Self.lastGameDirKey) {
            openGame(at: last)
        } else {
            let guess = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("GeneralsX/GeneralsZH").path
            if FileManager.default.fileExists(atPath: guess) { openGame(at: guess) }
        }
    }

    func openGame(at path: String) {
        let result = String(cString: end_editor_send("game.open\t" + path))
        if result.isEmpty {
            UserDefaults.standard.set(path, forKey: Self.lastGameDirKey)
        } else {
            coreError = "game.open: \(result)"
        }
    }

    func chooseGameFolder() {
        let panel = NSOpenPanel()
        panel.message = "Choose the Zero Hour folder (the one with the .big files)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { openGame(at: url.path) }
    }

    private func tick() {
        end_editor_update()
        guard let reader, let fresh = reader.status(), fresh.revision != status.revision else { return }
        status = fresh
        if fresh.state == "ready" || fresh.state == "error" {
            objects = reader.objects()
            warnings = reader.warnings()
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: EditorModel
    @State private var side: String? = Self.allSides
    @State private var search = ""
    @State private var selection: ObjectItem.ID?
    @State private var sortOrder = [KeyPathComparator(\ObjectItem.name)]

    static let allSides = "\u{1}all"

    private var sides: [(String, Int)] {
        Dictionary(grouping: model.objects, by: \.side).map { ($0.key, $0.value.count) }
            .sorted { $0.1 == $1.1 ? $0.0 < $1.0 : $0.1 > $1.1 }
    }

    private var shown: [ObjectItem] {
        model.objects.filter { item in
            (side == Self.allSides || item.side == side)
                && (search.isEmpty || item.name.localizedCaseInsensitiveContains(search)
                    || item.model.localizedCaseInsensitiveContains(search)
                    || item.kindOf.localizedCaseInsensitiveContains(search))
        }.sorted(using: sortOrder)
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $side) {
                Label("All objects (\(model.objects.count))", systemImage: "square.grid.2x2").tag(Self.allSides)
                Section("Side") {
                    ForEach(sides, id: \.0) { name, count in
                        Text("\(name.isEmpty ? "(no side)" : name) (\(count))").tag(name)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 240)
        } content: {
            Table(shown, selection: $selection, sortOrder: $sortOrder) {
                TableColumn("Name", value: \.name)
                TableColumn("Side", value: \.side)
                TableColumn("Sorting", value: \.editorSorting)
                TableColumn("Buildable", value: \.buildable)
                TableColumn("Model", value: \.model)
            }
            .searchable(text: $search, prompt: "Name, model or KindOf")
            .navigationSplitViewColumnWidth(min: 480, ideal: 640)
        } detail: {
            if let id = selection, let item = model.objects.first(where: { $0.id == id }) {
                ObjectDetail(item: item)
            } else {
                Text("Select an object").foregroundStyle(.secondary)
            }
        }
        .toolbar {
            ToolbarItem(placement: .navigation) {
                Button { model.chooseGameFolder() } label: { Label("Open Game Folder", systemImage: "folder") }
            }
            ToolbarItem(placement: .status) { StatusText() }
        }
        .alert("Editor core", isPresented: .constant(model.coreError != nil)) {
            Button("OK") { model.coreError = nil }
        } message: {
            Text(model.coreError ?? "")
        }
    }
}

struct StatusText: View {
    @EnvironmentObject var model: EditorModel
    var body: some View {
        switch model.status.state {
        case "loading": HStack { ProgressView().controlSize(.small); Text("Reading game data…") }
        case "ready": Text(model.status.message).foregroundStyle(.secondary).help(model.status.gameDir)
        case "error": Text(model.status.message).foregroundStyle(.red)
        default: Text("No game folder open").foregroundStyle(.secondary)
        }
    }
}

struct ObjectDetail: View {
    let item: ObjectItem
    var body: some View {
        Form {
            Section(item.name) {
                LabeledContent("Definition", value: item.definition)
                if !item.parent.isEmpty { LabeledContent("Copies", value: item.parent) }
                LabeledContent("Side", value: item.side.isEmpty ? "—" : item.side)
                LabeledContent("Editor sorting", value: item.editorSorting.isEmpty ? "—" : item.editorSorting)
                LabeledContent("Buildable", value: item.buildable.isEmpty ? "—" : item.buildable)
                LabeledContent("Model", value: item.model.isEmpty ? "—" : item.model)
            }
            Section("KindOf") {
                Text(item.kindOf.isEmpty ? "—" : item.kindOf).textSelection(.enabled)
            }
            Section("Source") {
                Text("\(item.sourceFile):\(item.line)").font(.caption.monospaced()).textSelection(.enabled)
            }
            Section("Preview") {
                Text("Model preview comes in the next step.").foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .navigationSplitViewColumnWidth(min: 260, ideal: 320)
    }
}

@main
struct EndEditorApp: App {
    @StateObject private var model = EditorModel()
    var body: some Scene {
        WindowGroup("END Editor") {
            ContentView().environmentObject(model).frame(minWidth: 1000, minHeight: 600)
        }
    }
}
