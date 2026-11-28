using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;

namespace EndEditor.Views;

/// Small builders for the code-made WPF UI. Every control the UI check drives has an AutomationId
/// (the same names as the macOS client's accessibility identifiers where there is one).
internal static class Ui
{
    public static readonly Brush Secondary = new SolidColorBrush(Color.FromRgb(0x6B, 0x6B, 0x6B));
    public static readonly Brush Tertiary = new SolidColorBrush(Color.FromRgb(0x9A, 0x9A, 0x9A));
    public static readonly Brush ErrorBrush = new SolidColorBrush(Color.FromRgb(0xC4, 0x2B, 0x1C));
    public static readonly Brush Warning = new SolidColorBrush(Color.FromRgb(0xCA, 0x6A, 0x00));

    public static T Id<T>(this T element, string id) where T : DependencyObject
    {
        AutomationProperties.SetAutomationId(element, id);
        return element;
    }

    public static T Tip<T>(this T element, string tip) where T : FrameworkElement
    {
        element.ToolTip = tip;
        return element;
    }

    public static TextBlock Text(string text, double size = 12, Brush? brush = null, FontWeight? weight = null) => new()
    {
        Text = text, FontSize = size, Foreground = brush ?? SystemColors.ControlTextBrush, FontWeight = weight ?? FontWeights.Normal,
        VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis,
    };

    public static Button Button(string content, Action click, string? id = null)
    {
        var button = new Button { Content = content, Padding = new Thickness(10, 3, 10, 3), Margin = new Thickness(2), MinWidth = 28 };
        button.Click += (_, _) => click();
        if (id != null) button.Id(id);
        return button;
    }

    /// A checkbox; `changed` runs however it is toggled (a click, the keyboard, UI Automation).
    public static CheckBox Check(string label, bool value, Action<bool> changed, string? id = null)
    {
        var box = new CheckBox { Content = label, IsChecked = value, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(4, 2, 8, 2) };
        box.Checked += (_, _) => changed(true);
        box.Unchecked += (_, _) => changed(false);
        if (id != null) box.Id(id);
        return box;
    }

    public static StackPanel Row(params UIElement[] children)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        foreach (var child in children) panel.Children.Add(child);
        return panel;
    }

    /// A text field that sends its value when Enter is pressed and shows the store's value otherwise.
    public static TextBox CommitField(string value, Action<string> commit, string id, double width = double.NaN)
    {
        var box = new TextBox { Text = value, Width = width, Margin = new Thickness(2), VerticalContentAlignment = VerticalAlignment.Center };
        box.Id(id);
        AutomationProperties.SetName(box, id);
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                commit(box.Text);
                e.Handled = true;
            }
        };
        return box;
    }

    /// A pop-up of (label, value) items; `chosen` gets the value of the item the user picks.
    public static ComboBox Choice<T>(IEnumerable<(string Label, T Value)> items, T selected, Action<T> chosen, string id,
                                     double width = double.NaN)
    {
        var box = new ComboBox { Width = width, Margin = new Thickness(2), VerticalAlignment = VerticalAlignment.Center };
        box.Id(id);
        AutomationProperties.SetName(box, id);
        int index = 0;
        foreach (var (label, value) in items)
        {
            box.Items.Add(new ComboBoxItem { Content = label, Tag = value });
            if (EqualityComparer<T>.Default.Equals(value, selected)) box.SelectedIndex = index;
            ++index;
        }
        box.SelectionChanged += (_, _) =>
        {
            if (box.SelectedItem is ComboBoxItem { Tag: T value } && !EqualityComparer<T>.Default.Equals(value, selected))
            {
                selected = value;
                chosen(value);
            }
        };
        return box;
    }

    /// Buttons of which one is on (a segmented control). `chosen` gets the value the user turns on;
    /// Select shows the value the store says (its Tag).
    public static StackPanel Segments(IEnumerable<(string Label, string Value)> items, string selected, Action<string> chosen, string id)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Tag = selected };
        panel.Id(id);
        foreach (var (label, value) in items)
        {
            var button = new ToggleButton { Content = label, IsChecked = value == selected, Padding = new Thickness(10, 3, 10, 3), Margin = new Thickness(0, 2, 0, 2) };
            button.Id(id + "." + value);
            button.Checked += (_, _) =>
            {
                if ((string)panel.Tag != value) chosen(value);
            };
            // The one that is on stays on: another one is chosen instead.
            button.Unchecked += (_, _) =>
            {
                if ((string)panel.Tag == value) button.IsChecked = true;
            };
            panel.Children.Add(button);
        }
        return panel;
    }

    public static void Select(StackPanel segments, string value)
    {
        segments.Tag = value;
        foreach (var child in segments.Children.OfType<ToggleButton>())
            child.IsChecked = AutomationProperties.GetAutomationId(child).EndsWith("." + value, StringComparison.Ordinal);
    }

    /// A labelled row of a form.
    public static Grid Field(string label, UIElement content, double labelWidth = 110)
    {
        var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(labelWidth) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var text = Text(label, 12, Secondary);
        grid.Children.Add(text);
        Grid.SetColumn((UIElement)content, 1);
        grid.Children.Add(content);
        return grid;
    }

    public static TextBlock Header(string text) => new()
    {
        Text = text, FontWeight = FontWeights.SemiBold, FontSize = 13, Margin = new Thickness(0, 10, 0, 4),
        TextTrimming = TextTrimming.CharacterEllipsis,
    };

    public static string Invariant(double value, string format = "0.0") => value.ToString(format, CultureInfo.InvariantCulture);

    public static Brush Rgb(uint rgb) => new SolidColorBrush(Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb));
}
