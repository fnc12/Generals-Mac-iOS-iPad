using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using EndEditor.Render;
using EndEditor.Views;
using Microsoft.Win32;

namespace EndEditor;

/// The window: the menu, the toolbar (mode, status, Play), and the Objects or Maps screen.
public sealed class MainWindow : Window
{
    public readonly EditorModel Editor;
    public readonly ObjectsScreen Objects;
    public readonly MapScreen MapsScreen;
    private readonly ContentControl content;
    private readonly StackPanel mode;
    private readonly TextBlock status;
    private readonly Button play, playSettingsButton;
    public readonly Popup PlaySettings;
    private readonly PlaySettingsView playSettingsView;
    public string Mode { get; private set; } = "Maps";

    public MainWindow(EditorModel editor)
    {
        Editor = editor;
        Title = "END Editor";
        Width = editor.Settings.WindowWidth;
        Height = editor.Settings.WindowHeight;
        MinWidth = 1000;
        MinHeight = 600;
        this.Id("window");

        var root = new DockPanel();
        var menu = new Menu();
        DockPanel.SetDock(menu, Dock.Top);
        menu.Items.Add(MenuOf("_File",
                              Item("_Zero Hour Folder…", ChooseGameFolder, id: "menu.zeroHourFolder"),
                              Item("_Base Generals Folder…", ChooseBaseGameFolder, id: "menu.baseFolder"),
                              Item("_Open Map File…", ChooseMapFile, id: "menu.openMap"),
                              new Separator(),
                              Item("_Save Map", SaveMap, "Ctrl+S", "menu.save"),
                              Item("Save Map _As…", SaveMapAs, "Ctrl+Shift+S", "menu.saveAs"),
                              new Separator(),
                              Item("E_xit", Close, "Alt+F4")));
        menu.Items.Add(MenuOf("_Edit",
                              Item("_Undo", editor.Undo, "Ctrl+Z", "menu.undo"),
                              Item("_Redo", editor.Redo, "Ctrl+Y", "menu.redo"),
                              new Separator(),
                              Item("_Copy", editor.Copy, "Ctrl+C", "menu.copy"),
                              Item("_Paste", editor.Paste, "Ctrl+V", "menu.paste"),
                              Item("_Duplicate", editor.Duplicate, "Ctrl+D", "menu.duplicate"),
                              Item("Select _All", editor.SelectAll, "Ctrl+A", "menu.selectAll"),
                              Item("De_lete", () => editor.Send("edit.delete"), "Del", "menu.delete")));
        menu.Items.Add(MenuOf("_Map",
                              Item("_Play Map", editor.PlayMap, "Ctrl+R", "menu.play"),
                              Item("_Stop Game", editor.StopGame, id: "menu.stop")));
        root.Children.Add(menu);

        var toolbar = new DockPanel { Margin = new Thickness(8, 4, 8, 4) };
        DockPanel.SetDock(toolbar, Dock.Top);
        mode = Ui.Segments([("Objects", "Objects"), ("Maps", "Maps")], editor.Settings.Mode, SetMode, "toolbar.mode");
        DockPanel.SetDock(mode, Dock.Left);
        toolbar.Children.Add(mode);
        play = Ui.Button("▶ Play", editor.PlayMap, "toolbar.play").Tip("Play the map in the game, with its unsaved edits (Ctrl+R)");
        playSettingsButton = Ui.Button("⚙ Play Settings", () => PlaySettings!.IsOpen = !PlaySettings.IsOpen, "toolbar.playSettings")
            .Tip("Faction, color, AI opponents and the game app for Play");
        var right = Ui.Row(play, playSettingsButton);
        DockPanel.SetDock(right, Dock.Right);
        toolbar.Children.Add(right);
        status = Ui.Text("No game folder open", 12, Ui.Secondary).Id("toolbar.status");
        status.Margin = new Thickness(12, 0, 12, 0);
        toolbar.Children.Add(status);
        root.Children.Add(toolbar);

        playSettingsView = new PlaySettingsView(editor);
        PlaySettings = new Popup
        {
            Child = new Border { Child = playSettingsView, Background = SystemColors.WindowBrush, BorderBrush = SystemColors.ActiveBorderBrush, BorderThickness = new Thickness(1) },
            StaysOpen = false, Placement = PlacementMode.Bottom, PlacementTarget = playSettingsButton,
        };

        Objects = new ObjectsScreen(editor);
        MapsScreen = new MapScreen(editor, this);
        MapsScreen.View.Shortcuts = (key, modifiers) => Shortcut(key, modifiers, false);
        content = new ContentControl();
        root.Children.Add(content);
        Content = root;
        SetMode(editor.Settings.Mode == "Objects" ? "Objects" : "Maps");

        editor.Changed += Refresh;
        KeyDown += OnKeyDown;
        // The mouse over the window's controls is not over the map: Ctrl+V pastes in the map's middle.
        // (The map view is a window of its own; its moves do not reach here.)
        PreviewMouseMove += (_, _) => editor.CursorGround = null;
        // A click anywhere but in the object picker closes it, like the macOS popover.
        PreviewMouseDown += (_, e) =>
        {
            var inPicker = e.OriginalSource is DependencyObject source && MapsScreen.Bar.Picker.Child.IsAncestorOf(source);
            if (editor.MapEditor.Picker && !inPicker) editor.Send("placement.cancel");
        };
        Deactivated += (_, _) => editor.CursorGround = null;
        Closing += (_, _) =>
        {
            editor.Settings.WindowWidth = Width;
            editor.Settings.WindowHeight = Height;
            editor.Settings.Mode = Mode;
            editor.Settings.Save();
        };
        Refresh(Changes.Status | Changes.Objects | Changes.Maps | Changes.Map | Changes.Editor | Changes.Play | Changes.MapEdit);
    }

    private static MenuItem MenuOf(string header, params object[] items)
    {
        var menu = new MenuItem { Header = header };
        foreach (var item in items) menu.Items.Add(item);
        return menu;
    }

    private static MenuItem Item(string header, Action action, string? gesture = null, string? id = null)
    {
        var item = new MenuItem { Header = header, InputGestureText = gesture ?? "" };
        item.Click += (_, _) => action();
        if (id != null) item.Id(id);
        return item;
    }

    public void SetMode(string value)
    {
        Mode = value;
        Ui.Select(mode, value);
        content.Content = value == "Objects" ? Objects : MapsScreen;
    }

    private void Refresh(Changes changes)
    {
        if (Editor.CoreError is { } error)
        {
            Editor.CoreError = null;
            MessageBox.Show(this, error, "Editor core", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        if (changes.HasFlag(Changes.Status))
        {
            var s = Editor.Status;
            status.Text = s.State switch
            {
                "loading" => "Reading game data…",
                "ready" or "error" => s.Message,
                _ => "No game folder open",
            };
            status.Foreground = s.State == "error" ? Ui.ErrorBrush : Ui.Secondary;
            status.ToolTip = s.GameDir;
        }
        play.IsEnabled = Editor.CanEditMap;
        Objects.Refresh(changes);
        MapsScreen.Refresh(changes);
        // Rebuilt with the store's values, unless its game app field is being typed into.
        var typing = Keyboard.FocusedElement is TextBox field && playSettingsView.IsAncestorOf(field);
        if ((changes & (Changes.Play | Changes.Objects)) != 0 && !typing) playSettingsView.Refresh();
    }

    /// Ctrl shortcuts of the menus, from the window or the map view (which has the keyboard while
    /// it is focused). A text field being edited keeps its own undo, copy, paste and select all.
    private bool Shortcut(int key, KeyModifiers modifiers, bool inText)
    {
        var shift = modifiers.HasFlag(KeyModifiers.Shift);
        if (!modifiers.HasFlag(KeyModifiers.Control) || modifiers.HasFlag(KeyModifiers.Alt)) return false;
        switch (key)
        {
            case 0x5A when !inText: if (shift) Editor.Redo(); else Editor.Undo(); return true;  // Z
            case 0x59 when !inText: Editor.Redo(); return true;  // Y
            case 0x43 when !inText: Editor.Copy(); return true;  // C
            case 0x56 when !inText: Editor.Paste(); return true;  // V
            case 0x41 when !inText: Editor.SelectAll(); return true;  // A
            case 0x44: Editor.Duplicate(); return true;  // D
            case 0x53: if (shift) SaveMapAs(); else SaveMap(); return true;  // S
            case 0x52: Editor.PlayMap(); return true;  // R
        }
        return false;
    }

    private void OnKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Handled) return;
        var modifiers = KeyModifiers.None;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Shift)) modifiers |= KeyModifiers.Shift;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) modifiers |= KeyModifiers.Control;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Alt)) modifiers |= KeyModifiers.Alt;
        var key = KeyInterop.VirtualKeyFromKey(e.Key == Key.System ? e.SystemKey : e.Key);
        e.Handled = Shortcut(key, modifiers, Keyboard.FocusedElement is TextBox);
    }

    // MARK: Game folders and files

    public void ChooseGameFolder()
    {
        var dialog = new OpenFolderDialog { Title = "Choose the Zero Hour folder (the one with the .big files)" };
        if (dialog.ShowDialog(this) == true) Editor.OpenGame(dialog.FolderName);
    }

    public void ChooseBaseGameFolder()
    {
        var dialog = new OpenFolderDialog { Title = "Choose the base Generals folder (most models are in its .big files)" };
        if (dialog.ShowDialog(this) != true) return;
        Editor.Settings.BaseGameDir = dialog.FolderName;
        Editor.Settings.Save();
        if (Editor.Status.GameDir.Length > 0) Editor.OpenGame(Editor.Status.GameDir);
    }

    public void ChooseMapFile()
    {
        if (Editor.Status.State != "ready") return;
        var dialog = new OpenFileDialog { Title = "Choose a .map file", Filter = "Maps (*.map)|*.map" };
        if (dialog.ShowDialog(this) != true) return;
        SetMode("Maps");
        Editor.OpenMap(dialog.FileName);
    }

    /// Ctrl+S: over the file it was saved to last, else Save As.
    public void SaveMap()
    {
        if (!Editor.CanEditMap) return;
        if (Editor.MapEdit.SavedPath.Length > 0 && File.Exists(Editor.MapEdit.SavedPath)) Editor.SaveMap(Editor.MapEdit.SavedPath);
        else SaveMapAs();
    }

    /// The Save Map As sheet; its Other Location… opens the system dialog once it is gone.
    public SaveMapDialog? SaveSheet { get; private set; }

    public void SaveMapAs()
    {
        if (!Editor.CanEditMap || SaveSheet != null) return;
        SaveSheet = new SaveMapDialog(Editor, this);
        SaveSheet.ShowDialog();
        var other = SaveSheet.OtherLocationChosen;
        SaveSheet = null;
        if (other) SaveMapDialog.SaveToOtherLocation(Editor, this);
    }
}
