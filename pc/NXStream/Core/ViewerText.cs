namespace NXStream.Core;

public static class ViewerText
{
    public const string Fullscreen = "Full screen";
    public const string Stats = "Statistics";
    public const string Options = "Options…";
    public const string Disconnect = "Disconnect";
    public const string KeyFullscreen = "F11";
    public const string KeyStats = "F3";
    public const string WaitingVideo = "Waiting for the video…";
    public const string WaitingTitle = "Waiting for the Switch…";
    public const string StartFailed = "Could not start the session";
    public const int HintMs = 5000;

    public static string WaitingBody(string reason) =>
        $"{reason}\nThe connection resumes by itself as soon as the console responds.";
}
