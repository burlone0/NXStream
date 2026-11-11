using NXStream.Core;
using NXStream.Net;
using NXStream.Platform;

namespace NXStream;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length >= 1 && args[0] == "--usb-driver")
        {
            Environment.Exit(WindowsUsbDriver.RunHelper(args[1..]));
            return;
        }
        var original = args.Length;
        args = SelfReplace.ConsumeRelaunchArgs(args);
        var relaunched = args.Length != original;
        using var single = new Mutex(true, @"Local\NXStream.Viewer", out var first);
        if (!first && relaunched)
        {
            var deadline = Environment.TickCount64 + 30000;
            while (!first && Environment.TickCount64 < deadline)
            {
                try { first = single.WaitOne(250); }
                catch (AbandonedMutexException) { first = true; }
            }
        }
        if (!first)
        {
            ApplicationConfiguration.Initialize();
            MessageBox.Show("NXStream is already open.", "NXStream", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        SelfReplace.CleanupOld();
        var uptime = AppStartup.Begin(args);

        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, e) => Log.Error("app", "unhandled UI exception", e.Exception);

        ApplicationConfiguration.Initialize();
        UsbPlatform.Access = new WindowsUsbDriver();
        UsbPlatform.AskInstall = text =>
        {
            var owner = Application.OpenForms.Count > 0 ? Application.OpenForms[0] : null;
            if (owner is { InvokeRequired: true })
                return (bool)owner.Invoke(() => AskYesNo(owner, text));
            return AskYesNo(owner, text);
        };
        try
        {
            Application.Run(new UI.MainForm(args.FirstOrDefault()));
        }
        catch (Exception ex)
        {
            Log.Error("app", "fatal error", ex);
        }
        AppStartup.End(uptime);
        Environment.Exit(0);
    }

    private static bool AskYesNo(Form? owner, string text) =>
        MessageBox.Show(owner, text, "NXStream", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
}
