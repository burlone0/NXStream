using System.Diagnostics;
using NXStream.Core;

namespace NXStream.Linux;

public static class FontLocator
{
    public const string SansFile = "selawk.ttf";
    public const string SansSemiboldFile = "selawksb.ttf";
    public const string MonoFile = "Inconsolata-Regular.ttf";
    public const string FallbackFile = "DejaVuSans.ttf";

    public static string? Sans => Resolve(SansFile, "sans-serif");
    public static string? SansSemibold => Resolve(SansSemiboldFile, "sans-serif:weight=demibold");
    public static string? Mono => Resolve(MonoFile, "monospace");
    public static string? Fallback => Resolve(FallbackFile, "DejaVu Sans");

    public static string? Find(string fileName)
    {
        foreach (var dir in Directories())
        {
            var path = Path.Combine(dir, fileName);
            if (File.Exists(path)) return path;
        }
        return null;
    }

    private static string? Resolve(string fileName, string pattern)
    {
        var found = Find(fileName) ?? Match(pattern);
        if (found is null) Log.Write("ui", $"font {fileName} not found and fc-match gave nothing for '{pattern}'");
        return found;
    }

    private static IEnumerable<string> Directories()
    {
        yield return Path.Combine(AppContext.BaseDirectory, "fonts");
        var appDir = Environment.GetEnvironmentVariable("APPDIR");
        if (!string.IsNullOrEmpty(appDir)) yield return Path.Combine(appDir, "usr", "share", "nxstream", "fonts");
    }

    private static string? Match(string pattern)
    {
        try
        {
            var info = new ProcessStartInfo("fc-match", ["-f", "%{file}", pattern])
            {
                RedirectStandardOutput = true,
                UseShellExecute = false,
            };
            using var process = Process.Start(info);
            if (process is null) return null;
            var output = process.StandardOutput.ReadToEndAsync();
            if (!process.WaitForExit(2000))
            {
                process.Kill();
                return null;
            }
            var path = output.Result.Trim();
            return path.Length > 0 && File.Exists(path) ? path : null;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            return null;
        }
    }
}
