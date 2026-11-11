namespace NXStream.Core;

public static class UpdateText
{
    public const string Title = "Update available";
    public const string Update = "Update";
    public const string Cancel = "Cancel";
    public const string Verifying = "Verifying update…";
    public const string Restarting = "Restarting…";
    public const string ReasonDownload = "download error";
    public const string ReasonChecksum = "checksum mismatch";
    public const string ReasonWrite = "cannot write to the NXStream folder";

    public static string Body(string newVersion) =>
        $"NXStream {newVersion} is available.";

    public static string Downloading(int pct) => $"Downloading update… {pct}%";

    public static string Failed(string reason) => $"Update failed: {reason}.";
}
