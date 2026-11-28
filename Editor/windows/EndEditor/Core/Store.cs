using System.IO;
using System.Numerics;
using Microsoft.Data.Sqlite;

namespace EndEditor.Core;

// What the client reads from the core's store (Editor/core/src/storage.h has every column).

public sealed record ObjectItem(long Id, string Name, string Definition, string Parent, string Side, string EditorSorting,
                                string Buildable, string Model, string MuzzleFlashes, string HiddenSubObjects,
                                string KindOf, string SourceFile, int Line);

public record struct CoreStatus(string State, string Message, string GameDir, long ObjectCount, long Revision,
                                long MapListRevision)
{
    public static CoreStatus Initial => new("idle", "", "", 0, -1, 0);
}

public sealed record MapListItem(long Id, string Path, string Name, string Source);

/// A placed object (map_objects). Kind: "model", "marker", "waypoint", "road", "bridge".
public sealed record MapObjectItem(long Id, string TemplateName, string Kind, Vector3 Position, float Height, float Angle,
                                   int Flags, long ModelId, string Owner, uint Color, int Start)
{
    public MapObjectItem At(Vector3 position) => this with { Position = position };
}

public sealed record MapLink(string Kind, long From, long To);

public sealed record MapObjectProperty(string Key, string Type, string Value);

/// A player of the map's SidesList, or a start position (Start > 0) of a multiplayer map.
public sealed record MapPlayer(string Name, string DisplayName, string Team, uint Color, bool Neutral, int Start)
{
    public string Label => Neutral ? "Neutral"
        : Start > 0 ? $"Start {Start}"
        : string.IsNullOrEmpty(DisplayName) || DisplayName == Name ? Name : $"{DisplayName} ({Name})";
}

public sealed record RoadTypeItem(string Name, bool Bridge);

public sealed record FactionItem(string Name, string Side);

/// A Multiplayer.ini color; Index is the game's color index.
public sealed record PlayerColorItem(int Index, string Name, uint Rgb)
{
    public string Label => Name.StartsWith("Color") && Name.Length > 5 ? Name[5..] : Name;
}

public sealed record TextureData(int Width, int Height, byte[] Rgba);

public sealed record MapModelPart(string Texture, int Blend, int HouseColor, bool TwoSided, byte[] Vertices, byte[] Indices);

public sealed record MapModelData(long Id, string Name, bool Ready, string Message, Vector3 BoundsMin, Vector3 BoundsMax,
                                  List<MapModelPart> Parts);

/// The map row's editing columns.
public record struct MapEditState(long ObjectRevision, int UndoCount, int RedoCount, bool Edited, long LastObject,
                                  string Message, string SavedPath)
{
    public static MapEditState Initial => new(-1, 0, 0, false, 0, "", "");
}

/// The map_editor row: tool, template, owner, angle, picker, waiting points, note (map_editor.h).
public record struct MapEditorState(string Tool, string TemplateName, string RoadType, string Team, double Angle,
                                    bool Picker, Vector2? Pending, Vector2? RoadStart, string Note, string NoteKind,
                                    long NoteSerial, long SelectionRevision, int SelectionCount, int ClipboardCount,
                                    long Revision)
{
    public static MapEditorState Initial => new("object", "", "", "", 0, false, null, null, "", "", 0, -1, 0, 0, -1);
}

/// The mouse gesture in progress (map_editor's gesture, drag_dx, drag_dy).
public record struct MapGesture(string Kind, Vector2 Drag)
{
    public static MapGesture None => new("", Vector2.Zero);
}

/// The play row: the Play button's settings and its last run.
public record struct PlayState(string App, string AppResolved, string Faction, int AiCount, string AiDifficulty,
                               int StartPosition, string Color, string State, string Message, string MapPath,
                               string Command, string LogPath, long Pid, int ExitCode, long Runs, long Revision)
{
    public static PlayState Initial =>
        new("", "", "FactionAmerica", 1, "medium", 1, "Random", "idle", "", "", "", "", 0, 0, 0, -1);
}

public sealed record MapHeader(string Path, string Name, string State, string Message, long Revision, long ModelRevision,
                               int ModelsLoaded, int ModelCount, MapEditState Edit);

/// Everything of the open map but its models.
public sealed class MapScene
{
    public string Key = "";
    public string Name = "";
    public int Width, Height, Border;
    public Vector4 Playable;
    public float[] Lighting = new float[18];
    public byte[] Heights = [];
    public TextureData? Atlas, WaterTexture;
    public Vector4 WaterColor = new(0.5f, 0.6f, 1, 0.5f);
    public float WaterRepeat = 32;
    public List<(byte[] Vertices, byte[] Indices)> Terrain = [];
    public List<(byte[] Vertices, byte[] Indices)> Water = [];
    public List<MapObjectItem> Objects = [];
    public List<MapLink> Links = [];
    public long ObjectsRevision = -1;

    public bool IsEmpty => Heights.Length == 0;

    /// World x/y range of the whole height map (border included): x0 y0 x1 y1.
    public Vector4 Extent => new(-Border * 10f, -Border * 10f, (Width - 1 - Border) * 10f, (Height - 1 - Border) * 10f);

    /// Ground height at a world position (bilinear, like the core's groundHeight).
    public float GroundHeight(float x, float y)
    {
        if (Width <= 1 || Height <= 1 || Heights.Length < Width * Height) return 0;
        float fx = Math.Clamp(x / 10 + Border, 0, Width - 1);
        float fy = Math.Clamp(y / 10 + Border, 0, Height - 1);
        int x0 = Math.Min((int)fx, Width - 2), y0 = Math.Min((int)fy, Height - 2);
        float tx = fx - x0, ty = fy - y0;
        float At(int i, int j) => Heights[j * Width + i] * 0.625f;
        float south = At(x0, y0) * (1 - tx) + At(x0 + 1, y0) * tx;
        float north = At(x0, y0 + 1) * (1 - tx) + At(x0 + 1, y0 + 1) * tx;
        return south * (1 - ty) + north * ty;
    }
}

/// The preview's model with its pose (model_* tables).
public sealed class PreviewModel
{
    public string Name = "";
    public long Revision = -1;
    public int BoneCount;
    public string Animation = "";
    public int FrameCount = 1;
    public int FrameRate = 30;
    public byte[] Pose = [];
    public byte[] Visible = [];
    public Vector3 BoundsMin = new(-1), BoundsMax = new(1);
    public List<PreviewPart> Parts = [];
    public Dictionary<string, TextureData> Textures = [];
    public string GeometryKey = "";

    public PreviewModel WithPoseOf(PreviewModel geometry)
    {
        Parts = geometry.Parts;
        Textures = geometry.Textures;
        GeometryKey = geometry.GeometryKey;
        return this;
    }
}

public sealed record PreviewPart(string Mesh, string Texture, int Bone, bool Skinned, bool Hidden, int Blend,
                                 int HouseColor, byte[] Vertices, byte[] Indices);

/// The client's read-only connection to the core's store.
public sealed class StoreReader : IDisposable
{
    private readonly SqliteConnection connection;

    public StoreReader(string path)
    {
        connection = new SqliteConnection(new SqliteConnectionStringBuilder
        {
            DataSource = path, Mode = SqliteOpenMode.ReadOnly, Pooling = false
        }.ToString());
        connection.Open();
    }

    public void Dispose() => connection.Dispose();

    private void Rows(string sql, Action<SqliteDataReader> read)
    {
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        try
        {
            using var reader = command.ExecuteReader();
            while (reader.Read()) read(reader);
        }
        catch (SqliteException)
        {
            // The core is between transactions or has not made the table yet: next tick.
        }
    }

    private static string Text(SqliteDataReader r, int i) => r.IsDBNull(i) ? "" : r.GetString(i);
    private static byte[] Blob(SqliteDataReader r, int i) => r.IsDBNull(i) ? [] : (byte[])r.GetValue(i);
    private static float F(SqliteDataReader r, int i) => (float)r.GetDouble(i);
    private static uint Rgb(SqliteDataReader r, int i) => unchecked((uint)r.GetInt32(i));

    public CoreStatus? Status()
    {
        CoreStatus? result = null;
        Rows("SELECT state, message, game_dir, object_count, revision, map_list_revision FROM status WHERE id = 1",
             r => result = new CoreStatus(Text(r, 0), Text(r, 1), Text(r, 2), r.GetInt64(3), r.GetInt64(4), r.GetInt64(5)));
        return result;
    }

    public List<ObjectItem> Objects()
    {
        var result = new List<ObjectItem>();
        Rows("""
             SELECT id, name, definition, parent, side, editor_sorting, buildable, model, muzzle_flashes,
                    hidden_sub_objects, kind_of, source_file, line FROM objects ORDER BY name COLLATE NOCASE
             """,
             r => result.Add(new ObjectItem(r.GetInt64(0), Text(r, 1), Text(r, 2), Text(r, 3), Text(r, 4), Text(r, 5),
                                            Text(r, 6), Text(r, 7), Text(r, 8), Text(r, 9), Text(r, 10), Text(r, 11),
                                            r.GetInt32(12))));
        return result;
    }

    public List<string> Warnings()
    {
        var result = new List<string>();
        Rows("SELECT text FROM warnings ORDER BY id", r => result.Add(Text(r, 0)));
        return result;
    }

    public (string Name, string State, string Message, long Revision)? ModelHeader()
    {
        (string, string, string, long)? result = null;
        Rows("SELECT name, state, message, revision FROM model WHERE id = 1",
             r => result = (Text(r, 0), Text(r, 1), Text(r, 2), r.GetInt64(3)));
        return result;
    }

    /// The model with its pose; parts and textures only when `withGeometry`.
    public PreviewModel PreviewModel(bool withGeometry, PreviewModel previous)
    {
        var model = new PreviewModel().WithPoseOf(previous);
        Rows("""
             SELECT name, revision, bone_count, animation, frame_count, frame_rate, pose, visible,
                    min_x, min_y, min_z, max_x, max_y, max_z FROM model WHERE id = 1
             """, r =>
        {
            model.Name = Text(r, 0);
            model.Revision = r.GetInt64(1);
            model.BoneCount = r.GetInt32(2);
            model.Animation = Text(r, 3);
            model.FrameCount = Math.Max(r.GetInt32(4), 1);
            model.FrameRate = r.GetInt32(5);
            model.Pose = Blob(r, 6);
            model.Visible = Blob(r, 7);
            model.BoundsMin = new Vector3(F(r, 8), F(r, 9), F(r, 10));
            model.BoundsMax = new Vector3(F(r, 11), F(r, 12), F(r, 13));
        });
        if (!withGeometry) return model;
        model.Parts = [];
        Rows("SELECT mesh, texture, bone, skinned, hidden, blend, house_color, vertices, indices FROM model_parts ORDER BY id",
             r => model.Parts.Add(new PreviewPart(Text(r, 0), Text(r, 1), r.GetInt32(2), r.GetInt32(3) != 0,
                                                  r.GetInt32(4) != 0, r.GetInt32(5), r.GetInt32(6), Blob(r, 7), Blob(r, 8))));
        model.Textures = [];
        Rows("SELECT name, width, height, rgba FROM model_textures",
             r => model.Textures[Text(r, 0)] = new TextureData(r.GetInt32(1), r.GetInt32(2), Blob(r, 3)));
        model.GeometryKey = $"{model.Name}#{model.Revision}";
        return model;
    }

    public List<string> AnimationNames()
    {
        var result = new List<string>();
        Rows("SELECT name FROM model_animations ORDER BY name", r => result.Add(Text(r, 0)));
        return result;
    }

    public List<MapListItem> Maps()
    {
        var result = new List<MapListItem>();
        Rows("SELECT id, path, name, source FROM maps ORDER BY name COLLATE NOCASE",
             r => result.Add(new MapListItem(r.GetInt64(0), Text(r, 1), Text(r, 2), Text(r, 3))));
        return result;
    }

    public MapHeader? MapHeader()
    {
        MapHeader? result = null;
        Rows("""
             SELECT path, name, state, message, revision, model_revision, models_loaded, model_count,
                    object_revision, undo_count, redo_count, edited, last_object, edit_message, saved_path
             FROM map WHERE id = 1
             """,
             r => result = new MapHeader(Text(r, 0), Text(r, 1), Text(r, 2), Text(r, 3), r.GetInt64(4), r.GetInt64(5),
                                         r.GetInt32(6), r.GetInt32(7),
                                         new MapEditState(r.GetInt64(8), r.GetInt32(9), r.GetInt32(10), r.GetInt32(11) != 0,
                                                          r.GetInt64(12), Text(r, 13), Text(r, 14))));
        return result;
    }

    public string SaveConflict()
    {
        var result = "";
        Rows("SELECT save_conflict FROM map WHERE id = 1", r => result = Text(r, 0));
        return result;
    }

    public List<MapPlayer> MapPlayers()
    {
        var result = new List<MapPlayer>();
        Rows("SELECT name, display_name, team, color, neutral, start FROM map_players ORDER BY neutral DESC, id",
             r => result.Add(new MapPlayer(Text(r, 0), Text(r, 1), Text(r, 2), Rgb(r, 3), r.GetInt32(4) != 0, r.GetInt32(5))));
        return result;
    }

    /// The open map without its models.
    public MapScene MapScene()
    {
        var scene = new MapScene();
        Rows("""
             SELECT path, revision, name, width, height, border, playable_x0, playable_y0, playable_x1, playable_y1,
                    lighting, heights, water_r, water_g, water_b, water_a, water_repeat FROM map WHERE id = 1
             """, r =>
        {
            scene.Key = $"{Text(r, 0)}#{r.GetInt64(1)}";
            scene.Name = Text(r, 2);
            scene.Width = r.GetInt32(3);
            scene.Height = r.GetInt32(4);
            scene.Border = r.GetInt32(5);
            scene.Playable = new Vector4(F(r, 6), F(r, 7), F(r, 8), F(r, 9));
            var lighting = Blob(r, 10);
            scene.Lighting = new float[lighting.Length / 4];
            Buffer.BlockCopy(lighting, 0, scene.Lighting, 0, scene.Lighting.Length * 4);
            scene.Heights = Blob(r, 11);
            scene.WaterColor = new Vector4(F(r, 12), F(r, 13), F(r, 14), F(r, 15));
            scene.WaterRepeat = F(r, 16);
        });
        Rows("SELECT name, width, height, rgba FROM map_textures", r =>
        {
            var texture = new TextureData(r.GetInt32(1), r.GetInt32(2), Blob(r, 3));
            switch (Text(r, 0))
            {
                case "terrain": scene.Atlas = texture; break;
                case "water": scene.WaterTexture = texture; break;
            }
        });
        Rows("SELECT vertices, indices FROM map_terrain ORDER BY id", r => scene.Terrain.Add((Blob(r, 0), Blob(r, 1))));
        Rows("SELECT vertices, indices FROM map_water ORDER BY id", r => scene.Water.Add((Blob(r, 0), Blob(r, 1))));
        scene.Objects = MapObjects();
        scene.Links = MapLinks();
        return scene;
    }

    public List<MapObjectItem> MapObjects()
    {
        var result = new List<MapObjectItem>();
        Rows("SELECT id, template_name, kind, x, y, z, angle, flags, model_id, owner, color, height, start FROM map_objects ORDER BY id",
             r => result.Add(new MapObjectItem(r.GetInt64(0), Text(r, 1), Text(r, 2), new Vector3(F(r, 3), F(r, 4), F(r, 5)),
                                               F(r, 11), F(r, 6), r.GetInt32(7), r.GetInt64(8), Text(r, 9), Rgb(r, 10),
                                               r.GetInt32(12))));
        return result;
    }

    public List<MapLink> MapLinks()
    {
        var result = new List<MapLink>();
        Rows("SELECT kind, from_id, to_id FROM map_links ORDER BY id",
             r => result.Add(new MapLink(Text(r, 0), r.GetInt64(1), r.GetInt64(2))));
        return result;
    }

    public (MapEditorState State, MapGesture Gesture)? MapEditor()
    {
        (MapEditorState, MapGesture)? result = null;
        Rows("""
             SELECT tool, template_name, road_type, team, angle, picker, pending, pending_x, pending_y, road_start, road_x,
                    road_y, gesture, drag_dx, drag_dy, note, note_kind, note_serial, selection_revision, selection_count,
                    clipboard_count, revision FROM map_editor WHERE id = 1
             """, r =>
        {
            Vector2? Point(int flag, int x) => r.GetInt32(flag) != 0 ? new Vector2(F(r, x), F(r, x + 1)) : null;
            var state = new MapEditorState(Text(r, 0), Text(r, 1), Text(r, 2), Text(r, 3), r.GetDouble(4), r.GetInt32(5) != 0,
                                           Point(6, 7), Point(9, 10), Text(r, 15), Text(r, 16), r.GetInt64(17),
                                           r.GetInt64(18), r.GetInt32(19), r.GetInt32(20), r.GetInt64(21));
            result = (state, new MapGesture(Text(r, 12), new Vector2(F(r, 13), F(r, 14))));
        });
        return result;
    }

    public List<long> MapSelection()
    {
        var result = new List<long>();
        Rows("SELECT object_id FROM map_selection ORDER BY id", r => result.Add(r.GetInt64(0)));
        return result;
    }

    public List<RoadTypeItem> RoadTypes()
    {
        var result = new List<RoadTypeItem>();
        Rows("SELECT name, bridge FROM road_types ORDER BY id", r => result.Add(new RoadTypeItem(Text(r, 0), r.GetInt32(1) != 0)));
        return result;
    }

    /// Models loaded since `afterId` (ready or failed, not pending), with their parts.
    public List<MapModelData> MapModels(long afterId)
    {
        var result = new List<MapModelData>();
        Rows($"""
              SELECT id, name, state, message, min_x, min_y, min_z, max_x, max_y, max_z FROM map_models
              WHERE id > {afterId} AND state <> 'pending' ORDER BY id
              """,
             r => result.Add(new MapModelData(r.GetInt64(0), Text(r, 1), Text(r, 2) == "ready", Text(r, 3),
                                              new Vector3(F(r, 4), F(r, 5), F(r, 6)), new Vector3(F(r, 7), F(r, 8), F(r, 9)), [])));
        if (result.Count == 0) return result;
        var byId = result.ToDictionary(m => m.Id);
        Rows($"""
              SELECT model_id, texture, blend, house_color, two_sided, vertices, indices FROM map_model_parts
              WHERE model_id BETWEEN {result[0].Id} AND {result[^1].Id} ORDER BY id
              """, r =>
        {
            if (byId.TryGetValue(r.GetInt64(0), out var model))
                model.Parts.Add(new MapModelPart(Text(r, 1), r.GetInt32(2), r.GetInt32(3), r.GetInt32(4) != 0, Blob(r, 5), Blob(r, 6)));
        });
        return result;
    }

    /// Model textures with ids above `afterId`: the last id and the textures by name.
    public (long Last, Dictionary<string, TextureData> Textures) MapModelTextures(long afterId)
    {
        long last = afterId;
        var result = new Dictionary<string, TextureData>();
        Rows($"SELECT id, name, width, height, rgba FROM map_model_textures WHERE id > {afterId} ORDER BY id", r =>
        {
            last = r.GetInt64(0);
            result[Text(r, 1)] = new TextureData(r.GetInt32(2), r.GetInt32(3), Blob(r, 4));
        });
        return (last, result);
    }

    public List<MapObjectProperty> MapObjectProperties(long objectId)
    {
        var result = new List<MapObjectProperty>();
        Rows($"SELECT key, type, value FROM map_object_properties WHERE object_id = {objectId} ORDER BY id",
             r => result.Add(new MapObjectProperty(Text(r, 0), Text(r, 1), Text(r, 2))));
        return result;
    }

    public List<string> MapWarnings()
    {
        var result = new List<string>();
        Rows("SELECT text FROM map_warnings ORDER BY id", r => result.Add(Text(r, 0)));
        return result;
    }

    public PlayState? Play()
    {
        PlayState? result = null;
        Rows("""
             SELECT app, app_resolved, faction, ai_count, ai_difficulty, start_position, color, state, message, map_path,
                    command, log_path, pid, exit_code, runs, revision FROM play WHERE id = 1
             """,
             r => result = new PlayState(Text(r, 0), Text(r, 1), Text(r, 2), r.GetInt32(3), Text(r, 4), r.GetInt32(5),
                                         Text(r, 6), Text(r, 7), Text(r, 8), Text(r, 9), Text(r, 10), Text(r, 11),
                                         r.GetInt64(12), r.GetInt32(13), r.GetInt64(14), r.GetInt64(15)));
        return result;
    }

    public List<FactionItem> Factions()
    {
        var result = new List<FactionItem>();
        Rows("SELECT name, side FROM factions ORDER BY id", r => result.Add(new FactionItem(Text(r, 0), Text(r, 1))));
        return result;
    }

    public List<PlayerColorItem> PlayerColors()
    {
        var result = new List<PlayerColorItem>();
        Rows("SELECT idx, name, rgb FROM player_colors ORDER BY idx, id",
             r => result.Add(new PlayerColorItem(r.GetInt32(0), Text(r, 1), Rgb(r, 2))));
        return result;
    }
}
