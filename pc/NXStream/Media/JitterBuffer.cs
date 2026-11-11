using System;
using System.Runtime.InteropServices;
using NXStream.Core;

namespace NXStream.Media;

public sealed class JitterBuffer
{
    public const int BytesPerMs = 48000 * 4 / 1000;
    private const int MinTargetMs = 50;
    private const int MaxTargetMs = 200;
    private const long StableMs = 20000;

    private readonly byte[] _ring = new byte[BytesPerMs * 1000];
    private readonly object _sync = new();
    private int _start, _count;
    private bool _starving = true;
    private volatile float _gain = 1f;
    private readonly Func<long> _nowMs;
    private int _targetMs = MinTargetMs;
    private long _lastEventMs;

    public JitterBuffer() : this(() => Environment.TickCount64) { }

    public JitterBuffer(Func<long> nowMs)
    {
        _nowMs = nowMs;
        _lastEventMs = nowMs();
    }

    public bool Frozen { get; set; }

    public int TargetMs { get { lock (_sync) return _targetMs; } }
    public int MaxMs => TargetMs + 70;
    public int TrimToMs => TargetMs + 10;

    public double BufferedMs { get { lock (_sync) return _count / (double)BytesPerMs; } }
    public long Underruns { get; private set; }
    public long Trims { get; private set; }
    public long TrimmedMs { get; private set; }

    public float Gain
    {
        get => _gain;
        set => _gain = value;
    }

    public void Write(ReadOnlySpan<byte> pcm)
    {
        lock (_sync)
        {
            if (pcm.Length >= _ring.Length)
            {
                pcm = pcm.Slice(pcm.Length - _ring.Length);
                _start = 0;
                _count = 0;
            }
            var overflow = _count + pcm.Length - _ring.Length;
            if (overflow > 0)
            {
                _start = (_start + overflow) % _ring.Length;
                _count -= overflow;
            }
            var tail = (_start + _count) % _ring.Length;
            var first = Math.Min(pcm.Length, _ring.Length - tail);
            pcm.Slice(0, first).CopyTo(_ring.AsSpan(tail));
            pcm.Slice(first).CopyTo(_ring);
            _count += pcm.Length;
            var now = _nowMs();
            var maxMs = _targetMs + 70;
            if (_count > maxMs * BytesPerMs)
            {
                var drop = _count - (_targetMs + 10) * BytesPerMs;
                drop -= drop % 4;
                _start = (_start + drop) % _ring.Length;
                _count -= drop;
                Trims++;
                TrimmedMs += drop / BytesPerMs;
                _lastEventMs = now;
                if (Trims <= 10 || Trims % 50 == 0)
                    Log.Write("audio", $"latency above {maxMs} ms, dropped {drop / BytesPerMs} ms (trim #{Trims})");
            }
            else if (_targetMs > MinTargetMs && now - _lastEventMs >= StableMs)
            {
                var old = _targetMs;
                _targetMs = Math.Max(MinTargetMs, _targetMs - 10);
                _lastEventMs = now;
                Log.Write("audio", $"audio target {old} -> {_targetMs} ms after {StableMs / 1000} s stable");
            }
        }
    }

    private void ApplyGain(byte[] buffer, int offset, int count)
    {
        var gain = _gain;
        if (gain >= 0.999f) return;
        var samples = MemoryMarshal.Cast<byte, short>(buffer.AsSpan(offset, count - count % 2));
        for (var i = 0; i < samples.Length; i++) samples[i] = (short)(samples[i] * gain);
    }

    public int Read(byte[] buffer, int offset, int count)
    {
        lock (_sync)
        {
            if (_starving && _count < _targetMs * BytesPerMs)
            {
                Array.Clear(buffer, offset, count);
                return count;
            }
            _starving = false;
            var n = Math.Min(count, _count);
            var firstRead = Math.Min(n, _ring.Length - _start);
            Array.Copy(_ring, _start, buffer, offset, firstRead);
            Array.Copy(_ring, 0, buffer, offset + firstRead, n - firstRead);
            _start = (_start + n) % _ring.Length;
            _count -= n;
            ApplyGain(buffer, offset, n);
            if (n < count)
            {
                Array.Clear(buffer, offset + n, count - n);
                _starving = true;
                Underruns++;
                _lastEventMs = _nowMs();
                if (!Frozen)
                {
                    var old = _targetMs;
                    _targetMs = Math.Min(_targetMs * 3 / 2 + 10, MaxTargetMs);
                    if (_targetMs != old)
                        Log.Write("audio", $"audio target {old} -> {_targetMs} ms after underrun #{Underruns}");
                }
                if (Underruns <= 10 || Underruns % 50 == 0)
                    Log.Write("audio", $"underrun #{Underruns}: no audio from the console, waiting for {_targetMs} ms before playing again");
            }
            return count;
        }
    }
}
