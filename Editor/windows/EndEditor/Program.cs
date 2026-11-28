using System.IO;
using System.Windows;

namespace EndEditor;

public static class Program
{
    /// `END Editor.exe [--play-app <game exe>]` opens the last Zero Hour folder (or Steam's); --play-app
    /// sets the game Play starts (kept by the core, like the Play settings' Game app).
    /// `END Editor.exe --ui-check <zh> <base> <map path> <template> <save name> <game exe> <out.txt>` drives the
    /// real window through UI Automation and posted input (UICheck.cs); the exit code is the number
    /// of failed checks.
    [STAThread]
    public static int Main(string[] args)
    {
        if (Snapshot.RunIfRequested(args) is { } snapshot) return snapshot;
        var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
        var check = UICheck.Parse(args);
        // A failure in a view is reported, not a crash that loses the user's edits (the core keeps them).
        app.DispatcherUnhandledException += (_, e) =>
        {
            Console.Error.WriteLine("END Editor: " + e.Exception);
            if (check != null) UICheck.Failures.Add(e.Exception.Message);
            else MessageBox.Show(e.Exception.Message, "END Editor", MessageBoxButton.OK, MessageBoxImage.Error);
            e.Handled = true;
        };
        var editor = new EditorModel(check != null ? Path.Combine(Path.GetTempPath(), "end-editor-ui-check.sqlite") : null);
        var window = new MainWindow(editor);
        window.Loaded += (_, _) =>
        {
            if (check != null) editor.Start(check.ZeroHour, check.Base);
            else editor.Start();
            var playApp = Array.IndexOf(args, "--play-app");
            if (check == null && playApp >= 0 && playApp + 1 < args.Length) editor.Send("play.app\t" + args[playApp + 1]);
            if (check != null) UICheck.Run(check, window);
        };
        window.Closed += (_, _) => editor.Shutdown();
        var code = app.Run(window);
        return check != null ? UICheck.ExitCode : code;
    }
}
