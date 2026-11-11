using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Linux.Ui;
using NXStream.Net;
using NXStream.Render;

namespace NXStream.Linux;

internal static class App
{
    private static readonly ConcurrentQueue<Action> Queue = new();
    private static readonly Lazy<UiPalette> LazyPalette = new(() => UiPalette.For(ThemeDetect.IsDark()));
    private static readonly Lazy<byte[]> LazyIcon = new(LoadIcon);

    public static UiPalette Palette => LazyPalette.Value;
    public static UiWindow? Modal { get; set; }
    public static MainWindow? Main { get; set; }
    public static ViewerWindow? Viewer { get; set; }
    public static bool Quitting { get; private set; }

    public static void Post(Action action) => Queue.Enqueue(action);

    public static void RequestQuit() => Quitting = true;

    public static void Drain()
    {
        while (Queue.TryDequeue(out var action))
        {
            try { action(); }
            catch (Exception ex) when (ex is not OutOfMemoryException) { Log.Error("app", "unhandled UI exception", ex); }
        }
    }

    public static unsafe void ApplyIcon(IntPtr window)
    {
        var icon = LazyIcon.Value;
        if (icon.Length == 0) return;
        fixed (byte* data = icon)
        {
            var surface = Sdl.IMG_Load_RW(Sdl.SDL_RWFromConstMem(data, icon.Length), 1);
            if (surface == IntPtr.Zero)
            {
                Log.Write("ui", "icon load failed: " + Sdl.Error());
                return;
            }
            Sdl.SDL_SetWindowIcon(window, surface);
            Sdl.SDL_FreeSurface(surface);
        }
    }

    private static byte[] LoadIcon()
    {
        using var stream = typeof(App).Assembly.GetManifestResourceStream("logo.png");
        if (stream is null) return [];
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        return memory.ToArray();
    }
}

internal static class Program
{
    private static int Main(string[] args)
    {
        var original = args.Length;
        args = SelfReplace.ConsumeRelaunchArgs(args);
        var relaunched = args.Length != original;
        var single = SingleInstance.TryAcquire();
        if (single is null && relaunched)
        {
            var deadline = Environment.TickCount64 + 30000;
            while (single is null && Environment.TickCount64 < deadline)
            {
                Thread.Sleep(250);
                single = SingleInstance.TryAcquire();
            }
        }
        using var holder = single;
        if (single is null)
        {
            SingleInstance.ShowAlreadyRunning();
            return 0;
        }
        SelfReplace.CleanupOld();
        var uptime = AppStartup.Begin(args);
        var exitCode = 0;
        try
        {
            Run(args.FirstOrDefault());
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("app", "fatal error", ex);
            exitCode = 1;
        }
        AppStartup.End(uptime);
        Environment.Exit(exitCode);
        return exitCode;
    }

    private static void Run(string? target)
    {
        Sdl.SDL_SetHint("SDL_APP_ID", "nxstream");
        Sdl.SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "nxstream");
        Sdl.SDL_SetHint("SDL_NO_SIGNAL_HANDLERS", "1");
        Sdl.SDL_SetHint("SDL_QUIT_ON_LAST_WINDOW_CLOSE", "0");
        Sdl.SDL_SetHint("SDL_VIDEO_ALLOW_SCREENSAVER", "1");
        if (Sdl.SDL_Init(Sdl.InitVideo | Sdl.InitEvents) != 0)
            throw new InvalidOperationException("SDL failed to initialize: " + Sdl.Error());
        Log.Write("app", $"SDL ready, video driver {Sdl.VideoDriver()}");

        using var term = PosixSignalRegistration.Create(PosixSignal.SIGTERM, OnSignal);
        using var interrupt = PosixSignalRegistration.Create(PosixSignal.SIGINT, OnSignal);
        using var hangup = PosixSignalRegistration.Create(PosixSignal.SIGHUP, OnSignal);

        UsbPlatform.Access = new UdevUsbAccess();
        UsbPlatform.AskInstall = text => NativeDialog.Ask(text, UsbText.InstallButton, UpdateText.Cancel);

        var main = new MainWindow(new LinuxMedia(), target);
        App.Main = main;
        main.Closed += App.RequestQuit;
        main.Start();

        var buffer = Marshal.AllocHGlobal(Sdl.EventSize);
        while (!App.Quitting)
        {
            if (Sdl.SDL_WaitEventTimeout(buffer, 8) != 0)
            {
                do Handle(buffer);
                while (Sdl.SDL_PollEvent(buffer) != 0 && !App.Quitting);
            }
            App.Drain();
            UiWindow.TickAll();
            App.Viewer?.Tick();
            DebugHook.Tick();
        }

        Log.Write("app", "shutting down");
        App.Viewer?.Close();
        main.Shutdown();
        Marshal.FreeHGlobal(buffer);
        Sdl.SDL_Quit();
    }

    private static void OnSignal(PosixSignalContext context)
    {
        context.Cancel = true;
        Log.Write("app", $"signal {context.Signal}");
        App.Post(App.RequestQuit);
    }

    private static void Handle(IntPtr buffer)
    {
        try
        {
            var ev = SdlEvent.Read(buffer);
            if (ev.Type == Sdl.EventQuit)
            {
                if (App.Viewer is { } open)
                {
                    open.Close();
                    return;
                }
                App.RequestQuit();
                return;
            }
            if (App.Modal is { } modal && ev.WindowId != modal.WindowId && ev.Type is >= Sdl.EventKeyDown and <= Sdl.EventMouseWheel)
                return;
            if (UiWindow.Dispatch(ev)) return;
            App.Viewer?.HandleEvent(ev);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("app", "unhandled UI exception", ex);
        }
    }
}

internal static class DebugHook
{
    private static readonly string? Folder = Environment.GetEnvironmentVariable("NXSTREAM_DEBUG_SNAPSHOT");
    private static ulong _next;

    public static void Tick()
    {
        if (Folder is null) return;
        var now = Sdl.SDL_GetTicks64();
        if (now < _next) return;
        _next = now + 1500;
        Directory.CreateDirectory(Folder);
        try
        {
            RunCommands();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("app", "debug commands failed", ex);
        }
        if (App.Main?.Window is { Visible: true } main) Save("main", main);
        if (App.Modal is { } options) Save("options", options);
        if (App.Viewer is { } viewer)
            _ = viewer.SnapshotAsync().ContinueWith(t =>
            {
                if (t.IsCompletedSuccessfully)
                    WriteBmp(Path.Combine(Folder, "video.bmp"), t.Result.Bgra, t.Result.Width, t.Result.Height, false);
            });
    }

    private static void Save(string name, UiWindow window)
    {
        var pixels = window.ReadPixels(out var width, out var height);
        WriteBmp(Path.Combine(Folder!, name + ".bmp"), pixels, width, height, true);
    }

    private static void RunCommands()
    {
        var file = Path.Combine(Folder!, "commands.txt");
        if (!File.Exists(file)) return;
        var lines = File.ReadAllLines(file);
        File.Delete(file);
        var id = App.Modal?.WindowId ?? App.Viewer?.WindowId ?? App.Main!.Window.WindowId;
        foreach (var line in lines)
        {
            var p = line.Split(' ');
            var ev = new SdlEvent { WindowId = id };
            switch (p[0])
            {
                case "key":
                    ev.Type = Sdl.EventKeyDown;
                    ev.Sym = int.Parse(p[1]);
                    ev.Mod = ushort.Parse(p[2]);
                    break;
                case "click":
                    ev.Type = Sdl.EventMouseButtonDown;
                    ev.Button = byte.Parse(p[1]);
                    ev.Clicks = byte.Parse(p[2]);
                    ev.X = int.Parse(p[3]);
                    ev.Y = int.Parse(p[4]);
                    break;
                case "move":
                    ev.Type = Sdl.EventMouseMotion;
                    ev.X = int.Parse(p[1]);
                    ev.Y = int.Parse(p[2]);
                    break;
                default:
                    continue;
            }
            Sdl.SDL_PushEvent(ref ev);
            if (p[0] != "click") continue;
            ev.Type = Sdl.EventMouseButtonUp;
            Sdl.SDL_PushEvent(ref ev);
        }
    }

    private static void WriteBmp(string path, byte[] bgra, int width, int height, bool topDown)
    {
        using var writer = new BinaryWriter(File.Create(path));
        writer.Write((byte)'B');
        writer.Write((byte)'M');
        writer.Write(54 + bgra.Length);
        writer.Write(0);
        writer.Write(54);
        writer.Write(40);
        writer.Write(width);
        writer.Write(topDown ? -height : height);
        writer.Write((short)1);
        writer.Write((short)32);
        writer.Write(0);
        writer.Write(bgra.Length);
        writer.Write(new byte[16]);
        writer.Write(bgra);
    }
}
