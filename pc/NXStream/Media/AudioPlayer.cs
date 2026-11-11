using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using NAudio.Wave;
using NXStream.Core;

namespace NXStream.Media;

public sealed class AudioPlayer : IAudioSink
{
    private readonly JitterBuffer _buffer = new();
    private readonly object _lock = new();
    private readonly MMDeviceEnumerator? _enumerator;
    private readonly DefaultDeviceWatcher? _watcher;
    private WasapiOut? _output;
    private bool _disposed;
    private int _generation;

    public AudioPlayer()
    {
        _output = CreateOutput();
        try
        {
            using var devices = new MMDeviceEnumerator();
            using var device = devices.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            var mix = device.AudioClient.MixFormat;
            Log.Write("audio", $"WASAPI shared 30 ms, mix {mix.SampleRate} Hz {mix.Channels} ch {mix.BitsPerSample} bit, " +
                               $"adaptive jitter buffer 50-200 ms");
        }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException)
        {
            Log.Write("audio", $"WASAPI started, device details unavailable: {ex.GetType().Name}");
        }
        try
        {
            _enumerator = new MMDeviceEnumerator();
            _watcher = new DefaultDeviceWatcher(this);
            _enumerator.RegisterEndpointNotificationCallback(_watcher);
        }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException)
        {
            Log.Write("audio", $"device change notifications unavailable: {ex.GetType().Name}");
        }
    }

    public double BufferedMs => _buffer.BufferedMs;
    public long Underruns => _buffer.Underruns;
    public long Trims => _buffer.Trims;
    public bool AdaptationFrozen { set => _buffer.Frozen = value; }
    public int AudioTargetMs => _buffer.TargetMs;

    public float Volume
    {
        set => _buffer.Gain = Math.Clamp(value, 0f, 1f);
    }

    public void Push(ReadOnlySpan<byte> pcm) => _buffer.Write(pcm);

    private WasapiOut CreateOutput()
    {
        var output = new WasapiOut(AudioClientShareMode.Shared, true, 30);
        try
        {
            output.Init(new JitterProvider(_buffer));
            output.PlaybackStopped += OnPlaybackStopped;
            output.Play();
            return output;
        }
        catch
        {
            output.PlaybackStopped -= OnPlaybackStopped;
            output.Dispose();
            throw;
        }
    }

    private void OnPlaybackStopped(object? sender, StoppedEventArgs e)
    {
        lock (_lock)
        {
            if (_disposed || !ReferenceEquals(sender, _output)) return;
        }
        var code = e.Exception is null ? "none" : $"0x{e.Exception.HResult:X8}";
        Log.Write("audio", $"output lost ({code}), reopening");
        Schedule(500, true);
    }

    private void Schedule(int delayMs, bool announce)
    {
        int generation;
        lock (_lock)
        {
            if (_disposed) return;
            generation = ++_generation;
        }
        _ = Task.Run(async () =>
        {
            await Task.Delay(delayMs).ConfigureAwait(false);
            Recreate(generation, announce);
        });
    }

    private void Recreate(int generation, bool announce)
    {
        WasapiOut? old;
        lock (_lock)
        {
            if (_disposed || generation != _generation) return;
            old = _output;
            _output = null;
        }
        if (old != null)
        {
            old.PlaybackStopped -= OnPlaybackStopped;
            try { old.Stop(); }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
            try { old.Dispose(); }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
        }
        WasapiOut? created = null;
        try
        {
            lock (_lock)
            {
                if (_disposed || generation != _generation) return;
                created = CreateOutput();
                _output = created;
                created = null;
            }
            Log.Write("audio", "output reopened");
        }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException or ArgumentException)
        {
            created?.Dispose();
            if (announce) Log.Write("audio", $"no output device ({ex.GetType().Name}), retrying every 2 s");
            Schedule(2000, false);
        }
    }

    public void Dispose()
    {
        WasapiOut? output;
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            output = _output;
            _output = null;
        }
        if (_watcher != null)
        {
            try { _enumerator?.UnregisterEndpointNotificationCallback(_watcher); }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
        }
        _enumerator?.Dispose();
        if (output != null)
        {
            output.PlaybackStopped -= OnPlaybackStopped;
            try { output.Stop(); }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
            output.Dispose();
        }
        Log.Write("audio", $"stopped: {_buffer.Underruns} underruns, {_buffer.Trims} trims ({_buffer.TrimmedMs} ms dropped)");
    }

    private sealed class DefaultDeviceWatcher(AudioPlayer owner) : IMMNotificationClient
    {
        public void OnDeviceStateChanged(string deviceId, DeviceState newState) { }
        public void OnDeviceAdded(string pwstrDeviceId) { }
        public void OnDeviceRemoved(string deviceId) { }
        public void OnPropertyValueChanged(string pwstrDeviceId, PropertyKey key) { }

        public void OnDefaultDeviceChanged(DataFlow flow, Role role, string defaultDeviceId)
        {
            if (flow != DataFlow.Render || role != Role.Multimedia) return;
            Log.Write("audio", "default output changed, reopening");
            owner.Schedule(300, false);
        }
    }

    private sealed class JitterProvider(JitterBuffer buffer) : IWaveProvider
    {
        public WaveFormat WaveFormat { get; } = new(48000, 16, 2);
        public int Read(byte[] data, int offset, int count) => buffer.Read(data, offset, count);
    }
}
