using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using EndEditor.Core;
using Microsoft.Win32;

namespace EndEditor.Views;

/// The Play settings: the local player's faction, color and start position, the AI opponents and
/// the game app. Shown in a pop-up of the toolbar's settings button.
public sealed class PlaySettingsView : StackPanel
{
    private readonly EditorModel editor;

    public PlaySettingsView(EditorModel editor)
    {
        this.editor = editor;
        Width = 460;
        Margin = new Thickness(12);
        this.Id("play.settings");
        Refresh();
    }

    public void Refresh()
    {
        Children.Clear();
        var play = editor.Play;
        Children.Add(Ui.Header("Play Settings"));
        var factions = new List<(string, string)> { ("Random", "Random") };
        factions.AddRange(editor.Factions.Select(f => (f.Name, f.Name)));
        Children.Add(Ui.Field("Faction", Ui.Choice(factions, play.Faction, f => editor.Send("play.faction\t" + f), "play.faction")));
        var colors = new List<(string, string)> { ("Random", "Random") };
        // The first entry of a name is the one play.color means.
        colors.AddRange(editor.PlayerColors.Where(c => editor.PlayerColors.First(o => o.Name == c.Name).Index == c.Index)
                            .Select(c => (c.Label, c.Name)));
        Children.Add(Ui.Field("Color", Ui.Choice(colors, play.Color, c => editor.Send("play.color\t" + c), "play.color")));
        Children.Add(Ui.Field("Start position", Ui.Choice(new[] { ("Random", 0) }.Concat(Enumerable.Range(1, 8).Select(i => (i.ToString(), i))),
                                                          play.StartPosition, s => editor.Send($"play.start\t{s}"), "play.start", 100)));
        Children.Add(Ui.Field("AI opponents", Ui.Choice(Enumerable.Range(0, 8).Select(i => (i.ToString(), i)), play.AiCount,
                                                        n => editor.Send($"play.ai\t{n}"), "play.ai", 100)));
        Children.Add(Ui.Field("AI difficulty", Ui.Segments([("Easy", "easy"), ("Medium", "medium"), ("Hard", "hard")], play.AiDifficulty,
                                                           d => editor.Send("play.difficulty\t" + d), "play.difficulty")));
        Children.Add(Ui.Field("Game app", Ui.CommitField(play.App, app => editor.Send("play.app\t" + app.Trim()), "play.app")));
        var defaultButton = Ui.Button("Default", () => editor.Send("play.app\t"), "play.default");
        defaultButton.IsEnabled = play.App.Length > 0;
        Children.Add(Ui.Row(Ui.Button("Choose…", ChooseApp, "play.choose"), defaultButton));
        Children.Add(new TextBox { Text = "Starts: " + play.AppResolved, IsReadOnly = true, BorderThickness = new Thickness(0), FontSize = 11,
                                   Foreground = Ui.Secondary, TextWrapping = TextWrapping.Wrap }.Id("play.resolved"));
        if (play.Message.Length > 0)
        {
            var message = Ui.Text(play.Message, 11, play.State == "error" ? Ui.ErrorBrush : Ui.Secondary).Id("play.message");
            message.TextWrapping = TextWrapping.Wrap;
            Children.Add(message);
        }
        if (play.LogPath.Length > 0)
            Children.Add(new TextBox { Text = "Game output: " + play.LogPath, IsReadOnly = true, BorderThickness = new Thickness(0), FontSize = 10,
                                       Foreground = Ui.Tertiary, TextWrapping = TextWrapping.Wrap });
    }

    private void ChooseApp()
    {
        var dialog = new OpenFileDialog
        {
            Title = "Choose the game (GeneralsXZH.exe)", Filter = "Programs (*.exe)|*.exe",
            FileName = "GeneralsXZH.exe",
        };
        if (editor.Play.AppResolved is { Length: > 0 } current && Path.GetDirectoryName(current) is { } folder && Directory.Exists(folder))
            dialog.InitialDirectory = folder;
        if (dialog.ShowDialog() == true) editor.Send("play.app\t" + dialog.FileName);
    }
}

/// Save Map As: a name for the map in the game's user Maps folder (<Maps>\<name>\<name>.map, the
/// layout the game looks for; its preview <name>.tga next to it), or any file through the system
/// dialog.
public sealed class SaveMapDialog : Window
{
    private readonly EditorModel editor;
    public readonly TextBox NameBox;
    private readonly TextBlock targetText, conflictText;
    private readonly Button saveButton;
    private bool sent;
    public bool OtherLocationChosen { get; private set; }

    public SaveMapDialog(EditorModel editor, Window owner)
    {
        this.editor = editor;
        Owner = owner;
        Title = "Save Map As";
        SizeToContent = SizeToContent.WidthAndHeight;
        ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        ShowInTaskbar = false;
        this.Id("save.sheet");
        var panel = new StackPanel { Margin = new Thickness(16), Width = 440 };
        panel.Children.Add(Ui.Header("Save Map As"));
        NameBox = new TextBox { Text = editor.MapScene.Name.Length == 0 ? "New Map" : editor.MapScene.Name, Margin = new Thickness(0, 4, 0, 6) }.Id("save.name");
        System.Windows.Automation.AutomationProperties.SetName(NameBox, "Name of the map");
        NameBox.TextChanged += (_, _) => Update();
        NameBox.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                Save();
                e.Handled = true;
            }
        };
        panel.Children.Add(NameBox);
        targetText = Ui.Text("", 11, Ui.Secondary);
        targetText.TextWrapping = TextWrapping.Wrap;
        panel.Children.Add(targetText);
        conflictText = Ui.Text("", 11, Ui.Warning).Id("save.conflict");
        panel.Children.Add(conflictText);
        var buttons = new DockPanel { Margin = new Thickness(0, 12, 0, 0) };
        var other = Ui.Button("Other Location…", () =>
        {
            OtherLocationChosen = true;
            Close();
        }, "save.other");
        DockPanel.SetDock(other, Dock.Left);
        buttons.Children.Add(other);
        saveButton = Ui.Button("Save", Save, "save.save");
        saveButton.IsDefault = true;
        var cancel = Ui.Button("Cancel", Close, "save.cancel");
        cancel.IsCancel = true;
        var right = Ui.Row(cancel, saveButton);
        right.HorizontalAlignment = HorizontalAlignment.Right;
        buttons.Children.Add(right);
        panel.Children.Add(buttons);
        Content = panel;
        editor.Changed += OnChanged;
        Closed += (_, _) => editor.Changed -= OnChanged;
        Loaded += (_, _) =>
        {
            NameBox.Focus();
            NameBox.SelectAll();
        };
        Update();
    }

    private string MapName => NameBox.Text.Trim();
    public string Target => Path.Combine(editor.UserMapsFolder, MapName, MapName + ".map");

    private void Update()
    {
        targetText.Text = "Saved in the game's Maps folder, so it shows in the game's map lists:\n" + Target;
        var conflict = MapName.Length > 0 && string.Equals(editor.SaveConflict, Target, StringComparison.OrdinalIgnoreCase);
        conflictText.Text = conflict ? $"{MapName} already exists in the Maps folder. Replace it?" : "";
        conflictText.Visibility = conflict ? Visibility.Visible : Visibility.Collapsed;
        saveButton.Content = conflict ? "Replace" : "Save";
        saveButton.IsEnabled = MapName.Length > 0;
    }

    private void OnChanged(Changes changes)
    {
        if ((changes & (Changes.SaveConflict | Changes.Editor | Changes.MapEdit)) == 0) return;
        Update();
        // Saved (the map's saved path is the target, nothing in the way, no unsaved edits): done.
        if (sent && string.Equals(editor.MapEdit.SavedPath, Target, StringComparison.OrdinalIgnoreCase) &&
            editor.SaveConflict.Length == 0 && !editor.MapEdit.Edited)
        {
            Close();
        }
    }

    private void Save()
    {
        if (MapName.Length == 0) return;
        sent = true;
        var replace = string.Equals(editor.SaveConflict, Target, StringComparison.OrdinalIgnoreCase);
        editor.Send("map.save_as\t" + MapName + (replace ? "\treplace" : ""));
    }

    /// The sheet's "Other Location…": any .map file, through the system dialog; into the user Maps
    /// folder as Maps\<name>\<name>.map (the layout the game looks for). Never over the game's own
    /// files (the core refuses).
    public static void SaveToOtherLocation(EditorModel editor, Window owner)
    {
        var maps = editor.UserMapsFolder;
        Directory.CreateDirectory(maps);
        var dialog = new SaveFileDialog
        {
            Title = "Maps saved in the game's Maps folder show in the game's map lists", InitialDirectory = maps,
            Filter = "Maps (*.map)|*.map", FileName = editor.MapScene.Name.Length == 0 ? "New Map" : editor.MapScene.Name,
            DefaultExt = ".map", OverwritePrompt = true,
        };
        if (dialog.ShowDialog(owner) != true) return;
        var path = dialog.FileName;
        var name = Path.GetFileNameWithoutExtension(path);
        if (string.Equals(Path.GetFullPath(Path.GetDirectoryName(path)!).TrimEnd('\\'), Path.GetFullPath(maps).TrimEnd('\\'),
                          StringComparison.OrdinalIgnoreCase))
        {
            path = Path.Combine(maps, name, name + ".map");
            if (File.Exists(path) && !string.Equals(path, editor.MapEdit.SavedPath, StringComparison.OrdinalIgnoreCase) &&
                MessageBox.Show(owner, $"{name} already exists in the Maps folder. Replace it?", "Save Map", MessageBoxButton.OKCancel,
                                MessageBoxImage.Question) != MessageBoxResult.OK)
            {
                return;
            }
        }
        editor.SaveMap(path);
    }
}
