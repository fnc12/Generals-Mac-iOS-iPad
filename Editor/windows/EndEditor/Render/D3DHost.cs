using System.Numerics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace EndEditor.Render;

/// Draws into a view: told the view's size in pixels and its pixels per WPF unit.
public interface IViewRenderer
{
    /// Whether the frame would look different from the last one presented at this size.
    bool NeedsFrame(Vector2 size);
    void Render(Target target, Vector2 size, float scale);
}

[Flags]
public enum KeyModifiers
{
    None = 0,
    Shift = 1,
    Control = 2,
    Alt = 4,
}

/// A Win32 child window in the WPF tree with a Direct3D 11 swap chain (flip model). Input reaches it
/// as window messages, in pixels of the view with the origin top left; it is turned into events.
/// It draws on WPF's render tick, only when the renderer says the frame changed.
public sealed class D3DHost : HwndHost
{
    private IntPtr window;
    private IDXGISwapChain1? swapChain;
    private Target? target;
    private int width, height;
    public IViewRenderer? Renderer;
    private bool capturing;

    public event Action<MouseButton, Vector2, KeyModifiers>? MouseDownAt;
    public event Action<Vector2, KeyModifiers, bool>? MouseMoveAt;  // bool: the left button is down
    public event Action<MouseButton, Vector2, KeyModifiers>? MouseUpAt;
    public event Action<float, Vector2, KeyModifiers>? Wheel;  // notches (120 = 1)
    /// A key (virtual key code) and the modifiers held; true when handled.
    public event Func<int, KeyModifiers, bool>? KeyPressed;
    public event Func<char, bool>? CharTyped;

    public IntPtr Window => window;
    public Vector2 PixelSize => new(width, height);
    public float Scale => window == IntPtr.Zero ? 1 : GetDpiForWindow(window) / 96f;
    /// The last frame drawn (null before the first).
    public Target? CurrentTarget => target;

    protected override HandleRef BuildWindowCore(HandleRef parent)
    {
        RegisterClass();
        window = CreateWindowExW(0, ClassName, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                 parent.Handle, IntPtr.Zero, GetModuleHandleW(null), IntPtr.Zero);
        CompositionTarget.Rendering += OnRendering;
        return new HandleRef(this, window);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        CompositionTarget.Rendering -= OnRendering;
        target?.Dispose();
        swapChain?.Dispose();
        DestroyWindow(hwnd.Handle);
        window = IntPtr.Zero;
    }

    private void Resize(int newWidth, int newHeight)
    {
        newWidth = Math.Max(newWidth, 1);
        newHeight = Math.Max(newHeight, 1);
        if (newWidth == width && newHeight == height && target != null) return;
        width = newWidth;
        height = newHeight;
        target?.Dispose();
        target = null;
        var gpu = Gpu.Shared;
        gpu.Context.OMSetRenderTargets((ID3D11RenderTargetView)null!);
        if (swapChain == null && !offscreen)
        {
            swapChain = CreateSwapChain(gpu);
            if (swapChain == null) offscreen = true;
            else gpu.Factory.MakeWindowAssociation(window, WindowAssociationFlags.IgnoreAll);
        }
        else if (swapChain != null)
        {
            swapChain.ResizeBuffers(0, (uint)width, (uint)height, Format.Unknown, SwapChainFlags.None);
        }
        target = swapChain != null ? new Target(swapChain.GetBuffer<ID3D11Texture2D>(0), width, height) : Target.Offscreen(width, height);
        needsPresent = true;
    }

    private bool needsPresent;
    /// No swap chain could be made (a session without a desktop compositor, e.g. over ssh): the view
    /// draws into an offscreen texture, so its frames can still be read.
    private bool offscreen;

    /// The flip model (the compositor's), else the legacy blt model; null when neither is available.
    private IDXGISwapChain1? CreateSwapChain(Gpu gpu)
    {
        foreach (var (effect, buffers) in new[] { (SwapEffect.FlipDiscard, 2u), (SwapEffect.Discard, 1u) })
        {
            try
            {
                return gpu.Factory.CreateSwapChainForHwnd(gpu.Device, window, new SwapChainDescription1
                {
                    Width = (uint)width, Height = (uint)height, Format = Format.B8G8R8A8_UNorm, BufferCount = buffers,
                    BufferUsage = Usage.RenderTargetOutput, SampleDescription = new SampleDescription(1, 0),
                    Scaling = Scaling.Stretch, SwapEffect = effect, AlphaMode = AlphaMode.Ignore,
                });
            }
            catch (SharpGen.Runtime.SharpGenException)
            {
                // Not in this session: the next model.
            }
        }
        return null;
    }

    /// Draws now if the frame changed (or always when `force`).
    public void RenderNow(bool force = false)
    {
        if (target == null || Renderer == null || !IsVisible) return;
        var size = new Vector2(width, height);
        if (!force && !needsPresent && !Renderer.NeedsFrame(size)) return;
        needsPresent = false;
        Renderer.Render(target, size, Scale);
        swapChain?.Present(1, PresentFlags.None);
    }

    private void OnRendering(object? sender, EventArgs e) => RenderNow();

    private static KeyModifiers Modifiers()
    {
        var result = KeyModifiers.None;
        if (GetKeyState(0x10) < 0) result |= KeyModifiers.Shift;
        if (GetKeyState(0x11) < 0) result |= KeyModifiers.Control;
        if (GetKeyState(0x12) < 0) result |= KeyModifiers.Alt;
        return result;
    }

    private static Vector2 Point(IntPtr lParam) =>
        new((short)((long)lParam & 0xFFFF), (short)(((long)lParam >> 16) & 0xFFFF));

    protected override IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        switch (msg)
        {
            case WM_SIZE:
                Resize((int)((long)lParam & 0xFFFF), (int)(((long)lParam >> 16) & 0xFFFF));
                RenderNow(true);
                break;
            case WM_ERASEBKGND:
                handled = true;
                return 1;
            case WM_PAINT:
                ValidateRect(hwnd, IntPtr.Zero);
                needsPresent = true;
                handled = true;
                return IntPtr.Zero;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(hwnd);
                SetCapture(hwnd);
                capturing = true;
                MouseDownAt?.Invoke(msg == WM_LBUTTONDOWN ? MouseButton.Left : msg == WM_RBUTTONDOWN ? MouseButton.Right : MouseButton.Middle,
                                    Point(lParam), Modifiers());
                handled = true;
                return IntPtr.Zero;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                if (capturing && ((long)wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
                {
                    capturing = false;
                    ReleaseCapture();
                }
                MouseUpAt?.Invoke(msg == WM_LBUTTONUP ? MouseButton.Left : msg == WM_RBUTTONUP ? MouseButton.Right : MouseButton.Middle,
                                  Point(lParam), Modifiers());
                handled = true;
                return IntPtr.Zero;
            case WM_MOUSEMOVE:
                MouseMoveAt?.Invoke(Point(lParam), Modifiers(), ((long)wParam & MK_LBUTTON) != 0);
                handled = true;
                return IntPtr.Zero;
            case WM_MOUSEWHEEL:
            {
                var screen = Point(lParam);
                var point = new POINT { X = (int)screen.X, Y = (int)screen.Y };
                ScreenToClient(hwnd, ref point);
                Wheel?.Invoke((short)(((long)wParam >> 16) & 0xFFFF) / 120f, new Vector2(point.X, point.Y), Modifiers());
                handled = true;
                return IntPtr.Zero;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (KeyPressed?.Invoke((int)wParam, Modifiers()) == true)
                {
                    handled = true;
                    return IntPtr.Zero;
                }
                break;
            case WM_CHAR:
                if (CharTyped?.Invoke((char)wParam) == true)
                {
                    handled = true;
                    return IntPtr.Zero;
                }
                break;
            case WM_GETDLGCODE:
                // Every key comes here (arrows, Tab, Esc, Delete), not to dialog navigation.
                handled = true;
                return DLGC_WANTALLKEYS;
        }
        return base.WndProc(hwnd, msg, wParam, lParam, ref handled);
    }

    /// Keyboard focus into the view (as a click does).
    public void FocusView()
    {
        if (window != IntPtr.Zero) SetFocus(window);
    }

    // MARK: Win32

    private const string ClassName = "EndEditorD3DView";
    private static bool registered;

    private static void RegisterClass()
    {
        if (registered) return;
        var user32 = GetModuleHandleW("user32.dll");
        var wc = new WNDCLASSEXW
        {
            cbSize = (uint)Marshal.SizeOf<WNDCLASSEXW>(),
            style = CS_DBLCLKS_OFF,
            lpfnWndProc = GetProcAddress(user32, "DefWindowProcW"),
            hInstance = GetModuleHandleW(null),
            hCursor = LoadCursorW(IntPtr.Zero, 32512),  // IDC_ARROW
            lpszClassName = ClassName,
        };
        RegisterClassExW(ref wc);
        registered = true;
    }

    private const int WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000, WS_CLIPCHILDREN = 0x02000000, WS_CLIPSIBLINGS = 0x04000000;
    private const uint CS_DBLCLKS_OFF = 0;
    private const int WM_SIZE = 0x0005, WM_PAINT = 0x000F, WM_ERASEBKGND = 0x0014, WM_KEYDOWN = 0x0100, WM_CHAR = 0x0102,
                      WM_SYSKEYDOWN = 0x0104, WM_GETDLGCODE = 0x0087, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
                      WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207,
                      WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A;
    private const long MK_LBUTTON = 1, MK_RBUTTON = 2, MK_MBUTTON = 0x10;
    private const int DLGC_WANTALLKEYS = 0x0004;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEXW
    {
        public uint cbSize, style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        public string? lpszMenuName;
        public string lpszClassName;
        public IntPtr hIconSm;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X, Y;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClassExW(ref WNDCLASSEXW wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowExW(int exStyle, string className, string name, int style, int x, int y, int w, int h,
                                                 IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool ValidateRect(IntPtr hwnd, IntPtr rect);
    [DllImport("user32.dll")] private static extern IntPtr SetFocus(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr SetCapture(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern short GetKeyState(int key);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr LoadCursorW(IntPtr instance, int name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string? name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
}
