using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using EndEditor.Core;

namespace EndEditor.Views;

/// A map of the list, as the UI check and screen readers see it (its name).
public sealed record MapEntry(MapListItem Item)
{
    public override string ToString() => Item.Name;
}

/// An object template of the picker.
public sealed record TemplateEntry(ObjectItem Item)
{
    public override string ToString() => Item.Name;
}

/// The Maps mode: the map list with search on the left; the map with its placement bar, display
/// toggles and the selection's inspector on the right.
public sealed class MapScreen : Grid
{
    private readonly EditorModel editor;
    public readonly MapView View;
    public readonly TextBox Search;
    public readonly ListBox List;
    private readonly TextBlock overlay;
    private readonly Border viewFrame;
    public readonly PlacementBar Bar;
    public readonly Inspector Inspector;
    private readonly ColumnDefinition inspectorColumn;
    private readonly ProgressBar modelProgress;
    private readonly TextBlock modelText, mapMessage;

    public MapScreen(EditorModel editor, MainWindow window)
    {
        this.editor = editor;
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(260), MinWidth = 180 });
        ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        inspectorColumn = new ColumnDefinition { Width = new GridLength(0) };
        ColumnDefinitions.Add(inspectorColumn);

        // The map list.
        var left = new DockPanel { Margin = new Thickness(6) };
        Search = new TextBox { Margin = new Thickness(0, 0, 0, 6) }.Id("maps.search");
        AutomationProperties.SetName(Search, "Map name");
        Search.TextChanged += (_, _) => FillList();
        DockPanel.SetDock(Search, Dock.Top);
        left.Children.Add(Search);
        List = new ListBox().Id("maps.list");
        VirtualizingPanel.SetIsVirtualizing(List, true);
        List.ItemTemplate = (DataTemplate)System.Windows.Markup.XamlReader.Parse("""
            <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
              <StackPanel Margin="0,2,0,2">
                <TextBlock Text="{Binding Item.Name}"/>
                <TextBlock Text="{Binding Item.Source}" FontSize="10" Foreground="#888" TextTrimming="CharacterEllipsis"/>
              </StackPanel>
            </DataTemplate>
            """);
        List.SelectionChanged += (_, _) =>
        {
            if (!filling && List.SelectedItem is MapEntry entry) editor.OpenMap(entry.Item.Path);
        };
        left.Children.Add(List);
        Children.Add(left);
        var splitter = new GridSplitter { Width = 4, HorizontalAlignment = HorizontalAlignment.Stretch, Background = Brushes.Transparent };
        SetColumn(splitter, 1);
        Children.Add(splitter);

        // The map.
        var center = new DockPanel();
        SetColumn(center, 2);
        var hint = Ui.Text("Drag to pan, Alt-drag to turn, the wheel zooms at the cursor, click to select, Shift-click or Shift-drag to add. " +
                           "Right-click places, drag the selection to move it, [ ] turn it, Delete removes it, " +
                           "Ctrl+C Ctrl+V Ctrl+D copy, paste, duplicate, Ctrl+Z undoes. \"Start N\" marks start positions", 10, Ui.Tertiary);
        hint.Margin = new Thickness(10, 0, 10, 4);
        DockPanel.SetDock(hint, Dock.Bottom);
        center.Children.Add(hint);
        var toggles = new DockPanel { Margin = new Thickness(10, 4, 10, 4) };
        var showModels = Ui.Check("Models", true, on => { View!.Renderer.ShowModels = on; }, "map.showModels");
        var showMarkers = Ui.Check("Markers", true, on => { View!.Renderer.ShowMarkers = on; }, "map.showMarkers")
            .Tip("Waypoints and their links, roads, sounds and objects whose model is missing");
        var showWater = Ui.Check("Water", true, on => { View!.Renderer.ShowWater = on; }, "map.showWater");
        var checks = Ui.Row(showModels, showMarkers, showWater);
        DockPanel.SetDock(checks, Dock.Left);
        toggles.Children.Add(checks);
        modelProgress = new ProgressBar { Width = 100, Height = 10, Margin = new Thickness(8, 0, 4, 0), Visibility = Visibility.Collapsed };
        modelText = Ui.Text("", 11);
        mapMessage = Ui.Text("", 11, Ui.Secondary).Id("map.message");
        var status = Ui.Row(modelProgress, modelText, new Border { Width = 8 }, mapMessage);
        status.HorizontalAlignment = HorizontalAlignment.Right;
        toggles.Children.Add(status);
        DockPanel.SetDock(toggles, Dock.Bottom);
        center.Children.Add(toggles);
        Bar = new PlacementBar(editor, window);
        DockPanel.SetDock(Bar, Dock.Bottom);
        center.Children.Add(Bar);
        var stack = new Grid();
        View = new MapView(editor);
        viewFrame = new Border { Child = View, Background = new SolidColorBrush(Color.FromRgb(31, 33, 38)) };
        stack.Children.Add(viewFrame);
        overlay = Ui.Text("Choose a map", 13, Ui.Secondary);
        overlay.HorizontalAlignment = HorizontalAlignment.Center;
        overlay.Id("map.state");
        stack.Children.Add(overlay);
        center.Children.Add(stack);
        Children.Add(center);

        Inspector = new Inspector(editor);
        var inspectorScroll = new ScrollViewer { Content = Inspector, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(10) };
        var inspectorBorder = new Border { Child = inspectorScroll, BorderBrush = SystemColors.ActiveBorderBrush, BorderThickness = new Thickness(1, 0, 0, 0) };
        SetColumn(inspectorBorder, 3);
        Children.Add(inspectorBorder);
    }

    private void FillList()
    {
        var search = Search.Text.Trim();
        var selected = (List.SelectedItem as MapEntry)?.Item.Path;
        var shown = editor.Maps.Where(m => search.Length == 0 || m.Name.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Select(m => new MapEntry(m)).ToList();
        // The open map stays selected without opening it again.
        filling = true;
        List.ItemsSource = shown;
        List.SelectedItem = shown.FirstOrDefault(e => e.Item.Path == selected);
        filling = false;
    }

    private bool filling;

    public void Refresh(Changes changes)
    {
        if (changes.HasFlag(Changes.Maps)) FillList();
        View.Refresh(changes);
        if ((changes & (Changes.Map | Changes.MapModels | Changes.MapEdit)) != 0)
        {
            var state = editor.MapState;
            // The map view is a window of its own: text over it would not show (airspace), so it
            // takes the view's place while there is nothing to draw.
            overlay.Text = state switch
            {
                "loading" => "Reading the map…",
                "error" => editor.MapMessage,
                "idle" => "Choose a map",
                _ => "",
            };
            overlay.Visibility = state == "ready" ? Visibility.Collapsed : Visibility.Visible;
            viewFrame.Visibility = state == "ready" ? Visibility.Visible : Visibility.Hidden;
            var loading = editor.MapModelCount > 0 && editor.MapModelsLoaded < editor.MapModelCount;
            modelProgress.Visibility = loading ? Visibility.Visible : Visibility.Collapsed;
            modelProgress.Maximum = Math.Max(editor.MapModelCount, 1);
            modelProgress.Value = editor.MapModelsLoaded;
            modelText.Text = loading ? $"Models {editor.MapModelsLoaded}/{editor.MapModelCount}" : "";
            mapMessage.Text = editor.MapMessage;
            mapMessage.ToolTip = editor.MapWarnings.Count > 0 ? string.Join("\n", editor.MapWarnings) : null;
        }
        Bar.Refresh(changes);
        if ((changes & (Changes.Selection | Changes.MapObjects | Changes.Map | Changes.MapModels | Changes.MapEdit)) != 0)
        {
            inspectorColumn.Width = editor.MapSelection.Count > 0 ? new GridLength(340) : new GridLength(0);
            Inspector.Refresh(changes);
        }
    }
}

/// The placement tool (object, waypoint, road, bridge) with what it places; undo, redo, save and
/// the core's note.
public sealed class PlacementBar : DockPanel
{
    private readonly EditorModel editor;
    private readonly StackPanel tools, toolOptions;
    private readonly Button undo, redo;
    private readonly TextBlock edited, note;
    public readonly Popup Picker;
    private readonly TextBox pickerSearch;
    private readonly ListBox pickerList;
    private string shownTool = "";
    private Button? templateButton;

    public PlacementBar(EditorModel editor, MainWindow window)
    {
        this.editor = editor;
        Margin = new Thickness(10, 6, 10, 0);
        tools = Ui.Segments([("Object", "object"), ("Waypoint", "waypoint"), ("Road", "road"), ("Bridge", "bridge")], "object",
                            tool => editor.Send("placement." + tool), "placement.tool");
        tools.ToolTip = "What a right click on the map places";
        SetDock(tools, Dock.Left);
        Children.Add(tools);
        undo = Ui.Button("↶ Undo", () => editor.Send("map.undo"), "placement.undo").Tip("Undo (Ctrl+Z)");
        redo = Ui.Button("↷ Redo", () => editor.Send("map.redo"), "placement.redo").Tip("Redo (Ctrl+Y)");
        var saveAs = Ui.Button("Save As…", window.SaveMapAs, "placement.saveAs").Tip("Save the map with its edits and preview (Ctrl+Shift+S)");
        edited = Ui.Text("Edited", 11, Ui.Warning);
        note = Ui.Text("", 11, Ui.Secondary).Id("placement.note");
        note.MaxWidth = 380;
        var right = Ui.Row(undo, redo, saveAs, edited, new Border { Width = 6 }, note);
        SetDock(right, Dock.Right);
        Children.Add(right);
        toolOptions = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(10, 0, 0, 0) };
        Children.Add(toolOptions);

        // The object picker: a searchable list of the templates, opened when the store says so.
        pickerSearch = new TextBox { Margin = new Thickness(0, 0, 0, 6) }.Id("picker.search");
        AutomationProperties.SetName(pickerSearch, "Name or side");
        pickerSearch.TextChanged += (_, _) => FillPicker();
        pickerList = new ListBox { Width = 340, Height = 420 }.Id("picker.list");
        pickerList.ItemTemplate = (DataTemplate)System.Windows.Markup.XamlReader.Parse("""
            <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
              <DockPanel Width="300">
                <TextBlock DockPanel.Dock="Right" Text="{Binding Item.Side}" Foreground="#888" FontSize="11"/>
                <TextBlock Text="{Binding Item.Name}"/>
              </DockPanel>
            </DataTemplate>
            """);
        pickerList.SelectionChanged += (_, _) =>
        {
            if (pickerList.SelectedItem is TemplateEntry entry)
            {
                editor.Send("placement.template\t" + entry.Item.Name);
                pickerList.SelectedItem = null;
            }
        };
        var pickerPanel = new DockPanel { Margin = new Thickness(8) };
        DockPanel.SetDock(pickerSearch, Dock.Top);
        pickerPanel.Children.Add(pickerSearch);
        pickerPanel.Children.Add(pickerList);
        Picker = new Popup
        {
            Child = new Border { Child = pickerPanel, Background = SystemColors.WindowBrush, BorderBrush = SystemColors.ActiveBorderBrush, BorderThickness = new Thickness(1) },
            // Open while the store says so: a right click opens it while the mouse button is still
            // down, which would close a pop-up that closes itself on clicks outside it. A click
            // elsewhere (MainWindow, MapView) or Esc cancels it, like the macOS popover.
            StaysOpen = true, Placement = PlacementMode.Top, PlacementTarget = this, AllowsTransparency = false,
        };
        Picker.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) editor.Send("placement.cancel");
        };
        Refresh(Changes.Editor | Changes.MapEdit);
    }

    private void FillPicker()
    {
        var search = pickerSearch.Text.Trim();
        pickerList.ItemsSource = editor.Objects
            .Where(o => search.Length == 0 || o.Name.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                        o.Side.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Select(o => new TemplateEntry(o)).ToList();
    }

    private void BuildToolOptions(MapEditorState state)
    {
        toolOptions.Children.Clear();
        templateButton = null;
        switch (state.Tool)
        {
            case "object":
                templateButton = Ui.Button(state.TemplateName.Length == 0 ? "Choose an object…" : state.TemplateName,
                                           () => editor.Send("placement.pick"), "placement.template");
                toolOptions.Children.Add(templateButton);
                toolOptions.Children.Add(Ui.Text("Owner", 12, Ui.Secondary));
                toolOptions.Children.Add(Ui.Choice(editor.MapPlayers.Select(p => (p.Label, p.Team)), state.Team,
                                                   team => editor.Send("placement.owner\t" + team), "placement.owner", 180)
                                             .Tip("Placed objects belong to this player's default team"));
                toolOptions.Children.Add(Ui.Button("−", () => editor.Send("placement.angle\t" + EditorModel.Number(state.Angle - 15)),
                                                   "placement.angle.down"));
                toolOptions.Children.Add(Ui.Text($"{state.Angle:0}°").Id("placement.angle"));
                toolOptions.Children.Add(Ui.Button("+", () => editor.Send("placement.angle\t" + EditorModel.Number(state.Angle + 15)),
                                                   "placement.angle.up"));
                break;
            case "road":
            case "bridge":
                var bridge = state.Tool == "bridge";
                toolOptions.Children.Add(Ui.Choice(editor.RoadTypes.Where(t => t.Bridge == bridge).Select(t => (t.Name, t.Name)), state.RoadType,
                                                   type => editor.Send((bridge ? "placement.bridge\t" : "placement.road\t") + type),
                                                   "placement.road", 240));
                toolOptions.Children.Add(Ui.Text(state.RoadStart == null ? "Right-click the start, then the end" : "Right-click the end, Esc cancels",
                                                 11, Ui.Secondary));
                break;
            default:
                toolOptions.Children.Add(Ui.Text("Right-click places a waypoint linked from the selected one; right-click a waypoint to link it",
                                                 11, Ui.Secondary));
                break;
        }
    }

    private MapEditorState built = MapEditorState.Initial with { Tool = "" };

    public void Refresh(Changes changes)
    {
        IsEnabled = editor.CanEditMap;
        var state = editor.MapEditor;
        if ((changes & (Changes.Editor | Changes.Map | Changes.Objects)) != 0 &&
            (state.Tool != built.Tool || state.TemplateName != built.TemplateName || state.Team != built.Team ||
             state.Angle != built.Angle || state.RoadType != built.RoadType || (state.RoadStart == null) != (built.RoadStart == null) ||
             changes.HasFlag(Changes.Map)))
        {
            built = state;
            BuildToolOptions(state);
        }
        if (shownTool != state.Tool)
        {
            shownTool = state.Tool;
            Ui.Select(tools, state.Tool);
        }
        undo.IsEnabled = editor.MapEdit.UndoCount > 0;
        redo.IsEnabled = editor.MapEdit.RedoCount > 0;
        edited.Visibility = editor.MapEdit.Edited ? Visibility.Visible : Visibility.Collapsed;
        note.Text = state.Note;
        note.ToolTip = state.Note.Length > 0 ? state.Note : null;
        note.Foreground = state.NoteKind == "error" ? Ui.ErrorBrush : Ui.Secondary;
        if (state.Picker != Picker.IsOpen)
        {
            if (state.Picker)
            {
                FillPicker();
                Picker.PlacementTarget = (UIElement?)templateButton ?? this;
                Picker.IsOpen = true;
                pickerSearch.Focus();
            }
            else
            {
                Picker.IsOpen = false;
            }
        }
    }
}
