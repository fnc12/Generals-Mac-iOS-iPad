using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Numerics;
using System.Windows.Threading;
using EndEditor.Core;

namespace EndEditor;

/// What changed in the last store read; views refresh only what they show.
[Flags]
public enum Changes
{
    None = 0,
    Status = 1 << 0,
    Objects = 1 << 1,
    Maps = 1 << 2,
    Map = 1 << 3,  // another map (or none): scene, players, warnings
    MapObjects = 1 << 4,  // edits: objects, links
    MapModels = 1 << 5,
    MapEdit = 1 << 6,  // undo/redo counts, edited, saved path, map state
    Editor = 1 << 7,  // map_editor: tool, note, picker...
    Selection = 1 << 8,
    Gesture = 1 << 9,
    Play = 1 << 10,
    Preview = 1 << 11,
    SaveConflict = 1 << 12,
}

/// The client's view of the store, read each tick (100 ms) and after each command. The core
/// decides everything; this only sends commands and keeps what the store says.
public sealed class EditorModel
{
    public CoreStatus Status = CoreStatus.Initial;
    public List<ObjectItem> Objects = [];
    public List<string> Warnings = [];
    public string? CoreError;

    public PreviewModel Preview = new();
    public List<string> Animations = [];
    public string ModelState = "idle";
    public string ModelMessage = "";
    private long modelRevision = -1;
    private string modelRequest = "";
    private bool geometryRequested;

    public List<MapListItem> Maps = [];
    public MapScene MapScene = new();
    public List<MapModelData> MapModels = [];
    public Dictionary<string, TextureData> MapModelTextures = [];
    public string MapState = "idle";
    public string MapMessage = "";
    public List<string> MapWarnings = [];
    public int MapModelsLoaded, MapModelCount;
    public MapEditState MapEdit = MapEditState.Initial;
    public List<MapPlayer> MapPlayers = [];
    public List<RoadTypeItem> RoadTypes = [];
    public MapEditorState MapEditor = MapEditorState.Initial;
    public List<long> MapSelection = [];
    public MapGesture MapGesture = MapGesture.None;
    /// The ground point under the cursor while it is over the map: where Ctrl+V pastes.
    public Vector2? CursorGround;
    public PlayState Play = PlayState.Initial;
    public List<FactionItem> Factions = [];
    public List<PlayerColorItem> PlayerColors = [];
    public string SaveConflict = "";

    private long mapRevision = -1, mapModelRevision = -1, lastMapTexture;
    private readonly StoreReader? reader;
    private readonly DispatcherTimer timer;
    public readonly Settings Settings = Settings.Load();

    /// Raised on the UI thread after a read that changed something.
    public event Action<Changes>? Changed;

    public EditorModel(string? dbPath = null)
    {
        dbPath ??= DefaultDbPath();
        Directory.CreateDirectory(Path.GetDirectoryName(dbPath)!);
        if (!Native.Initialize(dbPath))
        {
            CoreError = $"The editor core could not open {dbPath}";
        }
        else
        {
            reader = new StoreReader(Native.DbPath);
        }
        timer = new DispatcherTimer(TimeSpan.FromMilliseconds(100), DispatcherPriority.Background, (_, _) => Tick(),
                                    Dispatcher.CurrentDispatcher);
    }

    public static string DefaultDbPath() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "END Editor", "editor.sqlite");

    public void Start(string? gameDir = null, string? baseDir = null)
    {
        timer.Start();
        gameDir ??= GameFolders.InitialGameDir(Settings);
        if (gameDir != null) OpenGame(gameDir, baseDir);
    }

    public void Shutdown()
    {
        timer.Stop();
        reader?.Dispose();
        Native.Shutdown();
    }

    // MARK: Commands

    /// Sends a command and shows its result at once (the core runs it in the next update). A
    /// refused command leaves its note in the store too.
    public string Send(string command)
    {
        var result = Native.Send(command);
        if (result.Length > 0) Debug.WriteLine($"END Editor: {command.Split('\t')[0]}: {result}");
        Tick();
        return result;
    }

    public static string Number(float value) => value.ToString("0.0000", CultureInfo.InvariantCulture);
    public static string Number(double value) => value.ToString("0.####", CultureInfo.InvariantCulture);

    public void OpenGame(string path, string? chosenBase = null)
    {
        if (chosenBase != null) Settings.BaseGameDir = chosenBase;
        Native.Send("maps.user\t" + GameFolders.UserMapsFolder(path));
        var command = "game.open\t" + path;
        var baseDir = GameFolders.BaseGameDir(path, Settings.BaseGameDir);
        if (baseDir != null && !string.Equals(baseDir, path, StringComparison.OrdinalIgnoreCase)) command += "\t" + baseDir;
        var result = Native.Send(command);
        if (result.Length == 0)
        {
            Settings.LastGameDir = path;
            Settings.Save();
        }
        else
        {
            CoreError = $"game.open: {result}";
            Changed?.Invoke(Changes.Status);
        }
    }

    public void ShowModel(ObjectItem? item)
    {
        var name = item?.Model ?? "";
        // The model plus what the object's Draw module hides when idle (muzzle flashes...).
        var request = string.Join('\t', name, item?.MuzzleFlashes ?? "", item?.HiddenSubObjects ?? "");
        if (request == modelRequest) return;
        modelRequest = request;
        if (name.Length == 0)
        {
            ModelState = "error";
            ModelMessage = "This object has no model";
            Preview = new PreviewModel();
            Animations = [];
            Changed?.Invoke(Changes.Preview);
            return;
        }
        geometryRequested = true;
        var result = Native.Send("model.open\t" + request);
        if (result.Length > 0)
        {
            ModelState = "error";
            ModelMessage = result;
            Changed?.Invoke(Changes.Preview);
        }
    }

    public void PlayAnimation(string name) => Send("model.animation\t" + name);

    public void OpenMap(string path)
    {
        var result = Native.Send("map.open\t" + path);
        if (result.Length > 0)
        {
            MapState = "error";
            MapMessage = $"map.open: {result}";
            Changed?.Invoke(Changes.Map);
        }
        Tick();
    }

    public List<MapObjectProperty> MapObjectProperties(long id) => reader?.MapObjectProperties(id) ?? [];

    public bool CanEditMap => MapState == "ready";

    public string UserMapsFolder => GameFolders.UserMapsFolder(Status.GameDir);

    public void Copy() => Send("edit.copy");
    public void Paste() =>
        Send("edit.paste\t" + (CursorGround is { } g ? Number(g.X) + "\t" + Number(g.Y) : "\t"));
    public void Duplicate() => Send("edit.duplicate");
    public void SelectAll() => Send("edit.select_all");
    public void Undo() => Send("map.undo");
    public void Redo() => Send("map.redo");
    public void PlayMap() => Send("map.play");
    public void StopGame() => Send("play.stop");
    public void SaveMap(string file) => Send("map.save\t" + file);

    // MARK: Store reads

    private void ReadMapEdits(StoreReader r, MapHeader header, ref Changes changes)
    {
        if (header.Edit == MapEdit) return;
        var objectsChanged = header.Edit.ObjectRevision != MapEdit.ObjectRevision;
        MapEdit = header.Edit;
        changes |= Changes.MapEdit;
        if (!objectsChanged || header.State != "ready") return;
        MapScene.Objects = r.MapObjects();
        MapScene.Links = r.MapLinks();
        MapScene.ObjectsRevision = header.Edit.ObjectRevision;
        changes |= Changes.MapObjects;
    }

    private void ReadEditor(StoreReader r, ref Changes changes)
    {
        if (r.MapEditor() is not var (state, gesture) || state.Revision == MapEditor.Revision) return;
        var conflict = r.SaveConflict();
        if (conflict != SaveConflict)
        {
            SaveConflict = conflict;
            changes |= Changes.SaveConflict;
        }
        if (gesture != MapGesture)
        {
            MapGesture = gesture;
            changes |= Changes.Gesture;
        }
        if (state.SelectionRevision != MapEditor.SelectionRevision)
        {
            var selection = r.MapSelection();
            if (!selection.SequenceEqual(MapSelection))
            {
                MapSelection = selection;
                changes |= Changes.Selection;
            }
        }
        var compared = state with { Revision = MapEditor.Revision };
        if (compared != MapEditor) changes |= Changes.Editor;
        MapEditor = state;
    }

    private void ReadMap(StoreReader r, ref Changes changes)
    {
        if (r.MapHeader() is not { } header) return;
        if (header.Revision != mapRevision)
        {
            mapRevision = header.Revision;
            MapState = header.State;
            MapMessage = header.Message;
            MapModels = [];
            MapModelTextures = [];
            lastMapTexture = 0;
            mapModelRevision = -1;
            MapModelCount = header.ModelCount;
            MapModelsLoaded = 0;
            MapScene = header.State == "ready" ? r.MapScene() : new MapScene();
            MapScene.ObjectsRevision = header.Edit.ObjectRevision;
            MapWarnings = header.State == "ready" ? r.MapWarnings() : [];
            MapEdit = header.Edit;
            MapPlayers = header.State == "ready" ? r.MapPlayers() : [];
            changes |= Changes.Map | Changes.MapObjects | Changes.MapEdit | Changes.MapModels;
        }
        ReadMapEdits(r, header, ref changes);
        if (header.State == "ready" && header.ModelRevision != mapModelRevision)
        {
            mapModelRevision = header.ModelRevision;
            MapModelCount = header.ModelCount;
            MapModelsLoaded = header.ModelsLoaded;
            var (last, textures) = r.MapModelTextures(lastMapTexture);
            lastMapTexture = last;
            foreach (var (name, texture) in textures) MapModelTextures.TryAdd(name, texture);
            var fresh = r.MapModels(MapModels.Count > 0 ? MapModels[^1].Id : 0);
            MapModels.AddRange(fresh);
            changes |= Changes.MapModels;
        }
    }

    public void Tick()
    {
        Native.Update();
        if (reader == null) return;
        var changes = Changes.None;
        var fresh = reader.Status();
        if (fresh is { } s && s.Revision == Status.Revision && s.MapListRevision != Status.MapListRevision)
        {
            Status = s;
            Maps = reader.Maps();  // only the list changed (a map saved into the user Maps folder)
            changes |= Changes.Maps | Changes.Status;
        }
        else if (fresh is { } status && status.Revision != Status.Revision)
        {
            Status = status;
            changes |= Changes.Status;
            if (status.State == "ready" || status.State == "error")
            {
                Objects = reader.Objects();
                Warnings = reader.Warnings();
                Maps = reader.Maps();
                RoadTypes = reader.RoadTypes();
                Factions = reader.Factions();
                PlayerColors = reader.PlayerColors();
                modelRequest = "";
                Preview = new PreviewModel();
                Animations = [];
                changes |= Changes.Objects | Changes.Maps | Changes.Preview | Changes.Play;
            }
        }
        ReadMap(reader, ref changes);
        ReadEditor(reader, ref changes);
        if (reader.Play() is { } play && play.Revision != Play.Revision)
        {
            Play = play;
            changes |= Changes.Play;
        }
        if (reader.ModelHeader() is { } header && header.Revision != modelRevision)
        {
            modelRevision = header.Revision;
            ModelState = header.State;
            ModelMessage = header.Message;
            if (header.State == "ready")
            {
                // Same model, new animation: keep the uploaded geometry.
                var geometryChanged = geometryRequested || Preview.Parts.Count == 0;
                geometryRequested = false;
                Preview = reader.PreviewModel(geometryChanged, Preview);
                if (geometryChanged) Animations = reader.AnimationNames();
            }
            else if (header.State is "error" or "idle")
            {
                Preview = new PreviewModel();
                Animations = [];
            }
            changes |= Changes.Preview;
        }
        if (changes != Changes.None) Changed?.Invoke(changes);
    }
}
