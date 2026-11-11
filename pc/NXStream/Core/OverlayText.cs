using System;

namespace NXStream.Core;

public static class OverlayText
{
    public static string[] Lines(bool showStats, bool showHint, ViewerSession? session, ViewerStats? stats)
    {
        var lines = new System.Collections.Generic.List<string>();
        if (stats is { Probing: true }) lines.Add("Testing network…");
        if (showStats)
        {
            if (session is not null)
                lines.Add($"NXStream   {session.Console.Name} · firmware {session.Console.Firmware.ToString(3)} · {session.TransportName}");
            else
                lines.Add("NXStream   waiting for the Switch");
            if (stats is { } s)
            {
                lines.Add("Source    " + s.Mode switch
                {
                    StreamMode.Game => "game · H.264 1280×720 from the recorder",
                    StreamMode.Screen => "HOME / system · JPEG 1280×720 from screenshots",
                    _ => "waiting for the video",
                });
                lines.Add(s.Mode == StreamMode.Game
                    ? $"Video     {s.Fps,5:0.0} fps shown · {s.Received:0.0} received · {s.Keyframes:0.0} keyframe/s · " +
                      $"{s.FrameKb:0} KB/frame"
                    : $"Video     {s.Fps,5:0.0} fps shown");
                lines.Add($"Network   {s.Mbps,5:0.0} Mbit/s · ping {Ms(s.RttMs)} (min {Ms(s.RttMin)} · max {Ms(s.RttMax)})");
                lines.Add($"Decoder   {s.DecodeErrors} errors/s · {s.Restarts} restarts");
                lines.Add($"Image     {s.Render.Scaler} · {s.Render.Size} · {s.Render.DrawMs:0.0} ms");
                if (s.Render.Gpu.Length > 0) lines.Add($"GPU       {s.Render.Gpu}");
                lines.Add(s.Audio
                    ? $"Audio     buffer {s.AudioBufferMs:0} ms · target {s.AudioTargetMs} ms · {s.AudioUnderruns} underruns · {s.AudioTrims} trims"
                    : "Audio     none (silence or audio off on the console)");
                lines.Add($"Session   {s.Uptime:hh\\:mm\\:ss} · {s.TotalMb:0} MB received");
                if (s.Render.Problem is not null) lines.Add("! " + s.Render.Problem);
                if (s.DecodeErrors > 0) lines.Add("! damaged video: game data was lost (Wi-Fi?)");
            }
            if (session?.AudioWarning is { } warning) lines.Add("! " + warning);
        }
        else if (showHint && stats is not { Probing: true })
        {
            lines.Add("NXStream");
            lines.Add("F11 full screen · F3 statistics · right click: menu and options");
        }
        return lines.ToArray();
    }

    private static string Ms(double value) => value < 0 ? "-" : $"{value:0} ms";
}
