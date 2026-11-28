using System.Numerics;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using EndEditor.Core;
using EndEditor.Render;

namespace EndEditor.Views;

/// The 3D map: turns mouse and key input into core commands (map.press, map.drag, map.release,
/// map.click_right, edit.*) with what needs the camera: the ground point under the cursor, the
/// objects under it and those inside a rubber band. What an input means (select, move, place,
/// pan) is the core's decision; the view moves its camera when the store's gesture says "pan" or
/// "orbit". The wheel zooms at the cursor. Ctrl is sent as "cmd" (what ⌘ is on the Mac).
public sealed class MapView : Border
{
    public readonly D3DHost Host = new();
    public readonly MapRenderer Renderer;
    private readonly EditorModel editor;
    private Vector2? downPoint, lastPoint;
    private bool dragged;
    private (Vector2 A, Vector2 B)? band;
    private string sceneKey = "";

    public MapView(EditorModel editor)
    {
        this.editor = editor;
        Renderer = new MapRenderer();
        Host.Renderer = Renderer;
        Renderer.OverlaySource = () => new MapOverlay(editor.MapSelection, editor.MapGesture, editor.MapEditor.RoadStart,
                                                      editor.MapEditor.Pending, band);
        Child = Host;
        AutomationProperties.SetAutomationId(this, "map.view");
        Host.MouseDownAt += OnMouseDown;
        Host.MouseMoveAt += OnMouseMove;
        Host.MouseUpAt += OnMouseUp;
        Host.Wheel += OnWheel;
        Host.KeyPressed += OnKey;
        Host.CharTyped += OnChar;
    }

    public Vector2 Size => Host.PixelSize;

    /// Takes what the store says about the map: a new scene, edits, models.
    public void Refresh(Changes changes)
    {
        if ((changes & (Changes.Map | Changes.MapObjects)) != 0) Renderer.Show(editor.MapScene);
        if (editor.MapScene.Key != sceneKey)
        {
            sceneKey = editor.MapScene.Key;
            Renderer.Show(editor.MapScene);
        }
        if (Renderer.LoadedModelCount < editor.MapModels.Count)
            Renderer.Add(editor.MapModels.Skip(Renderer.LoadedModelCount), editor.MapModelTextures);
    }

    private Vector2? Ground(Vector2 point) =>
        Renderer.Camera.GroundPoint(point, Size, Renderer.Scene) is { } p ? new Vector2(p.X, p.Y) : null;

    /// "x\ty" of the ground under the cursor, "\t" when it is off the map.
    private string GroundArgument(Vector2 point) =>
        Ground(point) is { } g ? EditorModel.Number(g.X) + "\t" + EditorModel.Number(g.Y) : "\t";

    private string HitsArgument(Vector2 point) => string.Join(',', Renderer.Hits(point, Size));

    private static string ModifiersArgument(KeyModifiers modifiers)
    {
        var names = new List<string>();
        if (modifiers.HasFlag(KeyModifiers.Shift)) names.Add("shift");
        if (modifiers.HasFlag(KeyModifiers.Alt)) names.Add("alt");
        if (modifiers.HasFlag(KeyModifiers.Control)) names.Add("cmd");
        return string.Join(',', names);
    }

    private void OnMouseDown(MouseButton button, Vector2 point, KeyModifiers modifiers)
    {
        // A click outside the open object picker closes it (and only that).
        if (editor.MapEditor.Picker)
        {
            editor.Send("placement.cancel");
            return;
        }
        if (button == MouseButton.Right)
        {
            editor.Send("map.click_right\t" + GroundArgument(point) + "\t" + HitsArgument(point));
            return;
        }
        if (button != MouseButton.Left) return;
        downPoint = point;
        lastPoint = point;
        dragged = false;
        editor.Send("map.press\t" + GroundArgument(point) + "\t" + ModifiersArgument(modifiers) + "\t" + HitsArgument(point));
    }

    private void OnMouseMove(Vector2 point, KeyModifiers modifiers, bool leftDown)
    {
        editor.CursorGround = Ground(point);  // where Ctrl+V pastes
        if (!leftDown || downPoint is not { } down) return;
        var last = lastPoint ?? point;
        var dx = point.X - last.X;
        var dy = point.Y - last.Y;  // down is positive
        lastPoint = point;
        if (!dragged)
        {
            dragged = Vector2.Distance(point, down) > 3 * Host.Scale;
            if (!dragged) return;
        }
        editor.Send("map.drag\t" + GroundArgument(point));
        switch (editor.MapGesture.Kind)
        {
            case "pan": Renderer.Camera.Pan(dx, dy, Size.Y); break;
            case "orbit": Renderer.Camera.Rotate(dx, dy); break;
            case "band": band = (down, point); break;
        }
    }

    private void OnMouseUp(MouseButton button, Vector2 point, KeyModifiers modifiers)
    {
        if (button != MouseButton.Left || downPoint is not { } down) return;
        var inside = "";
        if (editor.MapGesture.Kind == "band")
        {
            var rect = new Rect(new Point(down.X, down.Y), new Point(point.X, point.Y));
            inside = string.Join(',', Renderer.ObjectsIn(rect, Size));
        }
        downPoint = null;
        lastPoint = null;
        band = null;
        editor.Send("map.release\t" + inside);
    }

    /// A wheel notch zooms by 10% at the cursor.
    private void OnWheel(float notches, Vector2 point, KeyModifiers modifiers) =>
        Renderer.Camera.Zoom(1 - notches * 0.1f, point, Size, Renderer.Scene);

    private bool OnKey(int key, KeyModifiers modifiers)
    {
        switch (key)
        {
            case 0x2E or 0x08 when modifiers == KeyModifiers.None:  // Delete, Backspace
                editor.Send("edit.delete");
                return true;
            case 0x1B:  // Esc
                editor.Send("edit.cancel");
                return true;
            case 0x12:  // Alt alone: no menu bar focus while it turns the camera
                return true;
        }
        // Ctrl shortcuts (Ctrl+Z, Ctrl+C...): the window's, also while the map has the focus.
        return modifiers.HasFlag(KeyModifiers.Control) && Shortcuts?.Invoke(key, modifiers) == true;
    }

    /// The window's shortcut handler (MainWindow): key and modifiers, true when it was one.
    public Func<int, KeyModifiers, bool>? Shortcuts;

    private bool OnChar(char c)
    {
        // The characters, not the keys: with Shift these are { and } on most layouts.
        switch (c)
        {
            case '[': editor.Send("edit.rotate\t-15"); return true;
            case ']': editor.Send("edit.rotate\t15"); return true;
            case '{': editor.Send("edit.rotate\t-1"); return true;
            case '}': editor.Send("edit.rotate\t1"); return true;
        }
        return c < ' ';  // control characters of Ctrl shortcuts: no beep
    }
}
