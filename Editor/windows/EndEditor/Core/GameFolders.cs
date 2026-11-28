using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace EndEditor.Core;

/// What the client remembers between launches (%APPDATA%\END Editor\settings.json).
public sealed class Settings
{
    public string? LastGameDir { get; set; }
    public string? BaseGameDir { get; set; }
    public string Mode { get; set; } = "Maps";
    public double WindowWidth { get; set; } = 1400;
    public double WindowHeight { get; set; } = 900;

    private static string FilePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "END Editor", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath)) return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch (Exception)
        {
            // A damaged file: start with the defaults.
        }
        return new Settings();
    }

    public void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}

/// Where the game data and the game's user folders are on Windows.
public static class GameFolders
{
    public const string ZeroHourSteamApp = "2732960";
    public const string GeneralsSteamApp = "2229870";

    /// The Steam library folders (libraryfolders.vdf of the Steam install in the registry).
    public static List<string> SteamLibraries()
    {
        var result = new List<string>();
        var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string
                    ?? Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", null) as string;
        if (string.IsNullOrEmpty(steam)) return result;
        steam = steam.Replace('/', '\\');
        result.Add(steam);
        var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(vdf)) return result;
        foreach (Match match in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
        {
            var library = match.Groups[1].Value.Replace(@"\\", @"\");
            if (!result.Contains(library, StringComparer.OrdinalIgnoreCase)) result.Add(library);
        }
        return result;
    }

    /// The install folder of a Steam app (its appmanifest's installdir), or null.
    public static string? SteamApp(string appId)
    {
        foreach (var library in SteamLibraries())
        {
            var manifest = Path.Combine(library, "steamapps", $"appmanifest_{appId}.acf");
            if (!File.Exists(manifest)) continue;
            var match = Regex.Match(File.ReadAllText(manifest), "\"installdir\"\\s+\"([^\"]+)\"");
            if (!match.Success) continue;
            var folder = Path.Combine(library, "steamapps", "common", match.Groups[1].Value);
            if (Directory.Exists(folder)) return folder;
        }
        return null;
    }

    private static bool HasArchives(string folder) =>
        Directory.Exists(folder) && Directory.EnumerateFiles(folder, "*.big").Any();

    /// The base Generals folder: chosen by the user, next to the Zero Hour folder (GeneralsX and
    /// Steam layouts), or Steam's Generals.
    public static string? BaseGameDir(string gameDir, string? chosen)
    {
        if (!string.IsNullOrEmpty(chosen) && Directory.Exists(chosen)) return chosen;
        var parent = Path.GetDirectoryName(gameDir.TrimEnd('\\', '/'));
        if (parent != null)
        {
            foreach (var name in new[] { "Generals", "Command and Conquer Generals" })
            {
                var sibling = Path.Combine(parent, name);
                if (HasArchives(sibling)) return sibling;
            }
        }
        var steam = SteamApp(GeneralsSteamApp);
        return steam != null && HasArchives(steam) ? steam : null;
    }

    /// Whether a game folder is Zero Hour's (it has *ZH.big archives, or none at all).
    public static bool IsZeroHour(string gameDir)
    {
        if (!Directory.Exists(gameDir)) return true;
        var archives = Directory.EnumerateFiles(gameDir, "*.big").Select(Path.GetFileName).ToList();
        return archives.Any(n => n!.EndsWith("zh.big", StringComparison.OrdinalIgnoreCase)) || archives.Count == 0;
    }

    /// The game's user data folder: Documents\<UserDataLeafName> (GlobalData's on Windows; the
    /// registry's leaf name, or the game's default).
    public static string UserDataFolder(bool zeroHour)
    {
        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        var key = zeroHour
            ? @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour"
            : @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Generals";
        var leaf = Registry.GetValue(key, "UserDataLeafName", null) as string;
        if (string.IsNullOrWhiteSpace(leaf))
            leaf = zeroHour ? "Command and Conquer Generals Zero Hour Data" : "Command and Conquer Generals Data";
        return Path.Combine(documents, leaf);
    }

    /// The game's user Maps folder (the user data folder + MapCache's "Maps"): where the game
    /// finds maps saved there; listed with the game's maps.
    public static string UserMapsFolder(string gameDir) => Path.Combine(UserDataFolder(IsZeroHour(gameDir)), "Maps");

    /// The first Zero Hour folder to open: the last one, else Steam's Zero Hour.
    public static string? InitialGameDir(Settings settings)
    {
        if (!string.IsNullOrEmpty(settings.LastGameDir) && Directory.Exists(settings.LastGameDir)) return settings.LastGameDir;
        return SteamApp(ZeroHourSteamApp);
    }
}
