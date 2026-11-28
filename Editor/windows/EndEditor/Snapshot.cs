using System.Diagnostics;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using EndEditor.Core;
using EndEditor.Render;

namespace EndEditor;

/// `END Editor.exe --map-snapshot <zh> <base> <map path> <out.png> [--top] [--size <w>x<h>]` opens a
/// map without a window, waits for all its models and renders the default view (or top-down) into
/// a PNG, then exits: a check on a machine nobody is watching.
public static class Snapshot
{
    public static int? RunIfRequested(string[] args)
    {
        var at = Array.IndexOf(args, "--map-snapshot");
        if (at < 0) return null;
        var values = args[(at + 1)..];
        if (values.Length < 4)
        {
            Console.Error.WriteLine("usage: --map-snapshot <zh> <base> <map path> <out.png> [--top] [--size <w>x<h>]");
            return 2;
        }
        int width = 1280, height = 800;
        var top = false;
        for (int i = 4; i < values.Length; ++i)
        {
            if (values[i] == "--top") top = true;
            else if (values[i] == "--size" && i + 1 < values.Length && values[++i].Split('x') is [var w, var h])
            {
                width = int.Parse(w);
                height = int.Parse(h);
            }
        }
        var started = Stopwatch.StartNew();
        var editor = new EditorModel(Path.Combine(Path.GetTempPath(), "end-editor-map-snapshot.sqlite"));
        editor.OpenGame(values[0], values[1].Length > 0 ? values[1] : null);
        while (Native.PendingWork() > 0) Native.Update();
        editor.Tick();
        editor.OpenMap(values[2]);
        while (Native.PendingWork() > 0) Native.Update();
        editor.Tick();
        Console.WriteLine($"game: {editor.Status.State} {editor.Status.Message}");
        Console.WriteLine($"map: {editor.MapState} {editor.MapMessage}");
        if (editor.MapState != "ready") return 1;
        var renderer = new MapRenderer();
        renderer.Show(editor.MapScene);
        renderer.Add(editor.MapModels, editor.MapModelTextures);
        if (top) renderer.Camera.Frame(editor.MapScene, true);
        Console.WriteLine($"models: {editor.MapModels.Count(m => m.Ready)} of {editor.MapModels.Count} ready, " +
                          $"{editor.MapModelTextures.Count} textures, {editor.MapScene.Objects.Count} objects; " +
                          $"{started.Elapsed.TotalSeconds:0.0} s; GPU {Gpu.Shared.AdapterName}{(Gpu.Shared.Software ? " (WARP)" : "")}");
        foreach (var warning in editor.MapWarnings) Console.WriteLine($"warning: {warning}");
        var pixels = renderer.RenderPixels(width, height, 1);
        Save(pixels, width, height, values[3]);
        editor.Shutdown();
        return 0;
    }

    /// BGRA rows, top to bottom, as a PNG.
    public static void Save(byte[] pixels, int width, int height, string path)
    {
        var bitmap = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgr32, null, pixels, width * 4);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var file = File.Create(path);
        encoder.Save(file);
    }
}
