using System.ComponentModel;
using System.Numerics;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using EndEditor.Core;
using EndEditor.Render;

namespace EndEditor.Views;

/// A side of the sidebar: "All objects (n)" or "<side> (n)".
public sealed record SideEntry(string? Side, string Label)
{
    public override string ToString() => Label;
}

/// The Objects mode: sides on the left, the object table with search, the selected object's
/// model preview and details.
public sealed class ObjectsScreen : Grid
{
    private readonly EditorModel editor;
    public readonly ListBox Sides;
    public readonly TextBox Search;
    public readonly DataGrid Table;
    public readonly ModelPreviewPanel Preview;
    private readonly StackPanel details;
    private string? side;

    public ObjectsScreen(EditorModel editor)
    {
        this.editor = editor;
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star), MinWidth = 420 });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(440) });

        Sides = new ListBox { Margin = new Thickness(6) }.Id("objects.sides");
        Sides.SelectionChanged += (_, _) =>
        {
            if (Sides.SelectedItem is SideEntry entry)
            {
                side = entry.Side;
                FillTable();
            }
        };
        Children.Add(Sides);

        var center = new DockPanel { Margin = new Thickness(0, 6, 0, 6) };
        SetColumn(center, 1);
        Search = new TextBox { Margin = new Thickness(0, 0, 0, 6) }.Id("objects.search");
        AutomationProperties.SetName(Search, "Name, model or KindOf");
        Search.ToolTip = "Name, model or KindOf";
        Search.TextChanged += (_, _) => FillTable();
        DockPanel.SetDock(Search, Dock.Top);
        center.Children.Add(Search);
        Table = new DataGrid
        {
            AutoGenerateColumns = false, IsReadOnly = true, SelectionMode = DataGridSelectionMode.Single, HeadersVisibility = DataGridHeadersVisibility.Column,
            CanUserSortColumns = true, GridLinesVisibility = DataGridGridLinesVisibility.None, EnableRowVirtualization = true,
        }.Id("objects.table");
        foreach (var (header, path) in new[] { ("Name", "Name"), ("Side", "Side"), ("Sorting", "EditorSorting"), ("Buildable", "Buildable"), ("Model", "Model") })
            Table.Columns.Add(new DataGridTextColumn { Header = header, Binding = new Binding(path), SortMemberPath = path, Width = header == "Name" ? new DataGridLength(2, DataGridLengthUnitType.Star) : new DataGridLength(1, DataGridLengthUnitType.Star) });
        Table.SelectionChanged += (_, _) => ShowSelected();
        center.Children.Add(Table);
        Children.Add(center);

        var right = new DockPanel { Margin = new Thickness(8) };
        SetColumn(right, 2);
        Preview = new ModelPreviewPanel(editor);
        DockPanel.SetDock(Preview, Dock.Top);
        right.Children.Add(Preview);
        details = new StackPanel();
        right.Children.Add(new ScrollViewer { Content = details, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        Children.Add(right);
        ShowSelected();
    }

    public ObjectItem? Selected => Table.SelectedItem as ObjectItem;

    private void FillSides()
    {
        var entries = new List<SideEntry> { new(null, $"All objects ({editor.Objects.Count})") };
        entries.AddRange(editor.Objects.GroupBy(o => o.Side).Select(g => (g.Key, Count: g.Count()))
                             .OrderByDescending(g => g.Count).ThenBy(g => g.Key, StringComparer.Ordinal)
                             .Select(g => new SideEntry(g.Key, $"{(g.Key.Length == 0 ? "(no side)" : g.Key)} ({g.Count})")));
        Sides.ItemsSource = entries;
        Sides.SelectedItem = entries.FirstOrDefault(e => e.Side == side) ?? entries[0];
    }

    private void FillTable()
    {
        var search = Search.Text.Trim();
        var selected = Selected?.Name;
        var sort = Table.Items.SortDescriptions.ToList();
        var shown = editor.Objects.Where(o => (side == null || o.Side == side) &&
                                              (search.Length == 0 || o.Name.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                                               o.Model.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                                               o.KindOf.Contains(search, StringComparison.OrdinalIgnoreCase))).ToList();
        Table.ItemsSource = shown;
        foreach (var description in sort) Table.Items.SortDescriptions.Add(description);
        if (selected != null && shown.FirstOrDefault(o => o.Name == selected) is { } again) Table.SelectedItem = again;
    }

    private void ShowSelected()
    {
        details.Children.Clear();
        if (Selected is not { } item)
        {
            details.Children.Add(Ui.Text("Select an object", 12, Ui.Secondary));
            Preview.Visibility = Visibility.Collapsed;
            return;
        }
        Preview.Visibility = Visibility.Visible;
        editor.ShowModel(item);
        editor.Send("placement.template\t" + item.Name);  // what a right click on a map places
        details.Children.Add(Ui.Header(item.Name));
        string Dash(string text) => text.Length == 0 ? "—" : text;
        details.Children.Add(Ui.Field("Definition", Ui.Text(item.Definition)));
        if (item.Parent.Length > 0) details.Children.Add(Ui.Field("Copies", Ui.Text(item.Parent)));
        details.Children.Add(Ui.Field("Side", Ui.Text(Dash(item.Side))));
        details.Children.Add(Ui.Field("Editor sorting", Ui.Text(Dash(item.EditorSorting))));
        details.Children.Add(Ui.Field("Buildable", Ui.Text(Dash(item.Buildable))));
        details.Children.Add(Ui.Field("Model", Ui.Text(Dash(item.Model)).Id("objects.model")));
        details.Children.Add(Ui.Header("KindOf"));
        details.Children.Add(new TextBox { Text = Dash(item.KindOf), IsReadOnly = true, TextWrapping = TextWrapping.Wrap, BorderThickness = new Thickness(0) });
        details.Children.Add(Ui.Header("Source"));
        details.Children.Add(new TextBox { Text = $"{item.SourceFile}:{item.Line}", IsReadOnly = true, FontFamily = new FontFamily("Consolas"), BorderThickness = new Thickness(0) }.Id("objects.source"));
    }

    public void Refresh(Changes changes)
    {
        if (changes.HasFlag(Changes.Objects))
        {
            FillSides();
            FillTable();
        }
        Preview.Refresh(changes);
    }
}

/// The model preview with its controls: animation, play/pause, hidden parts.
public sealed class ModelPreviewPanel : StackPanel
{
    private readonly EditorModel editor;
    public readonly D3DHost Host = new();
    public readonly PreviewRenderer Renderer = new();
    public readonly ComboBox Animation;
    public readonly Button PlayPause;
    public readonly CheckBox HiddenParts;
    private readonly TextBlock message, state;
    private Vector2? last;
    private bool filling;

    public ModelPreviewPanel(EditorModel editor)
    {
        this.editor = editor;
        Host.Renderer = Renderer;
        var frame = new Grid { Height = 300 };
        frame.Children.Add(new Border { Child = Host }.Id("preview.view"));
        Children.Add(frame);
        state = Ui.Text("", 12, Ui.Secondary);
        Animation = new ComboBox { Width = 220, Margin = new Thickness(0, 6, 6, 0) }.Id("preview.animation");
        AutomationProperties.SetName(Animation, "Animation");
        Animation.SelectionChanged += (_, _) =>
        {
            if (!filling && Animation.SelectedItem is ComboBoxItem { Tag: string name } && name != editor.Preview.Animation)
                editor.PlayAnimation(name);
        };
        PlayPause = Ui.Button("Pause", Toggle, "preview.playPause");
        PlayPause.Margin = new Thickness(0, 6, 0, 0);
        Children.Add(Ui.Row(Animation, PlayPause, state));
        HiddenParts = Ui.Check("Hidden parts", false, on => Renderer.Playback.ShowHidden = on, "preview.hiddenParts");
        message = Ui.Text("", 11, Ui.Secondary);
        Children.Add(Ui.Row(HiddenParts, message));
        Children.Add(Ui.Text("Drag to turn, the wheel zooms", 10, Ui.Tertiary));
        Host.MouseDownAt += (_, p, _) => last = p;
        Host.MouseMoveAt += (p, _, down) =>
        {
            if (!down || last is not { } previous) return;
            last = p;
            Renderer.Playback.Yaw -= (p.X - previous.X) * 0.01f;
            Renderer.Playback.Pitch = Math.Clamp(Renderer.Playback.Pitch + (p.Y - previous.Y) * 0.01f, -1.5f, 1.5f);
        };
        Host.MouseUpAt += (_, _, _) => last = null;
        Host.Wheel += (notches, _, _) => Renderer.Playback.Zoom = Math.Clamp(Renderer.Playback.Zoom * (1 - notches * 0.1f), 0.1f, 10);
    }

    private void Toggle()
    {
        var playback = Renderer.Playback;
        var model = editor.Preview;
        if (playback.Playing) playback.PausedFrame = playback.Frame(model.FrameCount, model.FrameRate);
        else playback.StartTime = playback.Now - playback.PausedFrame / (double)Math.Max(model.FrameRate, 1);
        playback.Playing = !playback.Playing;
        PlayPause.Content = playback.Playing ? "Pause" : "Play";
    }

    public void Refresh(Changes changes)
    {
        if (!changes.HasFlag(Changes.Preview)) return;
        var model = editor.Preview;
        Renderer.Show(model);
        filling = true;
        Animation.Items.Clear();
        Animation.Items.Add(new ComboBoxItem { Content = "Bind pose", Tag = "" });
        foreach (var name in editor.Animations) Animation.Items.Add(new ComboBoxItem { Content = name, Tag = name });
        Animation.SelectedIndex = Math.Max(editor.Animations.IndexOf(model.Animation) + 1, 0);
        Animation.IsEnabled = editor.Animations.Count > 0;
        filling = false;
        PlayPause.IsEnabled = model.FrameCount > 1;
        state.Text = editor.ModelState switch { "loading" => "Loading…", "error" => editor.ModelMessage, _ => "" };
        message.Text = editor.ModelMessage;
    }
}
