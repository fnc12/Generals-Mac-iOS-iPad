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

    // MARK: Map

    func maps() -> [MapListItem] {
        var result: [MapListItem] = []
        rows("SELECT id, path, name, source FROM maps ORDER BY name COLLATE NOCASE") { s in
            result.append(MapListItem(id: sqlite3_column_int64(s, 0), path: Self.text(s, 1), name: Self.text(s, 2),
                                      source: Self.text(s, 3)))
        }
        return result
    }

    struct MapHeader {
        var path = "", name = "", state = "", message = ""
        var revision: Int64 = -1, modelRevision: Int64 = -1
        var modelsLoaded = 0, modelCount = 0
    }

    func mapHeader() -> MapHeader? {
        var result: MapHeader?
        rows("SELECT path, name, state, message, revision, model_revision, models_loaded, model_count FROM map WHERE id = 1") { s in
            result = MapHeader(path: Self.text(s, 0), name: Self.text(s, 1), state: Self.text(s, 2), message: Self.text(s, 3),
                               revision: sqlite3_column_int64(s, 4), modelRevision: sqlite3_column_int64(s, 5),
                               modelsLoaded: Int(sqlite3_column_int(s, 6)), modelCount: Int(sqlite3_column_int(s, 7)))
        }
        return result
    }

    private func floats(_ data: Data) -> [Float] {
        data.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
    }

    /// The open map without its models.
    func mapScene() -> MapScene {
        var scene = MapScene()
        rows("""
            SELECT path, revision, name, width, height, border, playable_x0, playable_y0, playable_x1, playable_y1,
                   lighting, heights, water_r, water_g, water_b, water_a, water_repeat FROM map WHERE id = 1
            """) { s in
            scene.key = "\(Self.text(s, 0))#\(sqlite3_column_int64(s, 1))"
            scene.name = Self.text(s, 2)
            scene.width = Int(sqlite3_column_int(s, 3))
            scene.height = Int(sqlite3_column_int(s, 4))
            scene.border = Int(sqlite3_column_int(s, 5))
            scene.playable = SIMD4(Float(sqlite3_column_double(s, 6)), Float(sqlite3_column_double(s, 7)),
                                   Float(sqlite3_column_double(s, 8)), Float(sqlite3_column_double(s, 9)))
            scene.lighting = floats(Self.blob(s, 10))
            scene.heights = Self.blob(s, 11)
            scene.waterColor = SIMD4(Float(sqlite3_column_double(s, 12)), Float(sqlite3_column_double(s, 13)),
                                     Float(sqlite3_column_double(s, 14)), Float(sqlite3_column_double(s, 15)))
            scene.waterRepeat = Float(sqlite3_column_double(s, 16))
        }
        rows("SELECT name, width, height, rgba FROM map_textures") { s in
            let texture = PreviewTexture(width: Int(sqlite3_column_int(s, 1)), height: Int(sqlite3_column_int(s, 2)),
                                         rgba: Self.blob(s, 3))
            switch Self.text(s, 0) {
            case "terrain": scene.atlas = texture
            case "water": scene.waterTexture = texture
            default: break
            }
        }
        rows("SELECT vertices, indices FROM map_terrain ORDER BY id") { s in
            scene.terrain.append((Self.blob(s, 0), Self.blob(s, 1)))
        }
        rows("SELECT vertices, indices FROM map_water ORDER BY id") { s in
            scene.water.append((Self.blob(s, 0), Self.blob(s, 1)))
        }
        rows("SELECT id, template_name, kind, x, y, z, angle, flags, model_id, owner, color FROM map_objects ORDER BY id") { s in
            scene.objects.append(MapObjectItem(
                id: sqlite3_column_int64(s, 0), templateName: Self.text(s, 1), kind: Self.text(s, 2),
                position: SIMD3(Float(sqlite3_column_double(s, 3)), Float(sqlite3_column_double(s, 4)),
                                Float(sqlite3_column_double(s, 5))),
                angle: Float(sqlite3_column_double(s, 6)), flags: Int(sqlite3_column_int(s, 7)),
                modelId: sqlite3_column_int64(s, 8), owner: Self.text(s, 9), color: UInt32(truncatingIfNeeded: sqlite3_column_int(s, 10))))
        }
        return scene
    }

    /// Models loaded since `afterId` (ready or failed, not pending), with their parts.
    func mapModels(after afterId: Int64) -> [MapModelData] {
        var result: [MapModelData] = []
        rows("""
            SELECT id, name, state, message, min_x, min_y, min_z, max_x, max_y, max_z FROM map_models
            WHERE id > \(afterId) AND state <> 'pending' ORDER BY id
            """) { s in
            result.append(MapModelData(
                id: sqlite3_column_int64(s, 0), name: Self.text(s, 1), ready: Self.text(s, 2) == "ready",
                message: Self.text(s, 3),
                boundsMin: SIMD3(Float(sqlite3_column_double(s, 4)), Float(sqlite3_column_double(s, 5)),
                                 Float(sqlite3_column_double(s, 6))),
                boundsMax: SIMD3(Float(sqlite3_column_double(s, 7)), Float(sqlite3_column_double(s, 8)),
                                 Float(sqlite3_column_double(s, 9))),
                parts: []))
        }
        guard let first = result.first?.id, let last = result.last?.id else { return [] }
        var parts: [Int64: [MapModelPart]] = [:]
        rows("""
            SELECT model_id, texture, blend, house_color, vertices, indices FROM map_model_parts
            WHERE model_id BETWEEN \(first) AND \(last) ORDER BY id
            """) { s in
            parts[sqlite3_column_int64(s, 0), default: []].append(MapModelPart(
                texture: Self.text(s, 1), blend: Int(sqlite3_column_int(s, 2)), houseColor: Int(sqlite3_column_int(s, 3)),
                vertices: Self.blob(s, 4), indices: Self.blob(s, 5)))
        }
        return result.map { model in
            MapModelData(id: model.id, name: model.name, ready: model.ready, message: model.message,
                         boundsMin: model.boundsMin, boundsMax: model.boundsMax, parts: parts[model.id] ?? [])
        }
    }

    /// Model textures with ids above `afterId`: (last id, textures by name).
    func mapModelTextures(after afterId: Int64) -> (Int64, [String: PreviewTexture]) {
        var last = afterId
        var result: [String: PreviewTexture] = [:]
        rows("SELECT id, name, width, height, rgba FROM map_model_textures WHERE id > \(afterId) ORDER BY id") { s in
            last = sqlite3_column_int64(s, 0)
            result[Self.text(s, 1)] = PreviewTexture(width: Int(sqlite3_column_int(s, 2)),
                                                     height: Int(sqlite3_column_int(s, 3)), rgba: Self.blob(s, 4))
        }
        return (last, result)
    }

    func mapObjectProperties(of objectId: Int64) -> [MapObjectProperty] {
        var result: [MapObjectProperty] = []
        rows("SELECT key, type, value FROM map_object_properties WHERE object_id = \(objectId) ORDER BY id") { s in
            result.append(MapObjectProperty(key: Self.text(s, 0), type: Self.text(s, 1), value: Self.text(s, 2)))
        }
        return result
    }

    func mapWarnings() -> [String] {
        var result: [String] = []
        rows("SELECT text FROM map_warnings ORDER BY id") { s in result.append(Self.text(s, 0)) }
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

    @Published var maps: [MapListItem] = []
    @Published var mapScene = MapScene()
    @Published var mapModels: [MapModelData] = []
    @Published var mapModelTextures: [String: PreviewTexture] = [:]
    @Published var mapState = "idle"
    @Published var mapMessage = ""
    @Published var mapWarnings: [String] = []
    @Published var mapModelsLoaded = 0
    @Published var mapModelCount = 0
    @Published var selectedMapObject: Int64?
    private var mapRevision: Int64 = -1
    private var mapModelRevision: Int64 = -1
    private var lastMapTexture: Int64 = 0

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
        PerfHarness.run(self)
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

    func openMap(_ path: String) {
        let result = String(cString: end_editor_send("map.open\t" + path))
        if !result.isEmpty {
            mapState = "error"
            mapMessage = "map.open: \(result)"
        }
    }

    func chooseMapFile() {
        let panel = NSOpenPanel()
        panel.message = "Choose a .map file"
        panel.allowedContentTypes = [.init(filenameExtension: "map") ?? .data]
        if panel.runModal() == .OK, let url = panel.url { openMap(url.path) }
    }

    func mapObjectProperties(of id: Int64) -> [MapObjectProperty] {
        reader?.mapObjectProperties(of: id) ?? []
    }

    /// Picks up map changes from the store: a new map, or more of its models.
    private func readMap(_ reader: StoreReader) {
        guard let header = reader.mapHeader() else { return }
        if header.revision != mapRevision {
            mapRevision = header.revision
            mapState = header.state
            mapMessage = header.message
            mapModels = []
            mapModelTextures = [:]
            lastMapTexture = 0
            mapModelRevision = -1
            mapModelCount = header.modelCount
            mapModelsLoaded = 0
            selectedMapObject = nil
            mapScene = header.state == "ready" ? reader.mapScene() : MapScene()
            mapWarnings = header.state == "ready" ? reader.mapWarnings() : []
        }
        if header.state == "ready" && header.modelRevision != mapModelRevision {
            mapModelRevision = header.modelRevision
            mapModelCount = header.modelCount
            mapModelsLoaded = header.modelsLoaded
            let (last, textures) = reader.mapModelTextures(after: lastMapTexture)
            lastMapTexture = last
            if !textures.isEmpty { mapModelTextures.merge(textures) { old, _ in old } }
            let fresh = reader.mapModels(after: mapModels.last?.id ?? 0)
            if !fresh.isEmpty { mapModels.append(contentsOf: fresh) }
        }
    }

    private func tick() {
        end_editor_update()
        guard let reader else { return }
        if let fresh = reader.status(), fresh.revision != status.revision {
            status = fresh
            if fresh.state == "ready" || fresh.state == "error" {
                objects = reader.objects()
                warnings = reader.warnings()
                maps = reader.maps()
                modelName = ""
                preview = PreviewModel()
                animations = []
            }
        }
        readMap(reader)
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

/// Objects (step 1-2) or maps (step 3).
enum EditorMode: String, CaseIterable, Identifiable {
    case objects = "Objects", maps = "Maps"
    var id: String { rawValue }
}

struct ContentView: View {
    @EnvironmentObject var model: EditorModel
    @AppStorage("editorMode") private var mode = EditorMode.objects

    var body: some View {
        Group {
            switch mode {
            case .objects: ObjectBrowser()
            case .maps: MapBrowser()
            }
        }
        .toolbar {
            ToolbarItem(placement: .navigation) {
                Menu {
                    Button("Zero Hour Folder…") { model.chooseGameFolder() }
                    Button("Base Generals Folder…") { model.chooseBaseGameFolder() }
                    Divider()
                    Button("Open Map File…") { mode = .maps; model.chooseMapFile() }
                        .disabled(model.status.state != "ready")
                } label: { Label("Game Folders", systemImage: "folder") }
            }
            ToolbarItem(placement: .principal) {
                Picker("Mode", selection: $mode) {
                    ForEach(EditorMode.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
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

struct ObjectBrowser: View {
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
/// renders one frame of a model to a PNG without opening a window, then exits.
/// `END Editor --gif <Zero Hour folder> <base folder> <model> <animation> <square|wide> <out.gif>`
/// writes the same GIF as the preview's "Export GIF" (default camera). Both check the preview
/// without driving a screen; `--map-snapshot` does the same for the map view.
enum Snapshot {
    static func runIfRequested() {
        let arguments = CommandLine.arguments
        if let at = arguments.firstIndex(of: "--snapshot") {
            let values = Array(arguments[(at + 1)...])
            guard values.count == 6, let frame = Int(values[4]) else {
                usage("--snapshot <zh> <base> <model> <anim|-> <frame> <out.png>")
            }
            let preview = load(zh: values[0], base: values[1], model: values[2], animation: values[3])
            guard let renderer = PreviewRenderer(playback: PreviewPlayback()) else { print("no Metal"); exit(1) }
            renderer.show(preview)
            guard let image = renderer.renderImage(width: 640, height: 480, frame: frame),
                  let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: values[5]) as CFURL,
                                                                    "public.png" as CFString, 1, nil) else { exit(1) }
            CGImageDestinationAddImage(destination, image, nil)
            exit(CGImageDestinationFinalize(destination) ? 0 : 1)
        }
        if let at = arguments.firstIndex(of: "--map-snapshot") {
            mapSnapshot(Array(arguments[(at + 1)...]))
        }
        if let at = arguments.firstIndex(of: "--gif") {
            let values = Array(arguments[(at + 1)...])
            guard values.count == 6, let format = GifFormat(rawValue: values[4]) else {
                usage("--gif <zh> <base> <model> <anim> <square|wide> <out.gif>")
            }
            let preview = load(zh: values[0], base: values[1], model: values[2], animation: values[3])
            do {
                try GifExport.write(preview, camera: PreviewPlayback().exportCamera(), format: format,
                                    to: URL(fileURLWithPath: values[5]))
            } catch {
                print("gif: \(error)")
                exit(1)
            }
            print("gif: \(preview.frameCount) frames at \(preview.frameRate) fps")
            exit(0)
        }
    }

    /// `--map-snapshot <zh> <base> <map path> <out.png> [--top] [--look <x> <y> <distance> <yaw°> <pitch°>]
    /// [--size <w>x<h>] [--pick <x> <y>]`: opens the map, waits for all its models, renders the
    /// default view (or top-down, or looking at a world point). --pick clicks at a point of the
    /// image (origin top left), prints what it selects and draws the selection box.
    private static func mapSnapshot(_ values: [String]) -> Never {
        let usageText = "--map-snapshot <zh> <base> <map path> <out.png> [--top] "
            + "[--look <x> <y> <distance> <yaw°> <pitch°>] [--size <w>x<h>] [--pick <x> <y>]"
        guard values.count >= 4 else { usage(usageText) }
        var width = 1280, height = 800
        var top = false
        var pick: CGPoint?
        var look: [Float]?
        var i = 4
        while i < values.count {
            switch values[i] {
            case "--top": top = true
            case "--size":
                let parts = (i + 1 < values.count ? values[i + 1] : "").split(separator: "x").compactMap { Int($0) }
                guard parts.count == 2 else { usage(usageText) }
                (width, height) = (parts[0], parts[1])
                i += 1
            case "--look":
                let numbers = values.dropFirst(i + 1).prefix(5).compactMap { Float($0) }
                guard numbers.count == 5 else { usage(usageText) }
                look = numbers
                i += 5
            case "--pick":
                guard i + 2 < values.count, let x = Double(values[i + 1]), let y = Double(values[i + 2]) else { usage(usageText) }
                pick = CGPoint(x: x, y: y)
                i += 2
            default: usage(usageText)
            }
            i += 1
        }
        let db = NSTemporaryDirectory() + "end-editor-map-snapshot.sqlite"
        guard end_editor_initialize(db), let reader = StoreReader(path: String(cString: end_editor_db_path())) else { exit(1) }
        let started = Date()
        for command in ["game.open\t" + values[0] + "\t" + values[1], "map.open\t" + values[2]] {
            let result = String(cString: end_editor_send(command))
            guard result.isEmpty else { print("\(command): \(result)"); exit(1) }
            end_editor_update()
        }
        let opened = Date()
        while end_editor_pending_work() > 0 { end_editor_update() }
        guard let header = reader.mapHeader(), header.state == "ready" else {
            print("map: \(reader.mapHeader()?.state ?? "?") \(reader.mapHeader()?.message ?? "")")
            exit(1)
        }
        let scene = reader.mapScene()
        let models = reader.mapModels(after: 0)
        let textures = reader.mapModelTextures(after: 0).1
        print("map: \(header.name): \(header.message)")
        print(String(format: "timing: game + map %.2f s, models %.2f s", opened.timeIntervalSince(started),
                     Date().timeIntervalSince(opened)))
        print("models: \(models.filter(\.ready).count) of \(models.count) ready, \(textures.count) textures")
        for warning in reader.mapWarnings() { print("warning: \(warning)") }

        let state = MapViewState()
        guard let renderer = MapRenderer(state: state) else { print("no Metal"); exit(1) }
        renderer.show(scene)
        renderer.add(models: models, textures: textures)
        if top { state.frame(scene, top: true) }
        if let look {
            state.target = SIMD3(look[0], look[1], scene.groundHeight(x: look[0], y: look[1]))
            state.distance = look[2]
            state.yaw = look[3] * .pi / 180
            state.pitch = look[4] * .pi / 180
        }
        if let pick {
            let size = CGSize(width: width, height: height)
            if let id = renderer.pick(at: pick, size: size), let object = scene.objects.first(where: { $0.id == id }) {
                renderer.selected = id
                print("pick: #\(id) \(object.templateName) (\(object.kind)) at \(object.position)")
                for property in reader.mapObjectProperties(of: id) {
                    print("   \(property.key) (\(property.type)) = \(property.value)")
                }
            } else {
                print("pick: nothing")
            }
        }
        guard let image = renderer.renderImage(width: width, height: height),
              let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: values[3]) as CFURL,
                                                                "public.png" as CFString, 1, nil) else { exit(1) }
        CGImageDestinationAddImage(destination, image, nil)
        exit(CGImageDestinationFinalize(destination) ? 0 : 1)
    }

    private static func usage(_ text: String) -> Never {
        FileHandle.standardError.write("usage: \(text)\n".data(using: .utf8)!)
        exit(2)
    }

    private static func load(zh: String, base: String, model: String, animation: String) -> PreviewModel {
        let db = NSTemporaryDirectory() + "end-editor-snapshot.sqlite"
        guard end_editor_initialize(db), let reader = StoreReader(path: String(cString: end_editor_db_path())) else { exit(1) }
        var commands: [String] = []
        commands.append("game.open\t" + zh + "\t" + base)
        commands.append("model.open\t" + model)
        if animation != "-" { commands.append("model.animation\t" + animation) }
        for command in commands {
            let result = String(cString: end_editor_send(command))
            guard result.isEmpty else { print("\(command): \(result)"); exit(1) }
            end_editor_update()
        }
        let header = reader.modelHeader()
        print("model: \(header?.state ?? "?") \(header?.message ?? "")")
        return reader.previewModel(withGeometry: true, keepingGeometryOf: PreviewModel())
    }
}

@main
struct EndEditorApp: App {
    @StateObject private var model = EditorModel()

    init() {
        Snapshot.runIfRequested()
        PerfHarness.parseArguments()
    }
    var body: some Scene {
        WindowGroup("END Editor") {
            ContentView().environmentObject(model).frame(minWidth: 1000, minHeight: 600)
        }
    }
}
