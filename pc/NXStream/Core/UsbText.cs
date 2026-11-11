using NXStream.Net;
using NXStream.Protocol;

namespace NXStream.Core;

public static class UsbText
{
    public const string ConnectionLabel = "Connection";
    public const string Network = "Network";
    public const string Usb = "USB";
    public const string UsbConsole = "Nintendo Switch (USB)";
    public const string UsbNativeConsole = "Nintendo Switch (USB, Native)";
    public const string NoUsbConsole = "No console on USB. Connect the Switch with a USB-C cable (undocked).";
    public const string DriverPrompt = "NXStream needs a USB driver to detect the Switch over USB. Install it now?";
    public const string DriverDeclined = "USB driver not installed: the Switch can't be detected over USB.";
    public const string DriverWorking = "Working on the USB driver…";
    public const string DriverFailed = "The USB driver change failed. Details are in the log.";
    public const string LibraryMissing = "The USB library (libusb) is missing, USB is unavailable.";
    public const string NoAnswer = "The Switch did not answer over USB. Check that the NXStream module is running.";
    public const string InstallButton = "Install";
    public const string RepairButton = "Repair";
    public const string UninstallButton = "Uninstall";
    public const string UninstallConfirmWindows = "This also removes the Android USB driver that other apps may use. Continue?";
    public const string UninstallConfirmLinux = "Remove the USB access rule?";

    public static string ConsoleName(string? serial)
    {
        var name = serial?.Trim();
        return string.IsNullOrEmpty(name) || name == Wire.UsbProductName ? UsbConsole : name;
    }

    public static string OpenFailed(string errorName) =>
        $"Can't open the USB device ({errorName}). Reconnect the cable and try again.";

    public static string StatusText(UsbDriverStatus status) => status switch
    {
        UsbDriverStatus.Ready => "USB driver: installed",
        UsbDriverStatus.NotInstalled => "USB driver: not installed",
        UsbDriverStatus.NoPermission => "USB access: no permission",
        _ => "USB driver: unknown",
    };
}
