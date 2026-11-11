using System.Runtime.InteropServices;

namespace NXStream.Linux;

internal sealed class SingleInstance : IDisposable
{
    private readonly FileStream _file;

    private SingleInstance(FileStream file) => _file = file;

    public static SingleInstance? TryAcquire()
    {
        var runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        var folder = !string.IsNullOrEmpty(runtime) && Directory.Exists(runtime)
            ? runtime
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".cache");
        try
        {
            Directory.CreateDirectory(folder);
            var path = Path.Combine(folder, "nxstream.lock");
            return new SingleInstance(new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None));
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return new SingleInstance(null!);
        }
    }

    public static void ShowAlreadyRunning() =>
        SDL_ShowSimpleMessageBox(0x40, "NXStream", "NXStream is already open.", IntPtr.Zero);

    public void Dispose() => _file?.Dispose();

    [DllImport("libSDL2-2.0.so.0")]
    private static extern int SDL_ShowSimpleMessageBox(uint flags, [MarshalAs(UnmanagedType.LPUTF8Str)] string title,
                                                       [MarshalAs(UnmanagedType.LPUTF8Str)] string message, IntPtr window);
}
