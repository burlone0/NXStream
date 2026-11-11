using System;
using System.Linq;
using System.Reflection;

namespace NXStream.Core;

public static class BuildInfo
{
    private static readonly Assembly Entry = Assembly.GetEntryAssembly() ?? typeof(BuildInfo).Assembly;

    public static string? ContentId { get; } = ReadContentId();

    public static string Version { get; } = ReadVersion();

    public static string Component => OperatingSystem.IsWindows() ? "windows" : "linux";

    private static string? ReadContentId()
    {
        var value = Entry.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "NXStreamContentId")?.Value;
        return string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    }

    private static string ReadVersion()
    {
        var raw = Entry.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
                  ?? Entry.GetName().Version?.ToString()
                  ?? "0.0.0";
        var plus = raw.IndexOf('+');
        if (plus >= 0) raw = raw[..plus];
        return raw.Length == 0 ? "0.0.0" : raw;
    }
}
