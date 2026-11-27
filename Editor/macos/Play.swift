// The Play button (like the World Editor's "Test Map") and the Save Map As sheet. Both only send
// commands: map.play saves the map with its edits to the core's test copy and starts the game on it
// (the play row says what was started and whether it still runs), play.* change its settings, and
// map.save_as saves under a name into the game's user Maps folder (the map row says when another
// map is in the way).
import AppKit
import SQLite3
import SwiftUI

/// The store's play row.
struct PlayState: Equatable {
    var app = ""
    var appResolved = ""
    var faction = "FactionAmerica"
    var aiCount = 1
    var aiDifficulty = "medium"
    var startPosition = 1
    var color = "Random"
    var state = "idle"
    var message = ""
    var mapPath = ""
    var command = ""
    var logPath = ""
    var pid: Int64 = 0
    var exitCode = 0
    var runs: Int64 = 0
    var revision: Int64 = -1
}

struct FactionItem: Hashable, Identifiable {
    let name: String
    let side: String
    var id: String { name }
}

/// A player color of Multiplayer.ini (the player_colors table); `index` is the game's color index.
struct PlayerColorItem: Hashable, Identifiable {
    let index: Int
    let name: String
    let rgb: UInt32
    var id: Int { index }
    /// "ColorBlue" -> "Blue"
    var label: String { name.hasPrefix("Color") && name.count > 5 ? String(name.dropFirst(5)) : name }
}

extension StoreReader {
    func play() -> PlayState? {
        var result: PlayState?
        rows("""
            SELECT app, app_resolved, faction, ai_count, ai_difficulty, start_position, state, message, map_path, command,
                   log_path, pid, exit_code, runs, revision, color FROM play WHERE id = 1
            """) { s in
            result = PlayState(app: Self.text(s, 0), appResolved: Self.text(s, 1), faction: Self.text(s, 2),
                               aiCount: Int(sqlite3_column_int(s, 3)), aiDifficulty: Self.text(s, 4),
                               startPosition: Int(sqlite3_column_int(s, 5)), state: Self.text(s, 6),
                               message: Self.text(s, 7), mapPath: Self.text(s, 8), command: Self.text(s, 9),
                               logPath: Self.text(s, 10), pid: sqlite3_column_int64(s, 11),
                               exitCode: Int(sqlite3_column_int(s, 12)), runs: sqlite3_column_int64(s, 13),
                               revision: sqlite3_column_int64(s, 14))
            result?.color = Self.text(s, 15)
        }
        return result
    }

    func factions() -> [FactionItem] {
        var result: [FactionItem] = []
        rows("SELECT name, side FROM factions ORDER BY id") { s in
            result.append(FactionItem(name: Self.text(s, 0), side: Self.text(s, 1)))
        }
        return result
    }

    func playerColors() -> [PlayerColorItem] {
        var result: [PlayerColorItem] = []
        rows("SELECT idx, name, rgb FROM player_colors ORDER BY idx, id") { s in
            result.append(PlayerColorItem(index: Int(sqlite3_column_int(s, 0)), name: Self.text(s, 1),
                                          rgb: UInt32(truncatingIfNeeded: sqlite3_column_int(s, 2))))
        }
        return result
    }

    func saveConflict() -> String {
        var result = ""
        rows("SELECT save_conflict FROM map WHERE id = 1") { s in result = Self.text(s, 0) }
        return result
    }
}

/// The Play settings: the local player's faction and start position, the AI opponents and the game
/// app. Shown in a popover of the toolbar's settings button.
struct PlaySettingsView: View {
    @Environment(EditorModel.self) private var editor

    var body: some View {
        let play = editor.play
        Form {
            Picker("Faction", selection: Binding(get: { play.faction }, set: { editor.send("play.faction\t" + $0) })) {
                Text("Random").tag("Random")
                ForEach(editor.factions) { faction in Text(faction.name).tag(faction.name) }
            }
            .accessibilityIdentifier("play.faction")
            Picker("Color", selection: Binding(get: { play.color }, set: { editor.send("play.color\t" + $0) })) {
                Text("Random").tag("Random")
                // The first entry of a name is the one play.color means.
                ForEach(editor.playerColors.filter { color in
                    editor.playerColors.first { $0.name == color.name }?.index == color.index
                }) { color in
                    Text(color.label).tag(color.name)
                }
            }
            .accessibilityIdentifier("play.color")
            Picker("Start position", selection: Binding(get: { play.startPosition },
                                                        set: { editor.send("play.start\t\($0)") })) {
                Text("Random").tag(0)
                ForEach(1...8, id: \.self) { Text("\($0)").tag($0) }
            }
            .accessibilityIdentifier("play.start")
            LabeledContent("AI opponents") {
                Stepper(value: Binding(get: { play.aiCount }, set: { editor.send("play.ai\t\($0)") }), in: 0...7) {
                    Text("\(play.aiCount)").monospacedDigit()
                }
                .accessibilityIdentifier("play.ai")
            }
            Picker("AI difficulty", selection: Binding(get: { play.aiDifficulty },
                                                       set: { editor.send("play.difficulty\t" + $0) })) {
                Text("Easy").tag("easy")
                Text("Medium").tag("medium")
                Text("Hard").tag("hard")
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("play.difficulty")
            LabeledContent("Game app") {
                CommitField(title: "Game app", value: play.app, identifier: "play.app") { editor.send("play.app\t" + $0) }
                    .frame(width: 260)
            }
            HStack {
                Button("Choose…") { chooseApp() }.accessibilityIdentifier("play.choose")
                Button("Default") { editor.send("play.app\t") }.disabled(play.app.isEmpty)
                    .accessibilityIdentifier("play.default")
            }
            Text("Starts: " + play.appResolved).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            if !play.message.isEmpty {
                Text(play.message).font(.caption)
                    .foregroundStyle(play.state == "error" ? .red : .secondary)
                    .accessibilityIdentifier("play.message")
            }
            if !play.logPath.isEmpty {
                Text("Game output: " + play.logPath).font(.caption2).foregroundStyle(.tertiary).textSelection(.enabled)
            }
        }
        .formStyle(.grouped)
        .frame(width: 440)
        .padding(4)
    }

    private func chooseApp() {
        let panel = NSOpenPanel()
        panel.message = "Choose the game app (GeneralsXZH.app)"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.treatsFilePackagesAsDirectories = false
        panel.allowedContentTypes = [.application, .unixExecutable]
        if panel.runModal() == .OK, let url = panel.url { editor.send("play.app\t" + url.path) }
    }
}

/// Save Map As: a name for the map in the game's user Maps folder (<Maps>/<name>/<name>.map, the
/// layout the game looks for; its preview <name>.tga next to it), or any file through the system
/// panel.
struct SaveMapSheet: View {
    @Environment(EditorModel.self) private var editor
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var sent = false

    private var target: String {
        editor.userMapsFolder.appendingPathComponent(name, isDirectory: true).appendingPathComponent(name + ".map").path
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Save Map As").font(.headline)
            TextField("Name of the map", text: $name)
                .textFieldStyle(.roundedBorder)
                .frame(width: 360)
                .accessibilityIdentifier("save.name")
                .onSubmit(save)
            Text("Saved in the game's Maps folder, so it shows in the game's map lists:\n" + target)
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            let conflict = editor.saveConflict == target && !name.isEmpty
            if conflict {
                Text("\(name) already exists in the Maps folder. Replace it?")
                    .font(.caption).foregroundStyle(.orange)
                    .accessibilityIdentifier("save.conflict")
            }
            HStack {
                Button("Other Location…") {
                    dismiss()
                    // Once this sheet is gone: the system panel is a sheet on the same window.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { editor.saveMapToOtherLocation() }
                }
                .accessibilityIdentifier("save.other")
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .accessibilityIdentifier("save.cancel")
                Button(conflict ? "Replace" : "Save") { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    .accessibilityIdentifier("save.save")
            }
        }
        .padding(16)
        .onAppear {
            name = editor.mapScene.name.isEmpty ? "New Map" : editor.mapScene.name
        }
        // Saved (a new note, the map's saved path is the target, nothing in the way): done.
        .onChange(of: editor.mapEditor.noteSerial) {
            if sent && editor.mapEdit.savedPath == target && editor.saveConflict.isEmpty && !editor.mapEdit.edited {
                dismiss()
            }
        }
    }

    private func save() {
        guard !name.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        sent = true
        let replace = editor.saveConflict == target
        editor.send("map.save_as\t" + name + (replace ? "\treplace" : ""))
    }
}

/// The toolbar's Play button (⌘R) and the Play settings next to it.
struct PlayToolbar: View {
    @Environment(EditorModel.self) private var editor
    @State private var settingsShown = false

    var body: some View {
        Button { editor.playMap() } label: { Label("Play", systemImage: "play.fill") }
            .help("Play the map in the game, with its unsaved edits (⌘R)")
            .disabled(!editor.canEditMap)
            .accessibilityIdentifier("toolbar.play")
        Button { settingsShown.toggle() } label: { Label("Play Settings", systemImage: "gearshape") }
            .help("Faction, color, AI opponents and the game app for Play")
            .accessibilityIdentifier("toolbar.playSettings")
            .popover(isPresented: $settingsShown, arrowEdge: .bottom) { PlaySettingsView() }
    }
}
