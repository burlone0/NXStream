using System;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;

namespace NXStream.Core;

public static class AppStartup
{
    public static Stopwatch Begin(string[] args)
    {
        Log.Open();
        var version = Assembly.GetEntryAssembly()?.GetName().Version?.ToString(3) ?? "?";
        Log.Write("app", $"NXStream {version} started, {args.Length} args");
        Log.Write("app", $"pid {Environment.ProcessId}");
        Log.Write("app", $"{RuntimeInformation.OSDescription} {RuntimeInformation.OSArchitecture}, " +
                         $"{RuntimeInformation.FrameworkDescription}, {Environment.ProcessorCount} logical CPUs, " +
                         $"culture {CultureInfo.CurrentCulture.Name}");
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex) Log.Error("app", "unhandled exception (fatal)", ex);
        };
        return Stopwatch.StartNew();
    }

    public static void End(Stopwatch uptime)
    {
        Log.Write("app", $"exit after {uptime.Elapsed:hh\\:mm\\:ss}");
        Log.Close();
    }
}
