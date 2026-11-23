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
    let muzzleFlashes: String
    let hiddenSubObjects: String
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
            SELECT id, name, definition, parent, side, editor_sorting, buildable, model, kind_of, source_file, line,
                   muzzle_flashes, hidden_sub_objects
            FROM objects ORDER BY name COLLATE NOCASE
            """) { s in
            result.append(ObjectItem(id: sqlite3_column_int64(s, 0), name: Self.text(s, 1), definition: Self.text(s, 2),
                                     parent: Self.text(s, 3), side: Self.text(s, 4), editorSorting: Self.text(s, 5),
                                     buildable: Self.text(s, 6), model: Self.text(s, 7),
                                     muzzleFlashes: Self.text(s, 11), hiddenSubObjects: Self.text(s, 12),
                                     kindOf: Self.text(s, 8),
                                     sourceFile: Self.text(s, 9), line: Int(sqlite3_column_int(s, 10))))
        }
        return result
    }

    private static func blob(_ s: OpaquePointer, _ i: Int32) -> Data {
        let count = Int(sqlite3_column_bytes(s, i))
        guard count > 0, let bytes = sqlite3_column_blob(s, i) else { return Data() }
        return Data(bytes: bytes, count: count)
    }

    /// The model row without the geometry: (name, state, message, revision).
    func modelHeader() -> (name: String, state: String, message: String, revision: Int64)? {
        var result: (String, String, String, Int64)?
        rows("SELECT name, state, message, revision FROM model WHERE id = 1") { s in
            result = (Self.text(s, 0), Self.text(s, 1), Self.text(s, 2), sqlite3_column_int64(s, 3))
        }
        return result
    }

    /// The model with its pose; parts and textures only when `withGeometry`.
    func previewModel(withGeometry: Bool, keepingGeometryOf previous: PreviewModel) -> PreviewModel {
        var model = previous
        rows("""
            SELECT name, revision, bone_count, animation, frame_count, frame_rate, pose, visible,
                   min_x, min_y, min_z, max_x, max_y, max_z FROM model WHERE id = 1
            """) { s in
            model.name = Self.text(s, 0)
            model.revision = sqlite3_column_int64(s, 1)
            model.boneCount = Int(sqlite3_column_int(s, 2))
            model.animation = Self.text(s, 3)
            model.frameCount = max(Int(sqlite3_column_int(s, 4)), 1)
            model.frameRate = Int(sqlite3_column_int(s, 5))
            model.pose = Self.blob(s, 6)
            model.visible = Self.blob(s, 7)
            model.boundsMin = SIMD3(Float(sqlite3_column_double(s, 8)), Float(sqlite3_column_double(s, 9)),
                                    Float(sqlite3_column_double(s, 10)))
            model.boundsMax = SIMD3(Float(sqlite3_column_double(s, 11)), Float(sqlite3_column_double(s, 12)),
                                    Float(sqlite3_column_double(s, 13)))
        }
        guard withGeometry else { return model }
        model.parts = []
        rows("""
            SELECT mesh, texture, bone, skinned, hidden, blend, house_color, vertices, indices
            FROM model_parts ORDER BY id
            """) { s in
            model.parts.append(PreviewPart(mesh: Self.text(s, 0), texture: Self.text(s, 1),
                                           bone: Int(sqlite3_column_int(s, 2)), skinned: sqlite3_column_int(s, 3) != 0,
                                           hidden: sqlite3_column_int(s, 4) != 0, blend: Int(sqlite3_column_int(s, 5)),
                                           houseColor: Int(sqlite3_column_int(s, 6)),
                                           vertices: Self.blob(s, 7), indices: Self.blob(s, 8)))
        }
        model.textures = [:]
        rows("SELECT name, width, height, rgba FROM model_textures") { s in
            model.textures[Self.text(s, 0)] = PreviewTexture(width: Int(sqlite3_column_int(s, 1)),
                                                              height: Int(sqlite3_column_int(s, 2)),
                                                              rgba: Self.blob(s, 3))
        }
        model.geometryKey = "\(model.name)#\(model.revision)"
        return model
    }

    func animationNames() -> [String] {
        var result: [String] = []
        rows("SELECT name FROM model_animations ORDER BY name") { s in result.append(Self.text(s, 0)) }
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
    @Published var preview = PreviewModel()
    @Published var animations: [String] = []
    @Published var modelState = "idle"
    @Published var modelMessage = ""
    private var modelRevision: Int64 = -1
    private var modelName = ""
    private var geometryRequested = false

    private var reader: StoreReader?
    private var timer: Timer?
    private static let lastGameDirKey = "lastGameDir"
    private static let baseGameDirKey = "baseGameDir"

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

    /// The base Generals folder: most models and textures live in its archives. Chosen by the
    /// user, or found next to the Zero Hour folder (GeneralsX and Steam layouts).
    private func baseGameDir(for gameDir: String) -> String? {
        if let chosen = UserDefaults.standard.string(forKey: Self.baseGameDirKey),
           FileManager.default.fileExists(atPath: chosen) { return chosen }
        let parent = URL(fileURLWithPath: gameDir).deletingLastPathComponent()
        for name in ["Generals", "Command and Conquer Generals"] {
            let sibling = parent.appendingPathComponent(name)
            let hasArchives = (try? FileManager.default.contentsOfDirectory(atPath: sibling.path))?
                .contains { $0.lowercased().hasSuffix(".big") } ?? false
            if hasArchives { return sibling.path }
        }
        return nil
    }

    func openGame(at path: String) {
        var command = "game.open\t" + path
        if let base = baseGameDir(for: path), base != path { command += "\t" + base }
        let result = String(cString: end_editor_send(command))
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

    func chooseBaseGameFolder() {
        let panel = NSOpenPanel()
        panel.message = "Choose the base Generals folder (most models are in its .big files)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            UserDefaults.standard.set(url.path, forKey: Self.baseGameDirKey)
            if !status.gameDir.isEmpty { openGame(at: status.gameDir) }
        }
    }

    func showModel(of item: ObjectItem?) {
        let name = item?.model ?? ""
        // The model plus what the object's Draw module hides when idle (muzzle flashes...).
        let request = [name, item?.muzzleFlashes ?? "", item?.hiddenSubObjects ?? ""].joined(separator: "\t")
        guard request != modelName else { return }
        modelName = request
        guard !name.isEmpty else {
            modelState = "error"
            modelMessage = "This object has no model"
            preview = PreviewModel()
            animations = []
            return
        }
        geometryRequested = true
        let result = String(cString: end_editor_send("model.open\t" + request))
        if !result.isEmpty {
            modelState = "error"
            modelMessage = result
        }
    }

    func playAnimation(_ name: String) {
        _ = end_editor_send("model.animation\t" + name)
    }

    private func tick() {
        end_editor_update()
        guard let reader else { return }
        if let fresh = reader.status(), fresh.revision != status.revision {
            status = fresh
            if fresh.state == "ready" || fresh.state == "error" {
                objects = reader.objects()
                warnings = reader.warnings()
                modelName = ""
                preview = PreviewModel()
                animations = []
            }
        }
        if let header = reader.modelHeader(), header.revision != modelRevision {
            modelRevision = header.revision
            modelState = header.state
            modelMessage = header.message
            if header.state == "ready" {
                // Same model, new animation: keep the uploaded geometry.
                // model.open brings new parts; model.animation only a new pose.
                let geometryChanged = geometryRequested || preview.parts.isEmpty
                geometryRequested = false
                preview = reader.previewModel(withGeometry: geometryChanged, keepingGeometryOf: preview)
                if geometryChanged { animations = reader.animationNames() }
            } else if header.state == "error" || header.state == "idle" {
                preview = PreviewModel()
                animations = []
            }
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
                    .onAppear { model.showModel(of: item) }
                    .onChange(of: item) { _, newItem in model.showModel(of: newItem) }
            } else {
                Text("Select an object").foregroundStyle(.secondary)
            }
        }
        .toolbar {
            ToolbarItem(placement: .navigation) {
                Menu {
                    Button("Zero Hour Folder…") { model.chooseGameFolder() }
                    Button("Base Generals Folder…") { model.chooseBaseGameFolder() }
                } label: { Label("Game Folders", systemImage: "folder") }
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
            Section("Model") {
                ModelPreviewPanel()
            }
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
        }
        .formStyle(.grouped)
        .navigationSplitViewColumnWidth(min: 320, ideal: 420)
    }
}

/// `END Editor --snapshot <Zero Hour folder> <base folder> <model> <animation or -> <frame> <out.png>`
/// renders one frame of a model to a PNG without opening a window, then exits. Used to check the
/// preview without driving a screen.
enum Snapshot {
    static func runIfRequested() {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--snapshot") else { return }
        let values = Array(arguments[(at + 1)...])
        guard values.count == 6, let frame = Int(values[4]) else {
            FileHandle.standardError.write("usage: --snapshot <zh> <base> <model> <anim|-> <frame> <out.png>\n".data(using: .utf8)!)
            exit(2)
        }
        let db = NSTemporaryDirectory() + "end-editor-snapshot.sqlite"
        guard end_editor_initialize(db), let reader = StoreReader(path: String(cString: end_editor_db_path())) else { exit(1) }
        var commands: [String] = []
        commands.append("game.open\t" + values[0] + "\t" + values[1])
        commands.append("model.open\t" + values[2])
        if values[3] != "-" { commands.append("model.animation\t" + values[3]) }
        for command in commands {
            let result = String(cString: end_editor_send(command))
            guard result.isEmpty else { print("\(command): \(result)"); exit(1) }
            end_editor_update()
        }
        let header = reader.modelHeader()
        print("model: \(header?.state ?? "?") \(header?.message ?? "")")
        let preview = reader.previewModel(withGeometry: true, keepingGeometryOf: PreviewModel())
        guard let renderer = PreviewRenderer(playback: PreviewPlayback()) else { print("no Metal"); exit(1) }
        renderer.show(preview)
        guard let image = renderer.renderImage(width: 640, height: 480, frame: frame),
              let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: values[5]) as CFURL,
                                                                "public.png" as CFString, 1, nil) else { exit(1) }
        CGImageDestinationAddImage(destination, image, nil)
        exit(CGImageDestinationFinalize(destination) ? 0 : 1)
    }
}

@main
struct EndEditorApp: App {
    @StateObject private var model = EditorModel()

    init() {
        Snapshot.runIfRequested()
    }
    var body: some Scene {
        WindowGroup("END Editor") {
            ContentView().environmentObject(model).frame(minWidth: 1000, minHeight: 600)
        }
    }
}
