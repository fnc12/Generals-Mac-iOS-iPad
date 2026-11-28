using System.Runtime.InteropServices;

namespace EndEditor.Core;

/// The core's C ABI (Editor/core/include/end_editor/editor_core_c.h). Text is UTF-8 both ways.
internal static partial class Native
{
    private const string Dll = "end_editor_core_c";

    [LibraryImport(Dll, EntryPoint = "end_editor_get_version")]
    private static partial nint GetVersionRaw();

    [LibraryImport(Dll, EntryPoint = "end_editor_initialize", StringMarshalling = StringMarshalling.Utf8)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static partial bool Initialize(string dbPath);

    [LibraryImport(Dll, EntryPoint = "end_editor_shutdown")]
    public static partial void Shutdown();

    [LibraryImport(Dll, EntryPoint = "end_editor_db_path")]
    private static partial nint DbPathRaw();

    [LibraryImport(Dll, EntryPoint = "end_editor_send", StringMarshalling = StringMarshalling.Utf8)]
    private static partial nint SendRaw(string command);

    [LibraryImport(Dll, EntryPoint = "end_editor_update")]
    public static partial void Update();

    [LibraryImport(Dll, EntryPoint = "end_editor_pending_work")]
    public static partial int PendingWork();

    public static string Version => Marshal.PtrToStringUTF8(GetVersionRaw()) ?? "";
    public static string DbPath => Marshal.PtrToStringUTF8(DbPathRaw()) ?? "";

    /// "" when accepted, else "error:<code>".
    public static string Send(string command) => Marshal.PtrToStringUTF8(SendRaw(command)) ?? "";
}
