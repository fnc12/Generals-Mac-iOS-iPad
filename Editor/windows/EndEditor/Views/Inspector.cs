using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using EndEditor.Core;

namespace EndEditor.Views;

/// The selection: one object with every field, or several with what they share. Every change is a
/// selection.* command; the core applies it to all of them as one undo step.
public sealed class Inspector : StackPanel
{
    private readonly EditorModel editor;
    private string newKey = "", newType = "string", newValue = "";

    private static readonly (string Key, string Label)[] Flags =
    [
        ("objectEnabled", "Enabled"), ("objectIndestructible", "Indestructible"), ("objectUnsellable", "Unsellable"),
        ("objectPowered", "Powered"), ("objectTargetable", "Targetable"), ("objectRecruitableAI", "Recruitable by AI"),
    ];

    public Inspector(EditorModel editor)
    {
        this.editor = editor;
        Width = 310;
        this.Id("inspector");
    }

    private static string Degrees(float radians) => Ui.Invariant(radians * 180 / MathF.PI);
    private static string Decimal(string text) => text.Replace(',', '.').Trim();

    public void Refresh(Changes changes)
    {
        // The field with the keyboard focus gets it back after the rebuild.
        var focused = Keyboard.FocusedElement is DependencyObject element && IsAncestorOf(element)
            ? AutomationProperties.GetAutomationId(element) : null;
        Children.Clear();
        var objects = editor.MapSelection.Select(id => editor.MapScene.Objects.FirstOrDefault(o => o.Id == id))
            .OfType<MapObjectItem>().ToList();
        if (objects.Count == 0)
        {
            Children.Add(Ui.Text("No object selected", 12, Ui.Secondary));
            return;
        }
        var first = objects[0];
        var single = objects.Count == 1;
        var properties = editor.MapObjectProperties(first.Id);
        var others = objects.Skip(1).Select(o => editor.MapObjectProperties(o.Id)).ToList();
        MapObjectProperty? Shared(string key)
        {
            var mine = properties.FirstOrDefault(p => p.Key == key);
            return mine != null && others.All(list => list.Contains(mine)) ? mine : null;
        }
        void Set(string key, string type, string value) => editor.Send($"selection.property\t{key}\t{type}\t{value}");

        Children.Add(Ui.Header(single ? first.TemplateName : $"{objects.Count} objects").Id("inspector.title"));
        if (single)
        {
            if (first.Start > 0)
            {
                // A start waypoint: which start position it is, in its label's color.
                var dot = new Ellipse { Width = 12, Height = 12, Fill = Ui.Rgb(first.Color), Stroke = Brushes.Black, StrokeThickness = 1, Margin = new Thickness(0, 0, 6, 0) };
                var start = Ui.Row(dot, Ui.Text($"Start {first.Start}", 15, null, FontWeights.Bold)).Id("inspector.start");
                AutomationProperties.SetName(start, $"Start {first.Start}");
                start.ToolTip = $"Player_{first.Start}_Start: whoever starts at start position {first.Start} begins here and owns the objects of Start {first.Start}";
                Children.Add(Ui.Field("Start position", start));
            }
            Children.Add(Ui.Field("Kind", Ui.Text(first.Start > 0 ? $"waypoint, Start {first.Start}" : first.Kind)));
            Children.Add(Ui.Field("Position", Ui.Text($"{Ui.Invariant(first.Position.X)}, {Ui.Invariant(first.Position.Y)} (ground + {Ui.Invariant(first.Height)})")));
            if (editor.MapModels.FirstOrDefault(m => m.Id == first.ModelId) is { } model)
                Children.Add(Ui.Field("Model", Ui.Text(model.Ready ? model.Name : $"{model.Name} ({model.Message})")));
            if (first.Flags != 0) Children.Add(Ui.Field("Flags", Ui.Text($"0x{first.Flags:X}")));
        }
        var angle = Ui.CommitField(objects.All(o => o.Angle == first.Angle) ? Degrees(first.Angle) : "",
                                   text => editor.Send("selection.angle\t" + Decimal(text)), "inspector.angle", 60);
        Children.Add(Ui.Field("Angle", Ui.Row(angle, Ui.Text("°"),
                                               Ui.Button("−", () => editor.Send("edit.rotate\t-15"), "inspector.angleDown"),
                                               Ui.Button("+", () => editor.Send("edit.rotate\t15"), "inspector.angleUp"))));
        Children.Add(Ui.Field("Height", Ui.CommitField(objects.All(o => o.Height == first.Height) ? Ui.Invariant(first.Height) : "",
                                                       text => editor.Send("selection.height\t" + Decimal(text)), "inspector.height", 80)
                                         .WithAlignment()));
        var sameOwner = objects.All(o => o.Owner == first.Owner);
        var owners = new List<(string, string)>();
        if (!sameOwner) owners.Add(("Several", ""));
        owners.AddRange(editor.MapPlayers.Select(p => (p.Label, p.Team)));
        if (first.Owner.Length > 0 && editor.MapPlayers.All(p => p.Team != first.Owner)) owners.Add((first.Owner, first.Owner));
        Children.Add(Ui.Field("Owner", Ui.Choice(owners, sameOwner ? first.Owner : "", team => editor.Send("selection.owner\t" + team),
                                                 "inspector.owner")));
        Children.Add(Ui.Field("Name", Ui.CommitField(Shared("objectName")?.Value ?? "",
                                                     text => Set("objectName", Shared("objectName")?.Type == "unicode" ? "unicode" : "string", text),
                                                     "inspector.name")));
        Children.Add(Ui.Field("Script", Ui.CommitField(Shared("objectScriptAttachment")?.Value ?? "",
                                                       text => Set("objectScriptAttachment", "string", text), "inspector.script")));
        Children.Add(Ui.Field("Health %", Ui.CommitField(Shared("objectInitialHealth")?.Value ?? "",
                                                         text => Set("objectInitialHealth", "int", text), "inspector.health", 60)
                                           .WithAlignment()));
        foreach (var (key, label) in Flags)
            Children.Add(Ui.Check(label, Shared(key)?.Value == "true", on => Set(key, "bool", on ? "true" : "false"), "inspector.flag." + key));
        var buttons = Ui.Row();
        if (objects.Count == 2 && objects.All(o => o.Kind == "waypoint"))
            buttons.Children.Add(Ui.Button("Link Waypoints", () => editor.Send("waypoint.link"), "inspector.link"));
        var delete = Ui.Button("Delete", () => editor.Send("edit.delete"), "inspector.delete");
        delete.Foreground = Ui.ErrorBrush;
        buttons.Children.Add(delete);
        buttons.Margin = new Thickness(0, 6, 0, 0);
        Children.Add(buttons);

        Children.Add(Ui.Header(single ? "Properties" : $"Properties of {first.TemplateName}"));
        foreach (var property in properties)
        {
            var field = Ui.CommitField(property.Value, text => Set(property.Key, property.Type, text), "inspector.property." + property.Key);
            field.ToolTip = property.Type;
            var remove = Ui.Button("−", () => editor.Send("selection.unset\t" + property.Key), "inspector.remove." + property.Key)
                .Tip($"Remove {property.Key}");
            var row = new DockPanel();
            DockPanel.SetDock(remove, Dock.Right);
            row.Children.Add(remove);
            row.Children.Add(field);
            var labelled = Ui.Field(property.Key, row, 140);
            labelled.ToolTip = property.Key;
            Children.Add(labelled);
        }
        var keyBox = new TextBox { Text = newKey, Width = 100, Margin = new Thickness(2) }.Id("inspector.newKey");
        AutomationProperties.SetName(keyBox, "key");
        keyBox.TextChanged += (_, _) => newKey = keyBox.Text;
        var typeBox = Ui.Choice(new[] { "string", "int", "real", "bool", "unicode" }.Select(t => (t, t)), newType, t => newType = t,
                                "inspector.newType", 72);
        var valueBox = new TextBox { Text = newValue, Width = 70, Margin = new Thickness(2) }.Id("inspector.newValue");
        AutomationProperties.SetName(valueBox, "value");
        valueBox.TextChanged += (_, _) => newValue = valueBox.Text;
        var add = Ui.Button("Add", () =>
        {
            if (newKey.Length == 0) return;
            var (key, type, value) = (newKey, newType, newValue);
            newKey = "";
            newValue = "";
            Set(key, type, value);
        }, "inspector.add");
        Children.Add(Ui.Row(keyBox, typeBox, valueBox, add));

        if (focused != null)
        {
            var again = FindById(this, focused);
            if (again is IInputElement input) Keyboard.Focus(input);
        }
    }

    private static DependencyObject? FindById(DependencyObject root, string id)
    {
        foreach (var child in LogicalTreeHelper.GetChildren(root).OfType<DependencyObject>())
        {
            if (AutomationProperties.GetAutomationId(child) == id) return child;
            if (FindById(child, id) is { } found) return found;
        }
        return null;
    }
}

internal static class InspectorExtensions
{
    public static TextBox WithAlignment(this TextBox box)
    {
        box.HorizontalAlignment = HorizontalAlignment.Left;
        return box;
    }
}
