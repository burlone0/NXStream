using System.Diagnostics;

namespace NXStream.Core;

public static class Log
{
    private static readonly object Sync = new();
    private static StreamWriter? _writer;

    public static string Folder { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NXStream");

    private static readonly string Home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);

    public static string FilePath => Path.Combine(Folder, "log.txt");

    public static void Open()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            var old = Path.Combine(Folder, "log.old.txt");
            if (File.Exists(FilePath)) File.Copy(FilePath, old, true);
            var stream = new FileStream(FilePath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
            _writer = new StreamWriter(stream) { AutoFlush = true };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _writer = null;
        }
    }

    public static void Write(string area, string message)
    {
        var line = $"{DateTime.Now:HH:mm:ss.fff} [{area,-7}] {message}";
        if (Home.Length > 1) line = line.Replace(Home, "~");
        lock (Sync)
        {
            try { _writer?.WriteLine(line); }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException) { _writer = null; }
        }
        Debug.WriteLine(line);
    }

    public static void Error(string area, string message, Exception ex) =>
        Write(area, $"{message}: {ex.GetType().Name}: {ex.Message}{Environment.NewLine}{ex.StackTrace}");

    public static void Close()
    {
        lock (Sync)
        {
            _writer?.Dispose();
            _writer = null;
        }
    }
}
