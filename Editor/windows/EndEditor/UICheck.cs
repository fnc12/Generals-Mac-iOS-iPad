using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Interop;
using System.Windows.Threading;
using EndEditor.Core;
using EndEditor.Render;

namespace EndEditor;

public sealed record UICheckRequest(string ZeroHour, string Base, string Map, string Template, string SaveName, string GameApp,
                                    string Output);

/// `--ui-check <zh> <base> <map path> <template> <save name> <game exe> <out.txt>`: walks the editor's
/// features through the real window, as a user does: controls through UI Automation (what
/// Narrator uses: Invoke, Toggle, Value, Selection, ExpandCollapse), clicks, drags, the wheel and
/// keys as window messages posted to the map view (the modifiers in the thread's key state, where
/// the view reads them). It never sets model state itself; every check reads what the store and
/// the views show. It runs on a worker thread (a UI Automation client must not block the UI
/// thread it asks). The exit code is the number of failed checks; out.txt lists them.
public static class UICheck
{
    public static int ExitCode;
    /// Exceptions on the UI thread while the check ran (each is a failed check).
    public static readonly System.Collections.Concurrent.ConcurrentBag<string> Failures = [];

    public static UICheckRequest? Parse(string[] args)
    {
        var at = Array.IndexOf(args, "--ui-check");
        if (at < 0) return null;
        var v = args[(at + 1)..];
        if (v.Length < 7)
        {
            Console.Error.WriteLine("usage: --ui-check <zh> <base> <map path> <template> <save name> <game exe> <out.txt>");
            Environment.Exit(2);
        }
        return new UICheckRequest(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }

    public static void Run(UICheckRequest request, MainWindow window)
    {
        var check = new Checker(request, window);
        var thread = new Thread(check.Run) { IsBackground = true, Name = "ui-check" };
        thread.SetApartmentState(ApartmentState.MTA);
        thread.Start();
    }

    private sealed class Checker(UICheckRequest request, MainWindow window)
    {
        private readonly Dispatcher ui = window.Dispatcher;
        private readonly List<string> lines = [];
        private int failed, passed;
        private readonly Stopwatch clock = Stopwatch.StartNew();
        private EditorModel Editor => window.Editor;
        private IntPtr mainHandle;
        private IntPtr mapHandle;

        private void Log(string line)
        {
            var text = $"[{clock.Elapsed.TotalSeconds,7:0.0}] {line}";
            lock (lines) lines.Add(text);
            Console.WriteLine(text);
            File.WriteAllLines(request.Output, lines);
        }

        private bool Check(string what, bool ok)
        {
            if (ok) ++passed;
            else ++failed;
            Log((ok ? "ok   " : "FAIL ") + what);
            return ok;
        }

        private T Ui<T>(Func<T> work) => ui.Invoke(work);
        private void Ui(Action work) => ui.Invoke(work);

        /// Lets the UI thread run what was posted and render.
        private void Settle(int ms = 150)
        {
            Thread.Sleep(ms);
            ui.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        }

        private bool Wait(string what, Func<bool> done, double seconds = 120)
        {
            var start = Stopwatch.StartNew();
            while (!Ui(done))
            {
                if (start.Elapsed.TotalSeconds > seconds) return Check("timeout: " + what, false);
                Thread.Sleep(100);
            }
            return true;
        }

        // MARK: UI Automation

        private AutomationElement? Find(string id, double seconds = 5)
        {
            var condition = new PropertyCondition(AutomationElement.AutomationIdProperty, id);
            var start = Stopwatch.StartNew();
            do
            {
                // The app's windows: the main one, pop-ups, the save sheet (found by their handles: the
                // desktop's element is not this session's in every window station).
                foreach (var handle in TopWindows())
                {
                    AutomationElement top;
                    try
                    {
                        top = AutomationElement.FromHandle(handle);
                    }
                    catch (ElementNotAvailableException)
                    {
                        continue;
                    }
                    if (top.Current.AutomationId == id) return top;
                    if (top.FindFirst(TreeScope.Descendants, condition) is { } inside) return inside;
                }
                Thread.Sleep(100);
            } while (start.Elapsed.TotalSeconds < seconds);
            Log($"     (no {id} in {string.Join(", ", TopWindows().Select(h => { try { var e = AutomationElement.FromHandle(h); return $"'{e.Current.Name}'/{e.Current.ClassName}"; } catch (Exception) { return h.ToString(); } }))})");
            return null;
        }

        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint process);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);

        /// The visible top-level windows of this process, the main window first.
        private List<IntPtr> TopWindows()
        {
            var result = new List<IntPtr> { mainHandle };
            EnumWindows((hwnd, _) =>
            {
                GetWindowThreadProcessId(hwnd, out var process);
                if (process == Environment.ProcessId && hwnd != mainHandle && IsWindowVisible(hwnd)) result.Add(hwnd);
                return true;
            }, IntPtr.Zero);
            return result;
        }

        private AutomationElement? FindByName(AutomationElement root, string name) =>
            root.FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.NameProperty, name));

        /// The menu that holds each menu item (its items are in the tree only while it is open).
        private static readonly Dictionary<string, string> Menus = new()
        {
            ["menu.zeroHourFolder"] = "File", ["menu.baseFolder"] = "File", ["menu.openMap"] = "File", ["menu.save"] = "File",
            ["menu.saveAs"] = "File", ["menu.undo"] = "Edit", ["menu.redo"] = "Edit", ["menu.copy"] = "Edit", ["menu.paste"] = "Edit",
            ["menu.duplicate"] = "Edit", ["menu.selectAll"] = "Edit", ["menu.delete"] = "Edit", ["menu.play"] = "Map", ["menu.stop"] = "Map",
        };

        /// Opens a menu of the menu bar, as a click on it does.
        private void OpenMenu(string name)
        {
            var bar = AutomationElement.FromHandle(mainHandle).FindFirst(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuBar));
            var menu = bar == null ? null : FindByName(bar, name);
            if (menu != null && menu.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand))
            {
                ((ExpandCollapsePattern)expand).Expand();
                Settle();
            }
        }

        private bool Invoke(string id)
        {
            if (Menus.TryGetValue(id, out var menuName)) OpenMenu(menuName);
            if (Find(id) is not { } element) return false;
            if (element.TryGetCurrentPattern(InvokePattern.Pattern, out var invoke)) ((InvokePattern)invoke).Invoke();
            else if (element.TryGetCurrentPattern(TogglePattern.Pattern, out var toggle)) ((TogglePattern)toggle).Toggle();
            else if (element.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var select)) ((SelectionItemPattern)select).Select();
            else return false;
            Settle();
            return true;
        }

        private bool? Toggled(string id) =>
            Find(id) is { } element && element.TryGetCurrentPattern(TogglePattern.Pattern, out var toggle)
                ? ((TogglePattern)toggle).Current.ToggleState == ToggleState.On : null;

        /// Opens a pop-up (combo box) and chooses the item showing `label`.
        private bool Choose(string id, string label)
        {
            if (Find(id) is not { } box || !box.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand)) return false;
            ((ExpandCollapsePattern)expand).Expand();
            Settle();
            var item = FindByName(box, label);
            if (item == null || !item.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var select))
            {
                ((ExpandCollapsePattern)expand).Collapse();
                return false;
            }
            ((SelectionItemPattern)select).Select();
            Settle();
            if (box.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var again)) ((ExpandCollapsePattern)again).Collapse();
            Settle();
            return true;
        }

        /// Types into a text field and presses Enter (the field commits on Enter).
        private bool Fill(string id, string text, bool enter = true)
        {
            if (Find(id) is not { } field || !field.TryGetCurrentPattern(ValuePattern.Pattern, out var value)) return false;
            field.SetFocus();
            ((ValuePattern)value).SetValue(text);
            Settle();
            if (enter) Key(mainHandle, 0x0D);
            Settle();
            return true;
        }

        private string Text(string id) => Find(id, 1) is { } element ? element.Current.Name : "";

        /// Selects the list item showing `name` (scrolled into view first: the lists are virtual).
        private bool SelectItem(string listId, string name)
        {
            if (Find(listId) is not { } list) return false;
            AutomationElement? item = null;
            if (list.TryGetCurrentPattern(ItemContainerPattern.Pattern, out var container))
            {
                item = ((ItemContainerPattern)container).FindItemByProperty(null, AutomationElement.NameProperty, name);
                if (item != null && item.TryGetCurrentPattern(VirtualizedItemPattern.Pattern, out var virtualized))
                    ((VirtualizedItemPattern)virtualized).Realize();
            }
            item ??= FindByName(list, name);
            if (item == null || !item.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var select)) return false;
            ((SelectionItemPattern)select).Select();
            Settle();
            return true;
        }

        // MARK: Input

        [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] private static extern bool GetKeyboardState(byte[] state);
        [DllImport("user32.dll")] private static extern bool SetKeyboardState(byte[] state);
        [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int X, Y;
        }

        private const int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12;

        /// The UI thread's key state: these modifiers held (what GetKeyState reads in the views).
        private void Hold(params int[] keys)
        {
            Ui(() =>
            {
                var state = new byte[256];
                GetKeyboardState(state);
                foreach (var key in new[] { VK_SHIFT, VK_CONTROL, VK_MENU, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 }) state[key] = 0;
                foreach (var key in keys) state[key] = 0x80;
                SetKeyboardState(state);
            });
        }

        private static IntPtr Pixel(Vector2 p) => (IntPtr)(((int)MathF.Round(p.Y) << 16) | ((int)MathF.Round(p.X) & 0xFFFF));

        private void Post(IntPtr hwnd, int msg, int wParam, IntPtr lParam)
        {
            PostMessageW(hwnd, msg, wParam, lParam);
            Settle(40);
        }

        private void Click(Vector2 p, params int[] modifiers)
        {
            Hold(modifiers);
            Post(mapHandle, 0x0200, 0, Pixel(p));  // WM_MOUSEMOVE
            Post(mapHandle, 0x0201, 1, Pixel(p));  // WM_LBUTTONDOWN
            Post(mapHandle, 0x0202, 0, Pixel(p));  // WM_LBUTTONUP
            Hold();
            Settle();
        }

        private void RightClick(Vector2 p)
        {
            Post(mapHandle, 0x0200, 0, Pixel(p));
            Post(mapHandle, 0x0204, 2, Pixel(p));  // WM_RBUTTONDOWN
            Post(mapHandle, 0x0205, 0, Pixel(p));  // WM_RBUTTONUP
            Settle();
        }

        private void Drag(Vector2 from, Vector2 to, params int[] modifiers)
        {
            Hold(modifiers);
            Post(mapHandle, 0x0200, 0, Pixel(from));
            Post(mapHandle, 0x0201, 1, Pixel(from));
            for (int i = 1; i <= 12; ++i) Post(mapHandle, 0x0200, 1, Pixel(Vector2.Lerp(from, to, i / 12f)));
            Post(mapHandle, 0x0202, 0, Pixel(to));
            Hold();
            Settle();
        }

        private void Wheel(Vector2 p, float notches)
        {
            var point = new POINT { X = (int)p.X, Y = (int)p.Y };
            ClientToScreen(mapHandle, ref point);
            Post(mapHandle, 0x020A, (int)(notches * 120) << 16, (IntPtr)((point.Y << 16) | (point.X & 0xFFFF)));
            Settle();
        }

        /// A key press (virtual key) with modifiers held; Ctrl+letter also posts its control character.
        private void Key(IntPtr hwnd, int key, params int[] modifiers)
        {
            Hold(modifiers);
            Post(hwnd, 0x0100, key, (IntPtr)1);  // WM_KEYDOWN
            Post(hwnd, 0x0101, key, unchecked((IntPtr)0xC0000001));  // WM_KEYUP
            Hold();
            Settle();
        }

        private void Char(char c)
        {
            Post(mapHandle, 0x0102, c, (IntPtr)1);  // WM_CHAR
            Settle();
        }

        // MARK: The map view

        private MapRenderer Renderer => window.MapsScreen.View.Renderer;
        private Vector2 ViewSize => Ui(() => window.MapsScreen.View.Size);
        private Vector2? Shown(Vector3 world) => Ui(() => Renderer.Camera.ViewPoint(world, window.MapsScreen.View.Size));
        private Vector3? GroundAt(Vector2 p) => Ui(() => Renderer.Camera.GroundPoint(p, window.MapsScreen.View.Size, Renderer.Scene));
        private MapObjectItem? Object(long id) => Ui(() => Editor.MapScene.Objects.FirstOrDefault(o => o.Id == id));
        private List<long> Selection => Ui(() => Editor.MapSelection.ToList());
        private int ObjectCount => Ui(() => Editor.MapScene.Objects.Count);

        /// A point of the view on the object, where a click hits it first.
        private Vector2? PointOn(long id)
        {
            return Ui(() =>
            {
                if (Renderer.Object(id) is not { } item) return (Vector2?)null;
                var size = window.MapsScreen.View.Size;
                foreach (var z in new[] { 4f, 6f, 2f, 8f, 10f })
                {
                    if (Renderer.Camera.ViewPoint(item.Position + new Vector3(0, 0, z), size) is { } p && p.X > 2 && p.Y > 2 &&
                        p.X < size.X - 2 && p.Y < size.Y - 2 && Renderer.Pick(p, size) == id)
                        return p;
                }
                return null;
            });
        }

        /// A frame of the current view drawn offscreen at the view's size: BGRA rows.
        private (byte[] Pixels, int Width, int Height) Frame() => Ui(() =>
        {
            var size = window.MapsScreen.View.Size;
            int w = (int)size.X, h = (int)size.Y;
            return (Renderer.RenderPixels(w, h, window.MapsScreen.View.Host.Scale), w, h);
        });

        private static double Brightness(byte[] pixels, int width, int x0, int y0, int x1, int y1)
        {
            double sum = 0;
            int count = 0;
            for (int y = y0; y < y1; y += 2)
            for (int x = x0; x < x1; x += 2)
            {
                var i = (y * width + x) * 4;
                sum += (pixels[i] + pixels[i + 1] + pixels[i + 2]) / 3.0;
                ++count;
            }
            return count == 0 ? 0 : sum / count;
        }

        /// The share of a label's pixels that have its color.
        private static double Coverage(byte[] pixels, int width, int height, StartLabel label)
        {
            int r = (int)(label.Color >> 16) & 0xFF, g = (int)(label.Color >> 8) & 0xFF, b = (int)label.Color & 0xFF;
            int hits = 0, count = 0;
            for (var y = (int)Math.Max(label.Frame.Top, 0); y < (int)Math.Min(label.Frame.Bottom, height); ++y)
            for (var x = (int)Math.Max(label.Frame.Left, 0); x < (int)Math.Min(label.Frame.Right, width); ++x)
            {
                var i = (y * width + x) * 4;
                ++count;
                if (Math.Abs(pixels[i + 2] - r) < 24 && Math.Abs(pixels[i + 1] - g) < 24 && Math.Abs(pixels[i] - b) < 24) ++hits;
            }
            return count == 0 ? 0 : hits / (double)count;
        }

        private static int Differing(byte[] a, byte[] b)
        {
            int count = 0;
            for (int i = 0; i < Math.Min(a.Length, b.Length); i += 4)
                if (Math.Abs(a[i] - b[i]) + Math.Abs(a[i + 1] - b[i + 1]) + Math.Abs(a[i + 2] - b[i + 2]) > 30) ++count;
            return count;
        }

        public void Run()
        {
            try
            {
                Walk();
            }
            catch (Exception error)
            {
                Check("the check ran to its end: " + error, false);
            }
            foreach (var failure in Failures) Check("no error on the UI thread: " + failure, false);
            Log($"-- {passed} ok, {failed} FAIL");
            ExitCode = failed;
            Ui(() =>
            {
                if (Editor.Play.State == "running") Editor.StopGame();
                window.Close();
            });
        }

        private void Walk()
        {
            mainHandle = Ui(() => new WindowInteropHelper(window).Handle);
            Log($"END Editor --ui-check on {Environment.MachineName}, GPU {Gpu.Shared.AdapterName}{(Gpu.Shared.Software ? " (WARP)" : "")}");

            // Game data.
            Wait("the game data is read", () => Editor.Status.State is "ready" or "error", 300);
            Check($"the game folder is open: {Ui(() => Editor.Status.Message)}",
                  Ui(() => Editor.Status.State == "ready" && Editor.Status.GameDir == request.ZeroHour && Editor.Objects.Count > 1000));
            Check($"the user Maps folder is the game's: {Ui(() => Editor.UserMapsFolder)}",
                  Ui(() => Editor.UserMapsFolder.EndsWith(@"Command and Conquer Generals Zero Hour Data\Maps", StringComparison.OrdinalIgnoreCase)));
            Check("the toolbar's Maps segment is clicked", Invoke("toolbar.mode.Maps") && Ui(() => window.Mode) == "Maps");

            // Open a map from the list, with a search.
            var mapName = Ui(() => Editor.Maps.FirstOrDefault(m => m.Path == request.Map)?.Name ?? "");
            Check($"the map list has {mapName} ({Ui(() => Editor.Maps.Count)} maps)", mapName.Length > 0);
            Check("the map list's search is typed into", Fill("maps.search", mapName, false));
            Check("the search left that map", Ui(() => window.MapsScreen.List.Items.Count) is > 0 and < 10);
            Check($"the map {mapName} is clicked in the map list", SelectItem("maps.list", mapName));
            Wait("the map is ready", () => Editor.MapState == "ready" && Editor.MapScene.Name.Length > 0, 120);
            Wait("its models are loaded", () => Editor.MapModelCount > 0 && Editor.MapModelsLoaded >= Editor.MapModelCount &&
                                                Renderer.LoadedModelCount >= Editor.MapModels.Count, 300);
            mapHandle = Ui(() => window.MapsScreen.View.Host.Window);
            Settle(500);
            var size = ViewSize;
            Check($"the map view is {size.X}x{size.Y} pixels", size.X > 400 && size.Y > 250);

            // Framed: the playable area's middle in the middle of the view.
            var camera = Ui(() => (Renderer.Camera.Target, Renderer.Camera.Distance, Editor.MapScene.Playable));
            var middle = new Vector2((camera.Playable.X + camera.Playable.Z) / 2, (camera.Playable.Y + camera.Playable.W) / 2);
            Check("the map is framed: the camera looks at the playable area's middle",
                  Vector2.Distance(new Vector2(camera.Target.X, camera.Target.Y), middle) < 1 && camera.Distance > 100);

            // Pixels of the framed view.
            var (pixels, width, height) = Frame();
            var terrain = Brightness(pixels, width, width / 4, height / 4, width * 3 / 4, height * 3 / 4);
            Check($"the terrain is drawn, not black (mean brightness {terrain:0})", terrain > 25);
            Ui(() => window.MapsScreen.View.Renderer.ShowModels = false);
            var withoutModels = Frame().Pixels;
            Ui(() => window.MapsScreen.View.Renderer.ShowModels = true);
            var modelPixels = Differing(pixels, withoutModels);
            Check($"models are drawn ({modelPixels} pixels change without them)", modelPixels > 500);

            // Start labels and colors.
            var starts = Ui(() => Editor.MapScene.Objects.Where(o => o.Start > 0).ToList());
            Check($"the map's start waypoints ({starts.Count}) are named Player_N_Start", starts.Count >= 2 &&
                  starts.All(s => s.TemplateName == "Waypoint" || s.Kind == "waypoint"));
            Check($"each start position has its own color: {string.Join(" ", starts.Select(s => $"#{s.Color:X6}"))}",
                  starts.Select(s => s.Color).Distinct().Count() == starts.Count &&
                  starts.All(w => Ui(() => Editor.MapPlayers.FirstOrDefault(p => p.Start == w.Start)?.Color) == w.Color));
            Ui(() => window.MapsScreen.View.Host.RenderNow(true));
            var labels = Ui(() => Renderer.StartLabels.ToList());
            Check($"the framed view has {labels.Count} start labels, each at its waypoint's projected point",
                  labels.Count == starts.Count && labels.All(l =>
                      starts.FirstOrDefault(s => s.Id == l.Object) is { } s && Shown(s.Position + new Vector3(0, 0, 13)) is { } p &&
                      Vector2.Distance(p, l.Anchor) < 1.5f && l.Color == s.Color && l.Start == s.Start));
            var frame = Frame();
            var coverage = labels.Select(l => Coverage(frame.Pixels, frame.Width, frame.Height, l)).ToList();
            Check($"each label is drawn in its start's color ({string.Join(" ", coverage.Select(c => $"{c * 100:0}%"))} of its pixels)",
                  coverage.Count > 0 && coverage.All(c => c > 0.3));
            Check("Markers off is clicked", Invoke("map.showMarkers") && Toggled("map.showMarkers") == false);
            Ui(() => window.MapsScreen.View.Host.RenderNow(true));
            var hidden = Frame();
            var hiddenCoverage = labels.Select(l => Coverage(hidden.Pixels, hidden.Width, hidden.Height, l)).ToList();
            Check($"Markers off: no start labels ({Ui(() => Renderer.StartLabels.Count)}), their pixels gone " +
                  $"({string.Join(" ", hiddenCoverage.Select(c => $"{c * 100:0}%"))})",
                  Ui(() => Renderer.StartLabels.Count) == 0 && hiddenCoverage.Zip(coverage).All(p => p.First < p.Second * 0.5));
            Check("Markers on again", Invoke("map.showMarkers") && Toggled("map.showMarkers") == true);
            Check("the Water checkbox turns it off and on", Invoke("map.showWater") && !Ui(() => Renderer.ShowWater) &&
                                                             Invoke("map.showWater") && Ui(() => Renderer.ShowWater));
            Check("the Models checkbox turns them off and on", Invoke("map.showModels") && !Ui(() => Renderer.ShowModels) &&
                                                                 Invoke("map.showModels") && Ui(() => Renderer.ShowModels));

            // Camera: pan, zoom at the cursor, turn.
            var center = size / 2;
            Vector2 EmptyGround(Vector2 near)
            {
                foreach (var dx in new[] { 0f, 60, -60, 120, -120, 180, -180 })
                foreach (var dy in new[] { 0f, 50, -50, 100, -100 })
                {
                    var p = near + new Vector2(dx, dy);
                    if (Ui(() => Renderer.Hits(p, window.MapsScreen.View.Size).Count) == 0 && GroundAt(p) != null) return p;
                }
                return near;
            }
            var panStart = EmptyGround(center);
            var before = Ui(() => Renderer.Camera.Target);
            var groundBefore = GroundAt(panStart);
            Drag(panStart, panStart + new Vector2(120, 60));
            var after = Ui(() => Renderer.Camera.Target);
            Check($"a drag on empty ground pans the camera ({Vector3.Distance(before, after):0} units)", Vector3.Distance(before, after) > 20);
            var groundAfter = GroundAt(panStart + new Vector2(120, 60));
            Check("the ground under the cursor followed it",
                  groundBefore is { } gb && groundAfter is { } ga && Vector2.Distance(new Vector2(gb.X, gb.Y), new Vector2(ga.X, ga.Y)) <
                  Ui(() => Renderer.Camera.Distance) * 0.05f);
            Check("the pan selected nothing", Selection.Count == 0);
            var zoomAt = new Vector2(size.X * 0.3f, size.Y * 0.55f);
            var underCursor = GroundAt(zoomAt);
            var distance = Ui(() => Renderer.Camera.Distance);
            Wheel(zoomAt, 2);
            var zoomed = Ui(() => Renderer.Camera.Distance);
            var stillUnder = GroundAt(zoomAt);
            Check($"the wheel zooms in ({distance:0} -> {zoomed:0})", zoomed < distance * 0.9f);
            Check($"and keeps the ground under the cursor ({(underCursor is { } u && stillUnder is { } s2 ? Vector3.Distance(u, s2) : -1):0.0} units)",
                  underCursor is { } u1 && stillUnder is { } s1 && Vector2.Distance(new Vector2(u1.X, u1.Y), new Vector2(s1.X, s1.Y)) < 3);
            var labelAnchors = Ui(() => Renderer.StartLabels.Select(l => l.Anchor).ToList());
            Wheel(zoomAt, -2);
            Check("the wheel zooms out again", Math.Abs(Ui(() => Renderer.Camera.Distance) - distance) < distance * 0.05f);
            Ui(() => window.MapsScreen.View.Host.RenderNow(true));
            Check("the start labels moved with the zoom", !Ui(() => Renderer.StartLabels.Select(l => l.Anchor).ToList()).SequenceEqual(labelAnchors));
            var yaw = Ui(() => Renderer.Camera.Yaw);
            var orbitStart = EmptyGround(center);
            Drag(orbitStart, orbitStart + new Vector2(80, 0), VK_MENU);
            Check($"Alt-drag turns the camera (yaw {yaw:0.00} -> {Ui(() => Renderer.Camera.Yaw):0.00})", Math.Abs(Ui(() => Renderer.Camera.Yaw) - yaw) > 0.3f);
            Drag(orbitStart + new Vector2(80, 0), orbitStart, VK_MENU);
            Check("and back", Math.Abs(Ui(() => Renderer.Camera.Yaw) - yaw) < 0.05f);

            // Placing through the picker.
            Check("no object is chosen at start", Ui(() => Editor.MapEditor.TemplateName).Length == 0);
            var placeAt = EmptyGround(center + new Vector2(-40, 30));
            var clickedGround = GroundAt(placeAt);
            var count0 = ObjectCount;
            RightClick(placeAt);
            Check("a right click with no object chosen: the store asks for the picker and holds the right click",
                  Ui(() => Editor.MapEditor.Picker && Editor.MapEditor.Pending != null));
            Check($"the store's note says what to do: {Ui(() => Editor.MapEditor.Note)}", Ui(() => Editor.MapEditor.Note).Length > 0);
            Check("the picker shows", Find("picker.search") != null && Ui(() => window.MapsScreen.Bar.Picker.IsOpen));
            Check($"the picker's search is typed into", Fill("picker.search", request.Template, false));
            Check($"the picker's row {request.Template} is clicked", SelectItem("picker.list", request.Template));
            var placed = Ui(() => Editor.MapScene.Objects.LastOrDefault());
            Check($"the click on the row placed {request.Template}", ObjectCount == count0 + 1 && placed?.TemplateName == request.Template);
            if (placed == null || clickedGround == null) throw new InvalidOperationException("nothing placed");
            Check("at the right-clicked ground point", Vector2.Distance(new Vector2(placed.Position.X, placed.Position.Y),
                                                                         new Vector2(clickedGround.Value.X, clickedGround.Value.Y)) < 1);
            Check("at the terrain height", Math.Abs(placed.Position.Z - Ui(() => Editor.MapScene.GroundHeight(placed.Position.X, placed.Position.Y))) < 0.5f);
            Check("selected, picker closed", Selection.SequenceEqual([placed.Id]) && !Ui(() => Editor.MapEditor.Picker) &&
                                              !Ui(() => window.MapsScreen.Bar.Picker.IsOpen));
            Check("the inspector shows it", Ui(() => window.MapsScreen.Inspector.Children.Count) > 5 &&
                                             Text("inspector.title") == request.Template);
            var players = Ui(() => Editor.MapPlayers.ToList());
            if (players.FirstOrDefault(p => p.Team != Ui(() => Editor.MapEditor.Team)) is { } other)
                Check($"the placement bar's Owner pop-up chose {other.Label}", Choose("placement.owner", other.Label) &&
                                                                               Ui(() => Editor.MapEditor.Team) == other.Team);
            var secondAt = EmptyGround(placeAt + new Vector2(90, 0));
            RightClick(secondAt);
            var second = Ui(() => Editor.MapScene.Objects.LastOrDefault());
            Check("a second right click placed another one", ObjectCount == count0 + 2 && second?.Id != placed.Id);
            if (second == null) throw new InvalidOperationException("no second object");
            Check("owned by the chosen team", second.Owner == Ui(() => Editor.MapEditor.Team));
            Check("only it is selected", Selection.SequenceEqual([second.Id]));
            Check("the bar's angle + is clicked", Invoke("placement.angle.up") && Math.Abs(Ui(() => Editor.MapEditor.Angle) - 15) < 0.01);
            Check("and − again", Invoke("placement.angle.down") && Math.Abs(Ui(() => Editor.MapEditor.Angle)) < 0.01);

            // Select, Shift-click, move.
            if (PointOn(placed.Id) is not { } firstShown) throw new InvalidOperationException("the first object is not in view");
            Click(firstShown, VK_SHIFT);
            Check("Shift-click selected both", Selection.ToHashSet().SetEquals([placed.Id, second.Id]));
            var cameraBefore = Ui(() => Renderer.Camera.Target);
            var p1 = PointOn(placed.Id) ?? firstShown;
            Drag(p1, p1 + new Vector2(70, 25));
            var moved1 = Object(placed.Id)!;
            var moved2 = Object(second.Id)!;
            var d1 = new Vector2(moved1.Position.X - placed.Position.X, moved1.Position.Y - placed.Position.Y);
            var d2 = new Vector2(moved2.Position.X - second.Position.X, moved2.Position.Y - second.Position.Y);
            Check($"the drag moved both by the same offset ({d1.X:0.0}, {d1.Y:0.0})", d1.Length() > 10 && Vector2.Distance(d1, d2) < 0.01f);
            Check("on the ground there", Math.Abs(moved1.Position.Z - Ui(() => Editor.MapScene.GroundHeight(moved1.Position.X, moved1.Position.Y))) < 0.5f);
            Check("the camera did not pan", Ui(() => Renderer.Camera.Target) == cameraBefore);
            Check($"the move is one undo step ({Ui(() => Editor.MapEdit.UndoCount)})", Ui(() => Editor.MapEdit.UndoCount) == 3);

            // Turn with [ ] { }.
            Char(']');
            var turned = Object(placed.Id)!;
            Check("] turned the selection by 15°", Math.Abs(turned.Angle - moved1.Angle - MathF.PI / 12) < 0.001f);
            Char('}');
            Check("} turned it by 1° more", Math.Abs(Object(placed.Id)!.Angle - turned.Angle - MathF.PI / 180) < 0.001f);
            Char('{');
            Char('[');
            Check("{ and [ turned it back", Math.Abs(Object(placed.Id)!.Angle - moved1.Angle) < 0.001f);

            // Copy, paste at the cursor, duplicate, select all, a click on empty ground, Delete, undo, redo.
            Key(mapHandle, 'C', VK_CONTROL);
            Check("Ctrl+C copied 2 objects", Ui(() => Editor.MapEditor.ClipboardCount) == 2);
            var pasteAt = EmptyGround(center + new Vector2(150, -60));
            Post(mapHandle, 0x0200, 0, Pixel(pasteAt));
            var cursor = Ui(() => Editor.CursorGround);
            Check("the cursor's ground point is known", cursor != null);
            var countBeforePaste = ObjectCount;
            Key(mapHandle, 'V', VK_CONTROL);
            var pasted = Selection;
            Check($"Ctrl+V pasted 2 objects, selected", ObjectCount == countBeforePaste + 2 && pasted.Count == 2 && !pasted.Contains(placed.Id));
            var pastedObjects = pasted.Select(Object).OfType<MapObjectItem>().ToList();
            if (pastedObjects.Count == 2 && cursor is { } c)
            {
                var layout = new Vector2(moved2.Position.X - moved1.Position.X, moved2.Position.Y - moved1.Position.Y);
                var copied = new Vector2(pastedObjects[1].Position.X - pastedObjects[0].Position.X, pastedObjects[1].Position.Y - pastedObjects[0].Position.Y);
                Check("with their layout kept", Vector2.Distance(copied, layout) < 0.01f || Vector2.Distance(copied, -layout) < 0.01f);
                var mid = (new Vector2(pastedObjects[0].Position.X, pastedObjects[0].Position.Y) + new Vector2(pastedObjects[1].Position.X, pastedObjects[1].Position.Y)) / 2;
                Check("around the cursor", Vector2.Distance(mid, c) < 1);
            }
            Key(mapHandle, 'D', VK_CONTROL);
            var duplicates = Selection;
            Check("Ctrl+D duplicated them", ObjectCount == countBeforePaste + 4 && duplicates.Count == 2 && !duplicates.Intersect(pasted).Any());
            Key(mapHandle, 'A', VK_CONTROL);
            Check($"Ctrl+A selected all {ObjectCount} objects", Selection.Count == ObjectCount);
            Click(EmptyGround(center + new Vector2(-150, -80)));
            Check("a click on empty ground cleared the selection", Selection.Count == 0);
            var bandPoints = duplicates.Select(id => Ui(() => Renderer.Object(id) is { } o ? Renderer.Camera.ViewPoint(o.Position + new Vector3(0, 0, 5), window.MapsScreen.View.Size) : null)).ToList();
            if (bandPoints.Count == 2 && bandPoints.All(p => p != null))
            {
                var lo = Vector2.Min(bandPoints[0]!.Value, bandPoints[1]!.Value) - new Vector2(25);
                var hi = Vector2.Max(bandPoints[0]!.Value, bandPoints[1]!.Value) + new Vector2(25);
                Drag(lo, hi, VK_SHIFT);
                Check("the Shift-drag band selected the duplicates", duplicates.All(Selection.Contains));
            }
            else
            {
                Check("the duplicates are in view", false);
            }
            var selectedNow = Selection;
            Key(mapHandle, 0x2E);
            Check("Delete removed the selection", selectedNow.Count > 0 && selectedNow.All(id => Object(id) == null));
            Key(mapHandle, 'Z', VK_CONTROL);
            Check("Ctrl+Z brought it back", selectedNow.All(id => Object(id) != null));
            Key(mapHandle, 'Y', VK_CONTROL);
            Check("Ctrl+Y removed it again", selectedNow.All(id => Object(id) == null));
            Check("Edit > Undo is chosen", Invoke("menu.undo"));
            Check("it brought the selection back", selectedNow.All(id => Object(id) != null));

            // The inspector.
            Click(PointOn(placed.Id) ?? firstShown);
            Check("a click selected only the first object", Selection.SequenceEqual([placed.Id]));
            Check("height typed into the inspector", Fill("inspector.height", "25"));
            Check("the object is 25 above the ground", Math.Abs(Object(placed.Id)!.Height - 25) < 0.01f);
            Check("angle typed into the inspector", Fill("inspector.angle", "30"));
            Check("the object is turned to 30°", Math.Abs(Object(placed.Id)!.Angle - MathF.PI / 6) < 0.001f);
            Check("the inspector's angle + is clicked", Invoke("inspector.angleUp"));
            Check("it turned the object by 15°", Math.Abs(Object(placed.Id)!.Angle - MathF.PI / 4) < 0.001f);
            MapObjectProperty? Property(string key) => Ui(() => Editor.MapObjectProperties(placed.Id).FirstOrDefault(p => p.Key == key));
            Check("name typed into the inspector", Fill("inspector.name", "UICheck"));
            Check("its objectName is UICheck", Property("objectName")?.Value == "UICheck");
            Check("script typed into the inspector", Fill("inspector.script", "UICheckScript"));
            Check("its objectScriptAttachment is UICheckScript", Property("objectScriptAttachment")?.Value == "UICheckScript");
            Check("health typed into the inspector", Fill("inspector.health", "40"));
            Check("its objectInitialHealth is 40", Property("objectInitialHealth") is { Type: "int", Value: "40" });
            var indestructible = Property("objectIndestructible")?.Value == "true";
            Check("the Indestructible checkbox is clicked", Invoke("inspector.flag.objectIndestructible"));
            Check($"objectIndestructible is {!indestructible}", (Property("objectIndestructible")?.Value == "true") == !indestructible);
            var startOwner = players.FirstOrDefault(p => p.Start == 1);
            Check($"the Owner pop-up lists the start positions: {string.Join(", ", players.Where(p => p.Start > 0).Select(p => p.Label))}",
                  players.Count(p => p.Start > 0) == starts.Count);
            if (startOwner != null)
            {
                Check("the inspector's Owner pop-up chose Start 1", Choose("inspector.owner", "Start 1") && Object(placed.Id)!.Owner == "teamStart1");
                var startWaypoint = starts.FirstOrDefault(s => s.Start == 1);
                Check($"Start 1's object is drawn in Start 1's color #{Object(placed.Id)!.Color:X6} (its waypoint's #{startWaypoint?.Color ?? 0:X6})",
                      Object(placed.Id)!.Color == startOwner.Color && startWaypoint?.Color == startOwner.Color);
            }
            Check("a new property's key typed", Fill("inspector.newKey", "uiCheckKey", false));
            Check("its type chosen in the pop-up", Choose("inspector.newType", "int"));
            Check("its value typed", Fill("inspector.newValue", "7", false));
            Check("Add is clicked", Invoke("inspector.add"));
            Check("the property uiCheckKey is an int 7", Property("uiCheckKey") is { Type: "int", Value: "7" });
            Check("its value field is typed into", Fill("inspector.property.uiCheckKey", "8"));
            Check("uiCheckKey is 8", Property("uiCheckKey")?.Value == "8");
            Check("its remove button is clicked", Invoke("inspector.remove.uiCheckKey"));
            Check("uiCheckKey is removed", Property("uiCheckKey") == null);
            var undoBefore = Ui(() => Editor.MapEdit.UndoCount);
            Check("the bar's Undo is clicked", Invoke("placement.undo"));
            Check("it brought uiCheckKey back", Property("uiCheckKey")?.Value == "8" && Ui(() => Editor.MapEdit.UndoCount) == undoBefore - 1);
            Check("the bar's Redo is clicked", Invoke("placement.redo"));
            Check("it removed it again", Property("uiCheckKey") == null);

            // Waypoints and a road.
            Check("the Waypoint button chose the waypoint tool", Invoke("placement.tool.waypoint") && Ui(() => Editor.MapEditor.Tool) == "waypoint");
            Click(EmptyGround(center + new Vector2(-150, -80)));
            var linksBefore = Ui(() => Editor.MapScene.Links.Count(l => l.Kind == "waypoint"));
            var countBeforeWaypoints = ObjectCount;
            foreach (var offset in new[] { new Vector2(-120, 80), new Vector2(-60, 100), new Vector2(0, 80) })
                RightClick(EmptyGround(center + offset));
            Check("three right clicks placed three waypoints", ObjectCount == countBeforeWaypoints + 3 &&
                                                               Ui(() => Editor.MapScene.Objects.TakeLast(3).All(o => o.Kind == "waypoint")));
            Check("linked into a path", Ui(() => Editor.MapScene.Links.Count(l => l.Kind == "waypoint")) == linksBefore + 2);
            Check("the Road button chose the road tool", Invoke("placement.tool.road") && Ui(() => Editor.MapEditor.Tool) == "road" &&
                                                         Ui(() => Editor.MapEditor.RoadType).Length > 0);
            RightClick(EmptyGround(center + new Vector2(-100, -100)));
            var started = Ui(() => Editor.MapEditor.RoadStart != null);
            Key(mapHandle, 0x1B);
            Check("Esc cancelled a started road", started && Ui(() => Editor.MapEditor.RoadStart == null));
            var countBeforeRoad = ObjectCount;
            RightClick(EmptyGround(center + new Vector2(-100, -100)));
            RightClick(EmptyGround(center + new Vector2(60, -100)));
            Check("the second right click placed the road segment", ObjectCount == countBeforeRoad + 2 &&
                                                                    Ui(() => Editor.MapScene.Links.Any(l => l.Kind == "road")));
            Check("the Bridge button chose the bridge tool", Invoke("placement.tool.bridge") && Ui(() => Editor.MapEditor.Tool) == "bridge");
            Check("the Object button chose the object tool again", Invoke("placement.tool.object") && Ui(() => Editor.MapEditor.Tool) == "object");

            // Save Map As.
            var maps = Ui(() => Editor.UserMapsFolder);
            var target = Path.Combine(maps, request.SaveName, request.SaveName + ".map");
            if (File.Exists(target)) File.Delete(target);
            Check("the bar's Save As… is clicked", Invoke("placement.saveAs"));
            Check("the Save Map As sheet shows", Find("save.name") != null);
            Check("its Cancel is clicked", Invoke("save.cancel") && Find("save.name", 1) == null && Ui(() => window.SaveSheet) == null);
            Check("File > Save Map As… is chosen", Invoke("menu.saveAs") && Find("save.name") != null);
            Check("the name is typed", Fill("save.name", request.SaveName, false));
            Check("Save is clicked", Invoke("save.save"));
            Wait("the sheet closes", () => window.SaveSheet == null, 20);
            Check($"saved as {target}", File.Exists(target) && string.Equals(Ui(() => Editor.MapEdit.SavedPath), target, StringComparison.OrdinalIgnoreCase) &&
                                        !Ui(() => Editor.MapEdit.Edited));
            Check("with its 128×128 preview", File.Exists(Path.ChangeExtension(target, ".tga")) &&
                                              new FileInfo(Path.ChangeExtension(target, ".tga")).Length == 18 + 128 * 128 * 4 + 26);
            Check("it is listed in the map list", Wait("the saved map is listed", () => Editor.Maps.Any(m => m.Name == request.SaveName), 10));
            var otherName = request.SaveName + "Other";
            var otherFile = Path.Combine(maps, otherName, otherName + ".map");
            Directory.CreateDirectory(Path.GetDirectoryName(otherFile)!);
            File.WriteAllText(otherFile, "someone else's map");
            Click(PointOn(placed.Id) ?? firstShown);
            Key(mapHandle, 0x2E);
            Check("Ctrl+Shift+S opened the sheet", PostShortcut('S', VK_CONTROL, VK_SHIFT) && Find("save.name") != null);
            Fill("save.name", otherName, false);
            Invoke("save.save");
            Check("the sheet asks before it replaces another map", Find("save.conflict") is { } conflict && conflict.Current.Name.Contains("Replace") &&
                                                                   File.ReadAllText(otherFile) == "someone else's map");
            Check("Replace is clicked", Text("save.save") == "Replace" && Invoke("save.save"));
            Wait("the sheet closes", () => window.SaveSheet == null, 20);
            Check("Replace saved over it", new FileInfo(otherFile).Length > 1000);
            foreach (var folder in new[] { Path.GetDirectoryName(otherFile)!, Path.GetDirectoryName(target)! })
                if (Directory.Exists(folder)) Directory.Delete(folder, true);

            // Play.
            Check("the toolbar's Play Settings is clicked", Invoke("toolbar.playSettings") && Ui(() => window.PlaySettings.IsOpen));
            Check("Faction: FactionChina chosen", Choose("play.faction", "FactionChina") && Ui(() => Editor.Play.Faction) == "FactionChina");
            Check("Color: Blue chosen", Choose("play.color", "Blue") && Ui(() => Editor.Play.Color) == "ColorBlue");
            Check("Start position: 2 chosen", Choose("play.start", "2") && Ui(() => Editor.Play.StartPosition) == 2);
            Check("AI opponents: 1 chosen", Choose("play.ai", "1") && Ui(() => Editor.Play.AiCount) == 1);
            Check("the difficulty's Hard is clicked", Invoke("play.difficulty.hard") && Ui(() => Editor.Play.AiDifficulty) == "hard");
            Check("the difficulty's Easy is clicked", Invoke("play.difficulty.easy") && Ui(() => Editor.Play.AiDifficulty) == "easy");
            Check("the game app typed", Fill("play.app", request.GameApp) && Ui(() => Editor.Play.App) == request.GameApp);
            Check($"it starts {Ui(() => Editor.Play.AppResolved)}", Ui(() => Editor.Play.AppResolved) == request.GameApp);
            Ui(() => window.PlaySettings.IsOpen = false);
            Settle();
            // An edit the map was not saved with: Play takes it to the test copy.
            var countBeforePlay = ObjectCount;
            RightClick(EmptyGround(center + new Vector2(30, 90)));
            Check("an object placed after the save: the map is edited", ObjectCount == countBeforePlay + 1 && Ui(() => Editor.MapEdit.Edited));
            var runsBefore = Ui(() => Editor.Play.Runs);
            Check("the toolbar's Play is clicked", Invoke("toolbar.play"));
            var play = Ui(() => Editor.Play);
            Check($"the game runs (pid {play.Pid}): {play.Message}", play.State == "running" && play.Pid > 0 && play.Runs == runsBefore + 1 &&
                                                                     processRuns(play.Pid));
            var testCopy = Path.Combine(maps, "__EditorTest", "__EditorTest.map");
            Check("with -testMap on the test copy and the settings",
                  play.Command == string.Join('\t', request.GameApp, "-testMap", testCopy, "-testMapFaction", "FactionChina", "-testMapAI", "1",
                                              "-testMapAIDifficulty", "easy", "-testMapStart", "2", "-testMapColor", "2"));
            Check("the test copy holds the unsaved edits, the map stays edited", File.Exists(testCopy) && Ui(() => Editor.MapEdit.Edited));
            // The real game prints a TEST_MAP line; the core's stand-in game (tests/fake_game.cpp, for a
            // machine without a Windows build of the game) writes the arguments it got to args.txt.
            var argsFile = Path.Combine(Path.GetDirectoryName(request.GameApp)!, "args.txt");
            if (File.Exists(Path.Combine(Path.GetDirectoryName(request.GameApp)!, "fake.cfg")))
            {
                Wait("the stand-in game writes its arguments", () => File.Exists(argsFile) && File.GetLastWriteTime(argsFile) > DateTime.Now.AddMinutes(-1), 20);
                var got = File.Exists(argsFile) ? File.ReadAllLines(argsFile) : [];
                Check($"the game (the core's stand-in) got -testMap on the test copy: {string.Join(" ", got)}",
                      got.Length >= 2 && got[0] == "-testMap" && string.Equals(got[1], testCopy, StringComparison.OrdinalIgnoreCase));
            }
            else
            {
                string? startLine = null;
                Wait("the game prints its TEST_MAP line", () =>
                {
                    startLine = ReadShared(play.LogPath).Split('\n').FirstOrDefault(l => l.Contains("TEST_MAP start"));
                    return startLine != null || Editor.Play.State != "running";
                }, 90);
                Check($"the game started the test copy: {startLine?.Trim() ?? "no TEST_MAP line; " + Ui(() => Editor.Play.Message)}",
                      startLine != null && startLine.Contains("__editortest", StringComparison.OrdinalIgnoreCase));
            }
            var firstPid = play.Pid;
            Check("Ctrl+R started it again", PostShortcut('R', VK_CONTROL) && Wait("the second game", () => Editor.Play.Runs == runsBefore + 2, 20) &&
                                             Ui(() => Editor.Play.State) == "running" && Ui(() => Editor.Play.Pid) != firstPid);
            Check("the first game is closed", !processRuns(firstPid));
            var secondPid = Ui(() => Editor.Play.Pid);
            Settle(3000);
            Check("Map > Stop Game is chosen", Invoke("menu.stop"));
            Wait("the game ends", () => Editor.Play.State != "running", 15);
            Check($"the game ended ({Ui(() => Editor.Play.Message)})", Ui(() => Editor.Play.State) == "exited" && !processRuns(secondPid));
            Check("no game process is left", Process.GetProcessesByName(Path.GetFileNameWithoutExtension(request.GameApp)).Length == 0);

            // Objects mode and the model preview.
            Check("the toolbar's Objects segment is clicked", Invoke("toolbar.mode.Objects") && Ui(() => window.Mode) == "Objects");
            Check("All objects is listed with every object", Ui(() => window.Objects.Sides.Items.Count) > 3);
            Check("the object table's search is typed into", Fill("objects.search", request.Template, false));
            Check($"the search left {Ui(() => window.Objects.Table.Items.Count)} rows", Ui(() => window.Objects.Table.Items.Count) is > 0 and < 50);
            Ui(() => window.Objects.Table.SelectedItem = window.Objects.Table.Items.Cast<ObjectItem>().FirstOrDefault(o => o.Name == request.Template));
            Check($"its row {request.Template} is selected", SelectRow(request.Template));
            Wait("the preview loads", () => Editor.ModelState is "ready" or "error" && window.Objects.Preview.Renderer.PartCount > 0, 60);
            Check($"the preview shows its model ({Ui(() => Editor.Preview.Parts.Count)} parts, {Ui(() => Editor.Animations.Count)} animations)",
                  Ui(() => Editor.Preview.Parts.Count) > 0);
            Check($"the details show its source {Text("objects.source")}", Ui(() => Editor.Objects.First(o => o.Name == request.Template).SourceFile).Length > 0);
            var previewPixels = Ui(() => window.Objects.Preview.Renderer.RenderPixels(400, 300));
            var background = Brightness(previewPixels, 400, 0, 0, 40, 40);
            var modelBrightness = Brightness(previewPixels, 400, 150, 100, 250, 200);
            Check($"the model is drawn in the preview (middle {modelBrightness:0} vs corner {background:0})", Math.Abs(modelBrightness - background) > 8);
            var animation = Ui(() => Editor.Animations.FirstOrDefault());
            if (animation != null)
            {
                Check($"the animation pop-up chose {animation}", Choose("preview.animation", animation));
                Wait("the animation loads", () => Editor.Preview.Animation == animation, 20);
                Check($"the preview plays it ({Ui(() => Editor.Preview.FrameCount)} frames)", Ui(() => Editor.Preview.FrameCount) > 1);
                var playing = Ui(() => window.Objects.Preview.Renderer.Playback.Playing);
                Check("play/pause is clicked", Invoke("preview.playPause") && Ui(() => window.Objects.Preview.Renderer.Playback.Playing) == !playing);
                Invoke("preview.playPause");
            }
            var hiddenParts = Ui(() => window.Objects.Preview.Renderer.Playback.ShowHidden);
            Check("Hidden parts is clicked", Invoke("preview.hiddenParts") && Ui(() => window.Objects.Preview.Renderer.Playback.ShowHidden) == !hiddenParts);
            Check("the game folder is still the one opened", Ui(() => Editor.Status.GameDir) == request.ZeroHour);
            Ui(() => window.SetMode("Maps"));
        }

        private bool SelectRow(string name)
        {
            Settle(300);
            return Ui(() => window.Objects.Selected?.Name) == name;
        }

        /// A Ctrl shortcut typed into the window (the focus is on the map view or a control of it).
        private bool PostShortcut(char key, params int[] modifiers)
        {
            Ui(() => window.Activate());
            Key(mapHandle, key, modifiers);
            Settle(300);
            return true;
        }

        private static bool processRuns(long pid)
        {
            try
            {
                using var process = Process.GetProcessById((int)pid);
                return !process.HasExited;
            }
            catch (ArgumentException)
            {
                return false;
            }
        }

        private static string ReadShared(string path)
        {
            if (path.Length == 0) return "";
            try
            {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                return new StreamReader(stream).ReadToEnd();
            }
            catch (IOException)
            {
                return "";
            }
        }
    }
}
