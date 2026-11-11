using System.Diagnostics;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Net;
using NXStream.Protocol;

namespace NXStream.Linux;

internal sealed class UdevUsbAccess : IUsbAccess
{
    private const string RulePath = "/etc/udev/rules.d/70-nxstream.rules";
    private static readonly string[] Rules =
    [
        "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"18d1\", ATTR{idProduct}==\"4ee0\", ATTR{product}==\"NXStream\", TAG+=\"uaccess\"",
        $"SUBSYSTEM==\"usb\", ATTR{{idVendor}}==\"18d1\", ATTR{{idProduct}}==\"{Wire.NativeUsbProductId:x4}\", ATTR{{product}}==\"{Wire.NativeUsbProductName}\", TAG+=\"uaccess\"",
    ];
    private static readonly string Rule = string.Join("\n", Rules);
    private const string Reload = "udevadm control --reload-rules && udevadm trigger --subsystem-match=usb --attr-match=idVendor=18d1";
    private const string InstallScript = "install -m 0644 \"$1\" " + RulePath + " && " + Reload;
    private const string RemoveScript = "rm -f " + RulePath + " && " + Reload;

    public static string ManualCommand(bool remove) => remove
        ? "su -c '" + RemoveScript + "'"
        : "su -c \"printf '%s\\n' " + string.Join(" ", Rules.Select(r => "'" + r + "'")) + " > " + RulePath + " && " + Reload + "\"";

    public UsbDriverStatus GetStatus()
    {
        try
        {
            var devices = LibUsb.Find(0x18d1, 0x4ee0);
            devices.AddRange(LibUsb.Find(0x18d1, Wire.NativeUsbProductId));
            if (devices.Count > 0)
            {
                var status = UsbDriverStatus.Ready;
                foreach (var device in devices)
                {
                    var code = LibUsb.Open(device.Device, out var handle);
                    if (code == LibUsb.Success) LibUsb.Close(handle);
                    else if (status == UsbDriverStatus.Ready) status = code == LibUsb.Access ? UsbDriverStatus.NoPermission : UsbDriverStatus.Unknown;
                    device.Dispose();
                }
                return status;
            }
            if (!File.Exists(RulePath)) return UsbDriverStatus.NotInstalled;
            return File.ReadAllText(RulePath).Trim() == Rule ? UsbDriverStatus.Ready : UsbDriverStatus.NotInstalled;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or DllNotFoundException or EntryPointNotFoundException or UsbException)
        {
            Log.Write("usb", $"driver status failed: {ex.GetType().Name}");
            return UsbDriverStatus.Unknown;
        }
    }

    public Task<bool> InstallAsync() => Task.Run(Install);

    public Task<bool> RepairAsync() => Task.Run(Install);

    public Task<bool> UninstallAsync() => Task.Run(() => Elevate(RemoveScript, null));

    private static bool Install()
    {
        if (!OperatingSystem.IsLinux()) return false;
        var folder = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder)) folder = Path.GetTempPath();
        var temp = Path.Combine(folder, "nxstream-" + Guid.NewGuid().ToString("N") + ".rules");
        try
        {
            var options = new FileStreamOptions
            {
                Mode = FileMode.CreateNew,
                Access = FileAccess.Write,
                Share = FileShare.None,
                UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite,
            };
            using (var writer = new StreamWriter(temp, options)) writer.Write(Rule + "\n");
            return Elevate(InstallScript, temp);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Log.Write("usb", $"rule file write failed: {ex.GetType().Name}");
            return false;
        }
        finally
        {
            try { File.Delete(temp); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
    }

    private static bool Elevate(string script, string? argument)
    {
        try
        {
            var start = new ProcessStartInfo("pkexec")
            {
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            start.ArgumentList.Add("sh");
            start.ArgumentList.Add("-c");
            start.ArgumentList.Add(script);
            start.ArgumentList.Add("sh");
            if (argument is not null) start.ArgumentList.Add(argument);
            start.Environment.Remove("LD_LIBRARY_PATH");
            using var process = Process.Start(start);
            if (process is null) return false;
            _ = process.StandardOutput.ReadToEndAsync();
            _ = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            Log.Write("usb", $"elevation exit code {process.ExitCode}");
            return process.ExitCode == 0;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            Log.Write("usb", $"elevation tool unavailable: {ex.GetType().Name}");
            return false;
        }
    }
}

internal static class NativeDialog
{
    public static bool Ask(string message, string yes, string no) => Show(message, yes, no) == 1;

    public static void Inform(string message) => Show(message, "OK", null);

    private static int Show(string message, string first, string? second)
    {
        var size = Marshal.SizeOf<SdlMessageBoxButton>();
        var texts = new List<IntPtr>
        {
            Marshal.StringToCoTaskMemUTF8("NXStream"),
            Marshal.StringToCoTaskMemUTF8(message),
            Marshal.StringToCoTaskMemUTF8(first),
        };
        if (second is not null) texts.Add(Marshal.StringToCoTaskMemUTF8(second));
        var count = second is null ? 1 : 2;
        var buttons = Marshal.AllocHGlobal(size * count);
        try
        {
            Marshal.StructureToPtr(new SdlMessageBoxButton { Flags = 1, Id = 1, Text = texts[2] }, buttons, false);
            if (second is not null)
                Marshal.StructureToPtr(new SdlMessageBoxButton { Flags = 2, Id = 0, Text = texts[3] }, buttons + size, false);
            var data = new SdlMessageBoxData
            {
                Flags = 0x40,
                Title = texts[0],
                Message = texts[1],
                ButtonCount = count,
                Buttons = buttons,
            };
            return Sdl.SDL_ShowMessageBox(ref data, out var id) == 0 ? id : 0;
        }
        finally
        {
            Marshal.FreeHGlobal(buttons);
            foreach (var text in texts) Marshal.FreeCoTaskMem(text);
        }
    }
}
