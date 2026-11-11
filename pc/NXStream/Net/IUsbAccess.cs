namespace NXStream.Net;

public enum UsbDriverStatus { Ready, NotInstalled, NoPermission, Unknown }

public interface IUsbAccess
{
    UsbDriverStatus GetStatus();
    Task<bool> InstallAsync();
    Task<bool> RepairAsync();
    Task<bool> UninstallAsync();
}

public static class UsbPlatform
{
    public static IUsbAccess? Access { get; set; }
    public static Func<string, bool>? AskInstall { get; set; }
}
