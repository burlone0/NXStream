using System.Text;
using System.Text.Json;
using NXStream.Net;

namespace NXStream.Core;

public sealed class DiscordPresence : IDisposable
{
    public const string ClientId = "1571022929788145684";

    private const ulong HomebrewTid = ulong.MaxValue;
    private const long MinSendMs = 4000;
    private const long RetryMs = 5000;
    private const long SessionLostMs = 60000;
    private const int ReadyTimeoutMs = 5000;

    private sealed class Link
    {
        public DiscordIpc Ipc = null!;
        public readonly ManualResetEventSlim Ready = new(false);
        public volatile bool Dead;
    }

    private readonly object gate = new();
    private readonly Thread worker;
    private bool enabled;
    private bool disposed;
    private CancellationTokenSource cts = new();
    private AppInfo? desired;
    private long? desiredStart;
    private long? keptStart;
    private ulong keptTid;
    private long sessionLostAt = -1;

    private readonly Dictionary<ulong, string?> iconCache = new();
    private Link? link;
    private string? sentKey;
    private long lastSendAt = long.MinValue / 2;
    private long nextConnectAt;
    private bool unavailableLogged;

    public DiscordPresence()
    {
        worker = new Thread(Loop) { IsBackground = true, Name = "discord-presence" };
        worker.Start();
    }

    public static string ActivityJson(AppInfo? info, long? startUnix, int pid, string nonce, string? homebrewIcon = null)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms))
        {
            w.WriteStartObject();
            w.WriteString("cmd", "SET_ACTIVITY");
            w.WriteStartObject("args");
            w.WriteNumber("pid", pid);
            if (info == null)
            {
                w.WriteNull("activity");
            }
            else
            {
                w.WriteStartObject("activity");
                w.WriteNumber("type", 0);
                w.WriteStartArray("buttons");
                w.WriteStartObject();
                w.WriteString("label", "GitHub");
                w.WriteString("url", "https://github.com/burlone0/NXStream");
                w.WriteEndObject();
                w.WriteEndArray();
                if (info.State is AppState.Running or AppState.Homebrew)
                {
                    var retail = info.State == AppState.Running && PresenceMath.IsRetailBase(info.ProgramId);
                    var name = PresenceMath.ClampName(info.Name) ?? (retail ? "Playing a game" : "Homebrew");
                    w.WriteString("name", name);
                    w.WriteString("state", retail ? "Playing on Switch using NXStream" : "Using homebrew apps on Switch");
                    w.WriteNumber("status_display_type", 0);
                    if (startUnix.HasValue)
                    {
                        w.WriteStartObject("timestamps");
                        w.WriteNumber("start", startUnix.Value);
                        w.WriteEndObject();
                    }
                    w.WriteStartObject("assets");
                    w.WriteString("large_image", retail ? PresenceMath.LargeImage(info.ProgramId) : info.State == AppState.Running ? homebrewIcon ?? "homebrew" : "homebrew");
                    w.WriteString("large_text", name);
                    w.WriteString("small_image", "nxstream");
                    w.WriteString("small_text", "NXStream");
                    w.WriteEndObject();
                }
                else
                {
                    w.WriteString("details", "In the HOME menu");
                    w.WriteNumber("status_display_type", 2);
                    w.WriteStartObject("assets");
                    w.WriteString("large_image", "nxstream");
                    w.WriteString("large_text", "NXStream");
                    w.WriteEndObject();
                }
                w.WriteEndObject();
            }
            w.WriteEndObject();
            w.WriteString("nonce", nonce);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(ms.ToArray());
    }

    public void SetEnabled(bool value)
    {
        lock (gate)
        {
            if (enabled == value) return;
            enabled = value;
            if (value) unavailableLogged = false;
            if (!value)
            {
                cts.Cancel();
                cts = new CancellationTokenSource();
            }
            Monitor.PulseAll(gate);
        }
    }

    public void Update(AppInfo info)
    {
        var now = DateTimeOffset.UtcNow;
        lock (gate)
        {
            if (disposed) return;
            sessionLostAt = -1;
            if (info.State is AppState.Running or AppState.Homebrew)
            {
                var tid = info.State == AppState.Homebrew ? HomebrewTid : info.ProgramId;
                if (tid != keptTid) keptStart = null;
                if (info.ElapsedSec.HasValue)
                {
                    keptStart = PresenceMath.StableStart(keptStart, keptTid, tid, PresenceMath.StartUnix(now, info.ElapsedSec.Value));
                }
                keptTid = tid;
                desiredStart = keptStart;
            }
            else
            {
                desiredStart = null;
            }
            desired = info;
            Monitor.PulseAll(gate);
        }
    }

    public void SessionLost()
    {
        lock (gate)
        {
            if (desired == null) return;
            sessionLostAt = Environment.TickCount64;
            Monitor.PulseAll(gate);
        }
    }

    public void Clear()
    {
        lock (gate)
        {
            desired = null;
            desiredStart = null;
            keptStart = null;
            keptTid = 0;
            sessionLostAt = -1;
            Monitor.PulseAll(gate);
        }
    }

    public void Dispose()
    {
        lock (gate)
        {
            if (disposed) return;
            disposed = true;
            desired = null;
            cts.Cancel();
            Monitor.PulseAll(gate);
        }
        if (Thread.CurrentThread != worker) worker.Join(1500);
    }

    private string? IconFor(AppInfo info) =>
        info.State == AppState.Running && iconCache.TryGetValue(info.ProgramId, out var icon) ? icon : null;

    private string Key(AppInfo info, long? start) =>
        info.State == AppState.Homebrew
            ? "b|" + start
            : info.State == AppState.Running
            ? "r|" + info.ProgramId + "|" + PresenceMath.ClampName(info.Name) + "|" + start + "|" + IconFor(info)
            : "h";

    private void Loop()
    {
        while (true)
        {
            AppInfo? info;
            long? start;
            bool on;
            bool stop;
            CancellationToken ct;
            lock (gate)
            {
                if (!disposed) Monitor.Wait(gate, 500);
                stop = disposed;
                if (sessionLostAt >= 0 && Environment.TickCount64 - sessionLostAt >= SessionLostMs)
                {
                    desired = null;
                    desiredStart = null;
                    keptStart = null;
                    keptTid = 0;
                    sessionLostAt = -1;
                }
                info = desired;
                start = desiredStart;
                on = enabled;
                ct = cts.Token;
            }

            try
            {
                if (stop || !on || info == null)
                {
                    if (!stop && link != null && !link.Dead && sentKey != null && !ClearAllowed()) continue;
                    CloseLink(true);
                    if (stop) return;
                    continue;
                }
                Step(info, start, ct);
            }
            catch (Exception ex)
            {
                Log.Error("discord", "worker error", ex);
                CloseLink(false);
                nextConnectAt = Environment.TickCount64 + RetryMs;
            }
        }
    }

    private void Step(AppInfo info, long? start, CancellationToken ct)
    {
        var now = Environment.TickCount64;
        if (link != null && link.Dead)
        {
            CloseLink(false);
            Log.Write("discord", "disconnected");
            nextConnectAt = now + RetryMs;
        }
        if (link == null)
        {
            if (now < nextConnectAt) return;
            if (!TryConnect(ct))
            {
                if (!ct.IsCancellationRequested) nextConnectAt = Environment.TickCount64 + RetryMs;
                return;
            }
        }
        if (info.State == AppState.Running && !PresenceMath.IsRetailBase(info.ProgramId) && !iconCache.ContainsKey(info.ProgramId))
        {
            var icon = HomebrewIcons.Lookup(info.Name, ct);
            if (ct.IsCancellationRequested) return;
            lock (gate) iconCache[info.ProgramId] = icon;
            Log.Write("discord", "homebrew icon " + (icon != null ? "found" : "not found") + " for tid=" + info.ProgramId.ToString("X16"));
        }
        var key = Key(info, start);
        if (key == sentKey) return;
        if (now - lastSendAt < MinSendMs) return;
        lock (gate)
        {
            if (disposed || !enabled || desired == null || Key(desired, desiredStart) != key) return;
        }
        var json = ActivityJson(info, start, Environment.ProcessId, Guid.NewGuid().ToString(), IconFor(info));
        if (!link!.Ipc.Send(DiscordIpc.OpFrame, json))
        {
            link.Dead = true;
            return;
        }
        lastSendAt = now;
        sentKey = key;
        Log.Write("discord", "presence set tid=" + info.ProgramId.ToString("X16"));
    }

    private bool TryConnect(CancellationToken ct)
    {
        var ipc = DiscordIpc.Connect(ct);
        if (ipc == null)
        {
            if (!unavailableLogged && !ct.IsCancellationRequested)
            {
                unavailableLogged = true;
                Log.Write("discord", "Discord is not running or not reachable");
            }
            return false;
        }
        var l = new Link { Ipc = ipc };
        var reader = new Thread(() => ReadLoop(l)) { IsBackground = true, Name = "discord-read" };
        reader.Start();
        bool ok;
        using (ct.Register(ipc.Dispose))
        {
            try
            {
                ok = ipc.Send(DiscordIpc.OpHandshake, DiscordIpc.HandshakeJson(ClientId)) && l.Ready.Wait(ReadyTimeoutMs, ct) && !l.Dead;
            }
            catch (OperationCanceledException)
            {
                ok = false;
            }
        }
        if (!ok)
        {
            ipc.Dispose();
            if (!unavailableLogged && !ct.IsCancellationRequested)
            {
                unavailableLogged = true;
                Log.Write("discord", "handshake failed");
            }
            return false;
        }
        link = l;
        sentKey = null;
        unavailableLogged = false;
        Log.Write("discord", "connected");
        return true;
    }

    private static void ReadLoop(Link l)
    {
        try
        {
            while (l.Ipc.Read(out var op, out var json))
            {
                if (op == DiscordIpc.OpPing)
                {
                    l.Ipc.Send(DiscordIpc.OpPong, json);
                }
                else if (op == DiscordIpc.OpClose)
                {
                    break;
                }
                else if (op == DiscordIpc.OpFrame && IsReady(json))
                {
                    l.Ready.Set();
                }
            }
        }
        catch (Exception ex) when (ex is ObjectDisposedException or NullReferenceException)
        {
        }
        l.Dead = true;
    }

    private static bool IsReady(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.ValueKind == JsonValueKind.Object
                && doc.RootElement.TryGetProperty("evt", out var evt)
                && evt.ValueKind == JsonValueKind.String
                && evt.GetString() == "READY";
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private bool ClearAllowed() => Environment.TickCount64 - lastSendAt >= MinSendMs;

    private void CloseLink(bool sendClear)
    {
        var l = link;
        link = null;
        if (l == null) return;
        if (sendClear && !l.Dead && sentKey != null && ClearAllowed())
        {
            l.Ipc.Send(DiscordIpc.OpFrame, ActivityJson(null, null, Environment.ProcessId, Guid.NewGuid().ToString()));
            lastSendAt = Environment.TickCount64;
            Log.Write("discord", "presence cleared");
        }
        sentKey = null;
        l.Ipc.Dispose();
        if (sendClear) Log.Write("discord", "closed");
    }
}
