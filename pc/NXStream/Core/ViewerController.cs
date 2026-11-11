using System;
using System.Net;
using NXStream.Media;
using NXStream.Net;

namespace NXStream.Core;

public interface IViewerHost
{
    void Post(Action action);
    void Close();
    void ShowMessage(string title, string body);
    void ShowOverlay(string[] lines);
    void ApplyKeepAwake(bool on);
}

public sealed class ViewerController
{
    private readonly IViewerHost _host;
    private readonly IVideoSurface _surface;
    private readonly IMediaFactory _media;
    private readonly AppSettings _settings;
    private readonly string _ffmpegPath;
    private readonly string _consoleName;
    private IPEndPoint _endpoint;
    private readonly DiscordPresence _presence = new();
    private ViewerSession? _session;
    private Reconnector? _reconnect;
    private ViewerStats? _stats;
    private bool _showHint = true;
    private bool _closing;
    private volatile bool _infoSeen;
    private System.Threading.Timer? _infoTimer;

    public string? EndReason { get; private set; }

    public ViewerController(IViewerHost host, IVideoSurface surface, IMediaFactory media, AppSettings settings,
                            ViewerSession session, IPEndPoint endpoint, string ffmpegPath)
    {
        _host = host;
        _surface = surface;
        _media = media;
        _settings = settings;
        _endpoint = endpoint;
        _ffmpegPath = ffmpegPath;
        _consoleName = session.Console.Name;
        _presence.SetEnabled(settings.DiscordPresence);
        Attach(session);
    }

    public void Start()
    {
        RefreshOverlay();
        _host.ApplyKeepAwake(_settings.KeepAwake);
        try
        {
            _session?.Start(_surface);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("ui", "session start failed", ex);
            EndReason = ex.Message;
            _host.Close();
        }
    }

    public void Stop()
    {
        if (_closing) return;
        _closing = true;
        _reconnect?.Cancel();
        _session?.Stop();
        _infoTimer?.Dispose();
        _presence.Clear();
        _presence.Dispose();
        _host.ApplyKeepAwake(false);
    }

    public void HideHint()
    {
        _showHint = false;
        RefreshOverlay();
    }

    public void ToggleStats()
    {
        _settings.ShowStats = !_settings.ShowStats;
        Log.Write("ui", $"stats overlay {(_settings.ShowStats ? "on" : "off")}");
        _settings.Save();
        RefreshOverlay();
    }

    public void ApplySettings()
    {
        if (_session is { } session)
        {
            session.Render = _settings.Render;
            session.Volume = _settings.EffectiveVolume;
        }
        _presence.SetEnabled(_settings.DiscordPresence);
        _host.ApplyKeepAwake(_settings.KeepAwake);
        RefreshOverlay();
    }

    public void RefreshOverlay()
    {
        if (_closing) return;
        _host.ShowOverlay(OverlayText.Lines(_settings.ShowStats, _showHint, _session, _stats));
    }

    private void Attach(ViewerSession session)
    {
        _session = session;
        session.AppChanged += OnAppInfo;
        _infoSeen = false;
        _infoTimer?.Dispose();
        _infoTimer = null;
        if (_settings.DiscordPresence)
        {
            Log.Write("discord", "enabled, waiting for game info from the console");
            _infoTimer = new System.Threading.Timer(_ =>
            {
                if (!_closing && _session == session && !_infoSeen && _settings.DiscordPresence)
                    Log.Write("discord", "the console sent no game info, update NXStream on the Switch");
            }, null, 10000, System.Threading.Timeout.Infinite);
        }
        session.StatsUpdated += stats => _host.Post(() =>
        {
            if (_closing || _session != session) return;
            _stats = stats;
            RefreshOverlay();
        });
        session.Ended += reason => _host.Post(() => OnEnded(session, reason));
    }

    private void OnAppInfo(AppInfo info)
    {
        _infoSeen = true;
        _presence.Update(info);
    }

    private void OnEnded(ViewerSession session, string? reason)
    {
        if (_closing || session != _session) return;
        session.AppChanged -= OnAppInfo;
        _session = null;
        _stats = null;
        if (reason is null)
        {
            _host.Close();
            return;
        }
        if (!_settings.AutoReconnect)
        {
            EndReason = reason;
            _host.Close();
            return;
        }
        _presence.SessionLost();
        Log.Write("ui", $"connection lost ({reason}), waiting for the console");
        _host.ShowMessage(ViewerText.WaitingTitle, ViewerText.WaitingBody(reason));
        RefreshOverlay();
        _reconnect = new Reconnector(_consoleName, _endpoint,
            (candidate, ct) => ViewerSession.ConnectAsync(candidate, _ffmpegPath, _settings.EffectiveVolume,
                                                          _settings.Render, _media, ct),
            (next, candidate) => _host.Post(() => Resume(next, candidate)));
        _reconnect.Start();
    }

    private void Resume(ViewerSession next, IPEndPoint candidate)
    {
        if (_closing)
        {
            next.Stop();
            return;
        }
        try
        {
            _endpoint = candidate;
            Attach(next);
            next.Start(_surface);
            RefreshOverlay();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("ui", "starting the new session failed", ex);
            _session = next;
            OnEnded(next, ViewerText.StartFailed);
            next.Stop();
        }
    }
}
