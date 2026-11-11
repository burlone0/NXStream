using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;

namespace NXStream.Core;

public static class SelfReplace
{
    private const string OldName = "NXStream.old.exe";
    private const string AfterUpdate = "--after-update";

    public static bool TryGetTarget(out string path)
    {
        path = "";
        if (OperatingSystem.IsWindows())
        {
            var p = Environment.ProcessPath;
            if (string.IsNullOrEmpty(p)) return false;
            path = p;
            return true;
        }
        var env = Environment.GetEnvironmentVariable("APPIMAGE");
        if (string.IsNullOrEmpty(env) || !File.Exists(env))
        {
            Log.Write("update", "not an AppImage, updates disabled");
            return false;
        }
        var resolved = new FileInfo(env).ResolveLinkTarget(true)?.FullName ?? Path.GetFullPath(env);
        if (!File.Exists(resolved))
        {
            Log.Write("update", "not an AppImage, updates disabled");
            return false;
        }
        path = resolved;
        return true;
    }

    public static string TempPath(string target)
    {
        var dir = Path.GetDirectoryName(target) ?? ".";
        var name = Path.GetFileName(target);
        return OperatingSystem.IsWindows()
            ? Path.Combine(dir, name + ".update.tmp")
            : Path.Combine(dir, "." + name + ".update.tmp");
    }

    public static bool CanWrite(string dir)
    {
        var probe = Path.Combine(dir, ".nxstream-write-test-" + Environment.ProcessId);
        try
        {
            using (File.Create(probe)) { }
            File.Delete(probe);
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Log.Write("update", $"folder not writable: {ex.GetType().Name}");
            return false;
        }
    }

    public static void Swap(string target, string downloaded)
    {
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(downloaded,
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                UnixFileMode.GroupRead | UnixFileMode.GroupExecute |
                UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
            File.Move(downloaded, target, overwrite: true);
            Log.Write("update", "swap done");
            return;
        }
        var old = Path.Combine(Path.GetDirectoryName(target) ?? ".", OldName);
        Retry(() => File.Move(target, old, overwrite: true));
        try
        {
            Retry(() => File.Move(downloaded, target, overwrite: true));
        }
        catch
        {
            Log.Write("update", "swap failed, restoring previous exe");
            Retry(() => File.Move(old, target, overwrite: true));
            throw;
        }
        Log.Write("update", "swap done");
    }

    private static void Retry(Action action)
    {
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                action();
                return;
            }
            catch (Exception ex) when (attempt < 3 && ex is IOException or UnauthorizedAccessException)
            {
                Thread.Sleep(300);
            }
        }
    }

    public static void Relaunch(string target, IEnumerable<string> extraArgs)
    {
        var psi = new ProcessStartInfo(target) { UseShellExecute = false };
        psi.ArgumentList.Add(AfterUpdate);
        psi.ArgumentList.Add(Environment.ProcessId.ToString());
        foreach (var a in extraArgs) psi.ArgumentList.Add(a);
        if (!OperatingSystem.IsWindows())
        {
            foreach (var key in new[] { "APPIMAGE", "APPDIR", "ARGV0", "OWD" }) psi.Environment.Remove(key);
        }
        Process.Start(psi)?.Dispose();
        Log.Write("update", "relaunched");
    }

    public static string[] ConsumeRelaunchArgs(string[] args)
    {
        var i = Array.IndexOf(args, AfterUpdate);
        if (i < 0 || i + 1 >= args.Length) return args;
        if (int.TryParse(args[i + 1], out var pid))
        {
            try
            {
                using var p = Process.GetProcessById(pid);
                p.WaitForExit(10000);
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
            }
        }
        return args.Where((_, idx) => idx != i && idx != i + 1).ToArray();
    }

    public static void CleanupOld()
    {
        string? dir = null;
        if (OperatingSystem.IsWindows())
            dir = Path.GetDirectoryName(Environment.ProcessPath);
        else if (TryGetTarget(out var t))
            dir = Path.GetDirectoryName(t);
        if (!string.IsNullOrEmpty(dir)) CleanupOld(dir);
    }

    public static void CleanupOld(string dir)
    {
        try
        {
            var old = Path.Combine(dir, OldName);
            if (File.Exists(old))
            {
                File.Delete(old);
                Log.Write("update", "removed previous exe");
            }
            foreach (var f in Directory.EnumerateFiles(dir, "*.update.tmp"))
            {
                File.Delete(f);
                Log.Write("update", "removed stale temp file");
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Log.Write("update", $"cleanup skipped: {ex.GetType().Name}");
        }
    }
}
