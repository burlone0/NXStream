using System.ComponentModel;
using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.RegularExpressions;
using NXStream.Core;
using NXStream.Net;
using NXStream.Protocol;

namespace NXStream.Platform;

public sealed partial class WindowsUsbDriver : IUsbAccess
{
    public const string DriverUrl = "https://dl.google.com/android/repository/usb_driver_r13-windows.zip";
    private const string DriverSha256 = "360b01d3dfb6c41621a3a64ae570dfac2c9a40cca1b5a1f136ae90d02f5e9e0b";
    private const long DriverSize = 8682039;
    private const string InfName = "android_winusb.inf";
    private const string DriverHost = "dl.google.com";
    private const string ZipName = "usb_driver.zip";
    private const string PackageDir = "pkg";
    private const string HelperLog = "helper.log";
    private const ushort Vid = 0x18D1;
    private const ushort Pid = 0x4EE0;

    private static readonly string DriverDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NXStream", "usb_driver");

    private static readonly string ProtectedDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "NXStream", "usb_driver");

    private readonly object _lock = new();
    private UsbDriverStatus _cached = UsbDriverStatus.Unknown;
    private long _cachedAt;

    public UsbDriverStatus GetStatus()
    {
        lock (_lock)
        {
            if (_cachedAt != 0 && Environment.TickCount64 - _cachedAt < 3000) return _cached;
            _cached = Probe();
            _cachedAt = Environment.TickCount64;
            return _cached;
        }
    }

    private void Invalidate()
    {
        lock (_lock) _cachedAt = 0;
    }

    private static UsbDriverStatus Probe()
    {
        try
        {
            var devices = LibUsb.Find(Vid, Pid);
            devices.AddRange(LibUsb.Find(Vid, Wire.NativeUsbProductId));
            try
            {
                if (devices.Count > 0)
                {
                    var rc = LibUsb.Open(devices[0].Device, out var handle);
                    if (rc == LibUsb.Success)
                    {
                        LibUsb.Close(handle);
                        return UsbDriverStatus.Ready;
                    }
                    if (rc == LibUsb.Busy) return UsbDriverStatus.Ready;
                    if (rc is LibUsb.NotSupported or LibUsb.NotFound) return UsbDriverStatus.NotInstalled;
                    Log.Write("usb", $"driver probe: open failed {LibUsb.ErrorName(rc)}");
                    return UsbDriverStatus.Unknown;
                }
            }
            finally
            {
                foreach (var d in devices) d.Dispose();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or UsbException or EntryPointNotFoundException)
        {
            Log.Write("usb", $"driver probe: {ex.GetType().Name}");
            return UsbDriverStatus.Unknown;
        }
        var (code, output) = RunPnputil("/enum-drivers");
        if (code != 0) return UsbDriverStatus.Unknown;
        return output.Contains(InfName, StringComparison.OrdinalIgnoreCase) ? UsbDriverStatus.Ready : UsbDriverStatus.NotInstalled;
    }

    public async Task<bool> InstallAsync() => await RunElevated("install", true);

    public async Task<bool> RepairAsync() => await RunElevated("repair", true);

    public async Task<bool> UninstallAsync() => await RunElevated("uninstall", false);

    private async Task<bool> RunElevated(string verb, bool download)
    {
        try
        {
            if (download && !await Download()) return false;
            var exe = Environment.ProcessPath ?? throw new InvalidOperationException("no process path");
            var info = new ProcessStartInfo(exe)
            {
                UseShellExecute = true,
                Verb = "runas",
                Arguments = $"--usb-driver {verb}",
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            Log.Write("usb", $"driver {verb}: starting elevated helper");
            using var process = Process.Start(info);
            if (process is null) return false;
            await process.WaitForExitAsync();
            var logPath = Path.Combine(ProtectedDir, HelperLog);
            if (File.Exists(logPath))
            {
                using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var reader = new StreamReader(stream);
                while (reader.ReadLine() is { } line) Log.Write("usb", $"helper: {line}");
            }
            Log.Write("usb", $"driver {verb}: helper exit code {process.ExitCode}");
            return process.ExitCode == 0;
        }
        catch (Exception ex) when (ex is Win32Exception or IOException or HttpRequestException or InvalidDataException or
                                       UnauthorizedAccessException or InvalidOperationException or TaskCanceledException)
        {
            Log.Write("usb", $"driver {verb} failed: {ex.GetType().Name}");
            return false;
        }
        finally
        {
            Invalidate();
        }
    }

    private static async Task<bool> Download()
    {
        Log.Write("usb", "driver download started");
        Directory.CreateDirectory(DriverDir);
        var zipPath = Path.Combine(DriverDir, ZipName);
        if (File.Exists(zipPath)) File.Delete(zipPath);
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        using var response = await http.GetAsync(DriverUrl, HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();
        var finalUri = response.RequestMessage?.RequestUri;
        if (finalUri is null || finalUri.Scheme != Uri.UriSchemeHttps || !finalUri.Host.Equals(DriverHost, StringComparison.OrdinalIgnoreCase))
        {
            Log.Write("usb", "driver download rejected: unexpected host");
            return false;
        }
        if (response.Content.Headers.ContentLength is { } length && length != DriverSize)
        {
            Log.Write("usb", $"driver download rejected: size {length}");
            return false;
        }
        long total = 0;
        using (var source = await response.Content.ReadAsStreamAsync())
        using (var target = new FileStream(zipPath, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            var buffer = new byte[81920];
            int read;
            while ((read = await source.ReadAsync(buffer)) > 0)
            {
                total += read;
                if (total > DriverSize)
                {
                    Log.Write("usb", "driver download rejected: too large");
                    return false;
                }
                await target.WriteAsync(buffer.AsMemory(0, read));
            }
        }
        if (total != DriverSize)
        {
            Log.Write("usb", $"driver download rejected: size {total}");
            return false;
        }
        Log.Write("usb", "driver download completed");
        return true;
    }

    private static bool IsReparse(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; }
        catch (Exception ex) when (ex is FileNotFoundException or DirectoryNotFoundException) { return false; }
    }

    private static DirectorySecurity ProtectedSecurity()
    {
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);
        var security = new DirectorySecurity();
        security.SetOwner(admins);
        security.SetAccessRuleProtection(true, false);
        var inherit = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
        security.AddAccessRule(new FileSystemAccessRule(admins, FileSystemRights.FullControl, inherit, PropagationFlags.None, AccessControlType.Allow));
        security.AddAccessRule(new FileSystemAccessRule(system, FileSystemRights.FullControl, inherit, PropagationFlags.None, AccessControlType.Allow));
        security.AddAccessRule(new FileSystemAccessRule(users, FileSystemRights.ReadAndExecute, inherit, PropagationFlags.None, AccessControlType.Allow));
        return security;
    }

    private static void PrepareProtectedDir()
    {
        var parent = Path.GetDirectoryName(ProtectedDir)!;
        if (IsReparse(parent)) throw new IOException("reparse point");
        if (Directory.Exists(parent)) new DirectoryInfo(parent).SetAccessControl(ProtectedSecurity());
        else new DirectoryInfo(parent).Create(ProtectedSecurity());
        if (Directory.Exists(ProtectedDir))
        {
            if (IsReparse(ProtectedDir)) Directory.Delete(ProtectedDir, false);
            else Directory.Delete(ProtectedDir, true);
        }
        new DirectoryInfo(ProtectedDir).Create(ProtectedSecurity());
        if (IsReparse(ProtectedDir)) throw new IOException("reparse point");
    }

    private static void StageArchive(Action<string> say)
    {
        var source = Path.Combine(DriverDir, ZipName);
        if (IsReparse(DriverDir) || IsReparse(source)) throw new IOException("reparse point");
        var staged = Path.Combine(ProtectedDir, ZipName);
        using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var output = new FileStream(staged, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            var buffer = new byte[81920];
            long total = 0;
            int read;
            while ((read = input.Read(buffer)) > 0)
            {
                total += read;
                if (total > DriverSize) throw new InvalidDataException("archive too large");
                output.Write(buffer, 0, read);
            }
        }
        var info = new FileInfo(staged);
        string hash;
        using (var verify = new FileStream(staged, FileMode.Open, FileAccess.Read, FileShare.Read))
            hash = Convert.ToHexString(SHA256.HashData(verify)).ToLowerInvariant();
        if (info.Length != DriverSize || hash != DriverSha256) throw new InvalidDataException("archive rejected");
        say("archive verified");
    }

    private static string ExtractArchive(Action<string> say)
    {
        var pkg = Path.Combine(ProtectedDir, PackageDir);
        Directory.CreateDirectory(pkg);
        var root = Path.GetFullPath(pkg) + Path.DirectorySeparatorChar;
        using var zip = ZipFile.OpenRead(Path.Combine(ProtectedDir, ZipName));
        foreach (var entry in zip.Entries)
        {
            if (entry.Name.Length == 0) continue;
            var relative = entry.FullName.StartsWith("usb_driver/", StringComparison.Ordinal) ? entry.FullName[11..] : entry.FullName;
            var target = Path.GetFullPath(Path.Combine(pkg, relative));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
            var folder = Path.GetDirectoryName(target)!;
            Directory.CreateDirectory(folder);
            if (IsReparse(folder) || IsReparse(target)) throw new IOException("reparse point");
            entry.ExtractToFile(target, true);
        }
        say("archive extracted");
        return Path.Combine(pkg, InfName);
    }

    public static int RunHelper(string[] args)
    {
        if (args.Length != 1 || args[0] is not ("install" or "repair" or "uninstall")) return 2;
        var verb = args[0];
        var lines = new List<string>();
        var ready = false;
        void Say(string line)
        {
            lines.Add(line);
            if (!ready) return;
            try
            {
                using var stream = new FileStream(Path.Combine(ProtectedDir, HelperLog), FileMode.Create, FileAccess.Write, FileShare.Read);
                using var writer = new StreamWriter(stream);
                foreach (var l in lines) writer.WriteLine(l);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
        try
        {
            PrepareProtectedDir();
            ready = true;
            Say($"helper {verb}");
            switch (verb)
            {
                case "install":
                case "repair":
                {
                    StageArchive(Say);
                    var inf = ExtractArchive(Say);
                    if (!File.Exists(inf)) { Say("inf missing"); return 2; }
                    var code = Pnputil($"/add-driver \"{inf}\" /install", Say);
                    if (code == 0 && verb == "repair") code = Pnputil("/scan-devices", Say);
                    return code;
                }
                default:
                {
                    var (code, output) = RunPnputil("/enum-drivers");
                    Say($"enum-drivers exit {code}");
                    if (code != 0) return code;
                    var published = FindPublished(output);
                    if (published.Count == 0) { Say("driver not present"); return 0; }
                    var result = 0;
                    foreach (var oem in published)
                    {
                        var c = Pnputil($"/delete-driver {oem} /uninstall /force", Say);
                        if (c != 0) result = c;
                    }
                    return result;
                }
            }
        }
        catch (Exception ex) when (ex is Win32Exception or IOException or InvalidOperationException or InvalidDataException or
                                       UnauthorizedAccessException or SystemException)
        {
            Say($"{ex.GetType().Name}");
            return 1;
        }
    }

    private static List<string> FindPublished(string output)
    {
        var found = new List<string>();
        foreach (var block in Regex.Split(output.Replace("\r", ""), @"\n\s*\n"))
        {
            string? oem = null;
            var original = false;
            var google = false;
            foreach (var raw in block.Split('\n'))
            {
                var line = raw.Trim();
                var m = OemRegex().Match(line);
                if (m.Success) oem = m.Value.ToLowerInvariant();
                else if (line.EndsWith(InfName, StringComparison.OrdinalIgnoreCase)) original = true;
                else if (line.Contains("Google", StringComparison.OrdinalIgnoreCase)) google = true;
            }
            if (oem is not null && original && google && !found.Contains(oem)) found.Add(oem);
        }
        return found;
    }

    [GeneratedRegex(@"\boem\d+\.inf\b", RegexOptions.IgnoreCase)]
    private static partial Regex OemRegex();

    private static int Pnputil(string arguments, Action<string> say)
    {
        var (code, _) = RunPnputil(arguments);
        say($"pnputil {arguments.Split(' ')[0]} exit {code}");
        return code is 0 or 3010 ? 0 : code;
    }

    private static (int Code, string Output) RunPnputil(string arguments)
    {
        var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "pnputil.exe"), arguments)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        try
        {
            using var process = Process.Start(info);
            if (process is null) return (-1, "");
            var error = process.StandardError.ReadToEndAsync();
            var output = process.StandardOutput.ReadToEnd();
            if (!process.WaitForExit(60000)) { process.Kill(); return (-1, output); }
            return (process.ExitCode, output + error.Result);
        }
        catch (Exception ex) when (ex is Win32Exception or InvalidOperationException)
        {
            return (-1, "");
        }
    }
}
