using System;
using System.Diagnostics;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using NXStream.Media;
using NXStream.Net;
using NXStream.Protocol;
using NXStream.Render;

namespace NXStream.Core;

public enum StreamMode { Waiting, Game, Screen }

public sealed record ViewerStats(StreamMode Mode, double Fps, double Received, double Keyframes, double FrameKb, int DecodeErrors,
                                 double Mbps, double RttMs, double RttMin, double RttMax, bool Audio, double AudioBufferMs,
                                 long AudioUnderruns, long AudioTrims, TimeSpan Uptime, double TotalMb, int Restarts, RenderInfo Render,
                                 int AudioTargetMs = 0, bool Probing = false);

public sealed class ViewerSession
{
    private static readonly TimeSpan StallLimit = TimeSpan.FromSeconds(3);
    private const int MaxRestarts = 5;
    private const long RestartResetMs = 60000;
    private const int LogStatsEverySeconds = 5;

    private readonly IConsoleConnection _connection;
    private readonly string _ffmpegPath;
    private readonly IAudioSink? _audio;
    private readonly System.Threading.Timer _statsTimer;
    private readonly object _decoderLock = new();
    private readonly H264ParameterCache _parameters = new();
    private readonly IJpegDecoder _jpeg;
    private readonly Stopwatch _clock = Stopwatch.StartNew();

    private IVideoSurface? _surface;
    private H264Decoder? _decoder;
    private bool _waitKeyframe = true;
    private int _restarts;
    private long _lastFailureMs = long.MinValue / 2;
    private RenderSettings _render;
    private long _videoBytes;
    private volatile StreamMode _mode = StreamMode.Waiting;
    private int _ended;
    private long _frames;
    private long _received;
    private long _keyframes;
    private long _jpegFailures;
    private long _lastBytes;
    private long _lastAudioTick;
    private long _audioBytes;
    private int _ticks;
    private bool _probing;
    private long _probeBaseCount, _probeBaseAcks, _probeBaseBytes;
    private long _lastRttLogMs = -5000;
    private double _modeSince;
    private DateTime _lastStats = DateTime.UtcNow;

    public ConsoleInfo Console => _connection.Console;
    public string TransportName => _connection.TransportName;
    public string? AudioWarning { get; }

    public event Action<ViewerStats>? StatsUpdated;
    public event Action<string?>? Ended;
    public event Action<AppInfo>? AppChanged;

    private ViewerSession(IConsoleConnection connection, string ffmpegPath, float volume, RenderSettings render, IMediaFactory media)
    {
        _connection = connection;
        _ffmpegPath = ffmpegPath;
        _render = render;
        try
        {
            _audio = media.CreateAudio(volume);
        }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException)
        {
            AudioWarning = "Audio unavailable: " + ex.Message;
            Log.Write("audio", $"no audio output: {ex.GetType().Name}");
        }
        _jpeg = media.CreateJpeg();
        _statsTimer = new System.Threading.Timer(_ => Tick(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public static async Task<ViewerSession> ConnectAsync(IPEndPoint target, string ffmpegPath, float volume, RenderSettings render,
                                                         IMediaFactory media, CancellationToken ct)
    {
        IConsoleConnection connection;
        if (UsbTarget.IsNative(target)) connection = await NativeConnection.ConnectUsbAsync(ct);
        else if (UsbTarget.Is(target)) connection = await SwitchConnection.ConnectUsbAsync(ct);
        else if (target.Port == Wire.NativeTcpPort) connection = await NativeConnection.ConnectAsync(target, ct);
        else connection = await SwitchConnection.ConnectAsync(target, ct);
        try
        {
            return new ViewerSession(connection, ffmpegPath, volume, render, media);
        }
        catch
        {
            connection.Close();
            throw;
        }
    }

    public void Start(IVideoSurface surface)
    {
        _surface = surface;
        ApplyRenderSettings();
        lock (_decoderLock) _decoder = CreateDecoder();

        _connection.VideoH264 += OnVideoH264;
        _connection.VideoJpeg += (data, _) => OnVideoJpeg(data);
        _connection.Audio += (data, _) =>
        {
            _audio?.Push(data);
            Interlocked.Add(ref _audioBytes, data.Length);
            if (Interlocked.Exchange(ref _lastAudioTick, Environment.TickCount64) == 0)
                Log.Write("audio", $"first audio packet: {data.Length} bytes = {data.Length / 192.0:0.0} ms of 48 kHz s16 stereo");
        };
        _connection.Failed += End;
        _connection.AppInfoChanged += OnAppInfo;

        _connection.Start();
        _statsTimer.Change(1000, 1000);
        Log.Write("session", $"started, render {_render}");
    }

    public RenderSettings Render
    {
        get => _render;
        set
        {
            if (value == _render) return;
            _render = value;
            Log.Write("session", $"render {value}");
            ApplyRenderSettings();
        }
    }

    private void ApplyRenderSettings() => _surface?.Configure(_render);

    private void SetMode(StreamMode mode)
    {
        var previous = _mode;
        if (previous == mode) return;
        _mode = mode;
        var now = _clock.Elapsed.TotalMilliseconds;
        Log.Write("session", $"mode {previous} -> {mode} after {(now - _modeSince) / 1000:0.0} s");
        _modeSince = now;
    }

    private H264Decoder CreateDecoder(int width = H264Decoder.DefaultWidth, int height = H264Decoder.DefaultHeight)
    {
        var decoder = new H264Decoder(_ffmpegPath, width, height);
        decoder.FrameDecoded += frame =>
        {
            if (_mode != StreamMode.Game) return;
            _surface?.PresentFrame(frame, decoder.Width, decoder.Height);
            Interlocked.Increment(ref _frames);
        };
        decoder.Failed += reason => RestartDecoder(decoder, reason);
        return decoder;
    }

    private void RestartDecoder(H264Decoder failed, string reason)
    {
        if (Volatile.Read(ref _ended) != 0) return;
        H264Decoder? fresh = null;
        var outcome = "giving up";
        lock (_decoderLock)
        {
            if (_decoder != failed) return;
            var now = _clock.ElapsedMilliseconds;
            if (now - _lastFailureMs >= RestartResetMs) _restarts = 0;
            _lastFailureMs = now;
            if (++_restarts <= MaxRestarts)
            {
                try
                {
                    fresh = CreateDecoder(failed.Width, failed.Height);
                    _decoder = fresh;
                    _waitKeyframe = true;
                    outcome = $"restart {_restarts}/{MaxRestarts}";
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    _decoder = null;
                    outcome = $"restart {_restarts}/{MaxRestarts} failed: " + (ex is System.ComponentModel.Win32Exception w
                        ? $"win32 error {w.NativeErrorCode}" : ex.GetType().Name);
                }
            }
            else
            {
                _decoder = null;
            }
        }
        Log.Write("video", $"decoder pid {failed.ProcessId} failed ({reason}), {outcome}");
        Task.Run(failed.Dispose);
        if (fresh is null)
        {
            End(reason);
            return;
        }
        _connection.RequestKeyframe();
    }

    private void OnVideoH264(ReadOnlySpan<byte> data, bool keyframe)
    {
        SetMode(StreamMode.Game);
        Interlocked.Increment(ref _received);
        Interlocked.Add(ref _videoBytes, data.Length);
        if (keyframe) Interlocked.Increment(ref _keyframes);
        _parameters.Update(data);
        H264Decoder? decoder;
        H264Decoder? replaced = null;
        var primeParameters = false;
        lock (_decoderLock)
        {
            decoder = _decoder;
            if (decoder is null) return;
            if (keyframe && H264Sps.TryGetSize(data, out var width, out var height) &&
                (width != decoder.Width || height != decoder.Height))
            {
                try
                {
                    var fresh = CreateDecoder(width, height);
                    Log.Write("video", $"stream size {decoder.Width}x{decoder.Height} -> {width}x{height}, decoder restarted");
                    replaced = decoder;
                    decoder = _decoder = fresh;
                    _waitKeyframe = false;
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    Log.Write("video", $"decoder restart for {width}x{height} failed: {ex.GetType().Name}");
                }
            }
            if (_waitKeyframe)
            {
                if (!keyframe) return;
                primeParameters = !_parameters.Contains(data);
                _waitKeyframe = false;
                Log.Write("video", $"keyframe {data.Length} bytes reaches decoder pid {decoder.ProcessId}" +
                                   (primeParameters ? $", SPS/PPS replayed ({_parameters.AnnexB.Length} bytes)" : ""));
            }
        }
        if (replaced is not null) Task.Run(replaced.Dispose);
        if (primeParameters) decoder.Push(_parameters.AnnexB);
        decoder.Push(data);
    }

    private void OnVideoJpeg(ReadOnlySpan<byte> data)
    {
        SetMode(StreamMode.Screen);
        if (!_jpeg.Decode(data))
        {
            if (Interlocked.Increment(ref _jpegFailures) <= 5) Log.Write("video", $"corrupt JPEG ({data.Length} bytes) skipped");
            return;
        }
        _surface?.PresentFrame(_jpeg.Pixels, _jpeg.Width, _jpeg.Height);
        Interlocked.Increment(ref _frames);
    }

    public float Volume
    {
        set { if (_audio is not null) _audio.Volume = value; }
    }

    private void Tick()
    {
        try
        {
            H264Decoder? decoder;
            lock (_decoderLock) decoder = _decoder;
            UpdateProbe();
            if (!_probing && decoder is not null && decoder.Stalled(StallLimit)) RestartDecoder(decoder, "The video decoder (ffmpeg) is not responding.");
            ReportStats(decoder?.TakeErrors() ?? 0);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("stats", "statistics tick failed", ex);
        }
    }

    private void UpdateProbe()
    {
        var active = _connection.ProbeActive;
        if (active == _probing) return;
        _probing = active;
        if (_audio is not null) _audio.AdaptationFrozen = active;
        if (active)
        {
            _probeBaseCount = _connection.ProbeBaseCount;
            _probeBaseAcks = _connection.ProbeBaseAckCount;
            _probeBaseBytes = _connection.ProbeBaseBytes;
            Log.Write("session", "probe started");
            return;
        }
        Log.Write("session", $"probe ended: {_connection.ProbeCount - _probeBaseCount} probes, " +
                             $"{_connection.ProbeAckCount - _probeBaseAcks} acks, " +
                             $"{(_connection.ProbeBytes - _probeBaseBytes) / 1048576.0:0.0} MB");
    }

    private void ReportStats(int decodeErrors)
    {
        var now = DateTime.UtcNow;
        var seconds = Math.Max(0.001, (now - _lastStats).TotalSeconds);
        var bytes = _connection.BytesReceived;
        var received = Interlocked.Exchange(ref _received, 0);
        var videoBytes = Interlocked.Exchange(ref _videoBytes, 0);
        var keyframes = Interlocked.Exchange(ref _keyframes, 0);
        var audioBytes = Interlocked.Exchange(ref _audioBytes, 0);
        var stats = new ViewerStats(
            _mode,
            Interlocked.Exchange(ref _frames, 0) / seconds,
            received / seconds,
            keyframes / seconds,
            received > 0 ? videoBytes / 1024.0 / received : 0,
            decodeErrors,
            (bytes - _lastBytes) * 8 / seconds / 1e6,
            _connection.RttMs,
            _connection.RttMin,
            _connection.RttMax,
            Environment.TickCount64 - Interlocked.Read(ref _lastAudioTick) < 1500,
            _audio?.BufferedMs ?? 0,
            _audio?.Underruns ?? 0,
            _audio?.Trims ?? 0,
            _connection.Uptime,
            bytes / 1048576.0,
            _restarts,
            _surface?.Info ?? new RenderInfo("", "", "", 0, null),
            _audio?.AudioTargetMs ?? 0,
            _probing);
        _lastStats = now;
        _lastBytes = bytes;
        StatsUpdated?.Invoke(stats);

        var slowRtt = stats.RttMs > 150 && _clock.ElapsedMilliseconds - _lastRttLogMs >= 5000;
        if (slowRtt) _lastRttLogMs = _clock.ElapsedMilliseconds;
        if (++_ticks % LogStatsEverySeconds == 0 || decodeErrors > 0 || slowRtt)
            Log.Write("stats", $"{stats.Mode} shown {stats.Fps:0.0} fps, received {stats.Received:0.0} fps, " +
                               $"{keyframes} keyframes, frame {stats.FrameKb:0.0} KB, {stats.Mbps:0.00} Mbit/s, " +
                               $"rtt {stats.RttMs:0.0} ms, decode errors {decodeErrors}, " +
                               $"audio {audioBytes / seconds / 1000:0.0} kB/s buffered {stats.AudioBufferMs:0} ms target {stats.AudioTargetMs} ms | " +
                               $"{stats.Render.Scaler} {stats.Render.Size} draw {stats.Render.DrawMs:0.0} ms" +
                               (stats.Render.Problem is null ? "" : $" | {stats.Render.Problem}"));
    }

    public void Stop() => End(null);

    private void OnAppInfo(AppInfo info)
    {
        if (Volatile.Read(ref _ended) == 0) AppChanged?.Invoke(info);
    }

    private void End(string? reason)
    {
        if (Interlocked.Exchange(ref _ended, 1) != 0) return;
        Log.Write("session", $"ending: {reason ?? "closed by the user"} after {_clock.Elapsed:hh\\:mm\\:ss}, " +
                             $"{_restarts} decoder restarts, {Interlocked.Read(ref _jpegFailures)} bad JPEGs");
        _statsTimer.Dispose();
        _connection.Close();
        H264Decoder? decoder;
        lock (_decoderLock)
        {
            decoder = _decoder;
            _decoder = null;
        }
        decoder?.Dispose();
        _audio?.Dispose();
        Ended?.Invoke(reason);
    }
}

public interface IVideoSurface
{
    void PresentFrame(ReadOnlySpan<byte> bgra, int width, int height);
    void Configure(RenderSettings settings);
    RenderInfo Info { get; }
}
