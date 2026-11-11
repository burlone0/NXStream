namespace NXStream.Media;

public static class FfmpegLocator
{
    public static string? Find()
    {
        foreach (var candidate in Candidates())
        {
            try
            {
                if (!File.Exists(candidate)) continue;
                var path = Path.GetFullPath(candidate);
                Core.Log.Write("app", "ffmpeg found");
                _ = Task.Run(() => LogVersion(path));
                return path;
            }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException or IOException)
            {
            }
        }
        Core.Log.Write("app", "ffmpeg not found");
        return null;
    }

    private static void LogVersion(string path)
    {
        try
        {
            using var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path, "-hide_banner -version")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
            });
            if (p is null) return;
            var first = p.StandardOutput.ReadLine();
            p.WaitForExit(3000);
            Core.Log.Write("app", $"ffmpeg version: {first}");
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            Core.Log.Write("app", "ffmpeg version unknown: " + (ex is System.ComponentModel.Win32Exception w
                ? $"win32 error {w.NativeErrorCode}" : ex.GetType().Name));
        }
    }

    private static IEnumerable<string> Candidates()
    {
        var baseDir = AppContext.BaseDirectory;
        if (!OperatingSystem.IsWindows())
        {
            yield return Path.Combine(baseDir, "ffmpeg");
            if (Environment.GetEnvironmentVariable("APPDIR") is { Length: > 0 } appDir)
                yield return Path.Combine(appDir, "usr", "bin", "ffmpeg");
            var pathDirs = (Environment.GetEnvironmentVariable("PATH") ?? "")
                .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);
            foreach (var dir in pathDirs)
                yield return Path.Combine(dir, "ffmpeg");
            yield break;
        }
        yield return Path.Combine(baseDir, "ffmpeg.exe");
        yield return Path.Combine(baseDir, "ffmpeg", "ffmpeg.exe");
        yield return Path.Combine(baseDir, "ffmpeg", "bin", "ffmpeg.exe");

        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var dir in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            yield return Path.Combine(dir.Trim('"'), "ffmpeg.exe");

        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        yield return Path.Combine(localAppData, "Microsoft", "WinGet", "Links", "ffmpeg.exe");

        var packages = Path.Combine(localAppData, "Microsoft", "WinGet", "Packages");
        if (Directory.Exists(packages))
        {
            foreach (var pkg in SafeDirs(packages, "*FFmpeg*"))
            foreach (var build in SafeDirs(pkg, "*"))
                yield return Path.Combine(build, "bin", "ffmpeg.exe");
        }

        yield return @"C:\ffmpeg\bin\ffmpeg.exe";
        yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "ffmpeg", "bin", "ffmpeg.exe");
    }

    private static IEnumerable<string> SafeDirs(string root, string pattern)
    {
        try { return Directory.EnumerateDirectories(root, pattern).ToList(); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return []; }
    }
}
