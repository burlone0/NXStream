namespace NXStream.Net;

public delegate void MediaHandler(ReadOnlySpan<byte> payload, bool keyframe);

public interface IConsoleConnection
{
    ConsoleInfo Console { get; }
    string TransportName { get; }

    event MediaHandler? VideoH264;
    event MediaHandler? VideoJpeg;
    event MediaHandler? Audio;
    event Action<string>? Failed;
    event Action<AppInfo>? AppInfoChanged;

    long BytesReceived { get; }
    double RttMs { get; }
    double RttMin { get; }
    double RttMax { get; }
    TimeSpan Uptime { get; }
    bool ProbeActive { get; }
    long ProbeCount { get; }
    long ProbeAckCount { get; }
    long ProbeBytes { get; }
    long ProbeBaseCount { get; }
    long ProbeBaseAckCount { get; }
    long ProbeBaseBytes { get; }

    void Start();
    void RequestKeyframe();
    void Close();
}
