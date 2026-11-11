using System.Diagnostics;
using NXStream.Core;

namespace NXStream.Media;

public sealed class H264Decoder : IDisposable
{
    public const int DefaultWidth = 1280;
    public const int DefaultHeight = 720;

    private const int MaxLoggedErrors = 40;
    private static readonly byte[] AccessUnitDelimiter = [0, 0, 0, 1, 0x09, 0xF0];

    private readonly Process _process;
    private readonly Stream _input;
    private readonly Thread _reader;
    private readonly object _writeLock = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private volatile bool _stopping;
    private long _waitingSince;
    private int _errors;
    private int _loggedErrors;
    private long _pushed, _decoded, _bytesIn;

    public event Action<byte[]>? FrameDecoded;
    public event Action<string>? Failed;

    public int ProcessId { get; }
    public int Width { get; }
    public int Height { get; }
    private int FrameBytes => Width * Height * 4;

    public H264Decoder(string ffmpegPath, int width = DefaultWidth, int height = DefaultHeight)
    {
        Width = width;
        Height = height;
        var psi = new ProcessStartInfo(ffmpegPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        string[] args =
        [
            "-hide_banner", "-loglevel", "error", "-nostdin",
            "-max_error_rate", "1",
            "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
            "-f", "h264", "-i", "pipe:0",
            "-vf", $"scale={Width}:{Height}", "-pix_fmt", "bgra",
            "-fps_mode", "passthrough", "-threads", "1", "-filter_threads", "1",
            "-f", "rawvideo", "pipe:1",
        ];
        foreach (var a in args) psi.ArgumentList.Add(a);

        _process = Process.Start(psi) ?? throw new InvalidOperationException("Could not start ffmpeg.");
        ProcessId = _process.Id;
        ChildProcessGuard.Attach(_process);
        _process.ErrorDataReceived += (_, e) => OnStderr(e.Data);
        _process.BeginErrorReadLine();
        _input = _process.StandardInput.BaseStream;
        Log.Write("video", $"decoder pid {ProcessId} started in {_clock.ElapsedMilliseconds} ms, {Width}x{Height}");

        _reader = new Thread(ReadLoop)
        {
            IsBackground = true,
            Name = "ffmpeg video output",
            Priority = ThreadPriority.AboveNormal,
        };
        _reader.Start();
    }

    private void OnStderr(string? line)
    {
        if (string.IsNullOrEmpty(line)) return;
        Interlocked.Increment(ref _errors);
        var logged = Interlocked.Increment(ref _loggedErrors);
        if (logged <= MaxLoggedErrors) Log.Write("ffmpeg", $"pid {ProcessId}: {line}");
        else if (logged == MaxLoggedErrors + 1) Log.Write("ffmpeg", $"pid {ProcessId}: more errors, only counted from now on");
    }

    public int TakeErrors() => Interlocked.Exchange(ref _errors, 0);

    public bool Stalled(TimeSpan limit)
    {
        var since = Interlocked.Read(ref _waitingSince);
        return since != 0 && Environment.TickCount64 - since > limit.TotalMilliseconds;
    }

    public void Push(ReadOnlySpan<byte> accessUnit)
    {
        if (_stopping) return;
        accessUnit = SkipLeadingDelimiter(accessUnit);
        Interlocked.CompareExchange(ref _waitingSince, Environment.TickCount64, 0);
        _pushed++;
        _bytesIn += accessUnit.Length;
        try
        {
            lock (_writeLock)
            {
                _input.Write(accessUnit);
                _input.Write(AccessUnitDelimiter);
                _input.Flush();
            }
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
        }
    }

    private static ReadOnlySpan<byte> SkipLeadingDelimiter(ReadOnlySpan<byte> au)
    {
        var sc = au.Length >= 5 && au[0] == 0 && au[1] == 0 && au[2] == 0 && au[3] == 1 ? 4
               : au.Length >= 4 && au[0] == 0 && au[1] == 0 && au[2] == 1 ? 3 : 0;
        if (sc == 0 || (au[sc] & 0x1F) != 9) return au;
        for (var i = sc + 1; i + 3 <= au.Length; i++)
            if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
                return au[(i > 0 && au[i - 1] == 0 ? i - 1 : i)..];
        return au;
    }

    private void ReadLoop()
    {
        var frame = new byte[FrameBytes];
        var output = _process.StandardOutput.BaseStream;
        try
        {
            while (true)
            {
                var filled = 0;
                while (filled < FrameBytes)
                {
                    var n = output.Read(frame, filled, FrameBytes - filled);
                    if (n == 0) throw new EndOfStreamException();
                    filled += n;
                }
                var waited = Interlocked.Exchange(ref _waitingSince, 0);
                if (++_decoded == 1)
                    Log.Write("video", $"decoder pid {ProcessId}: first frame {_clock.ElapsedMilliseconds} ms after start, " +
                                       $"{(waited == 0 ? 0 : Environment.TickCount64 - waited)} ms after its input");
                FrameDecoded?.Invoke(frame);
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            if (_stopping) return;
            if (ex is not (IOException or ObjectDisposedException)) Log.Error("video", $"decoder pid {ProcessId} output thread", ex);
            var code = "?";
            try { if (_process.WaitForExit(500)) code = _process.ExitCode.ToString(); }
            catch (InvalidOperationException) { }
            Log.Write("video", $"decoder pid {ProcessId} exited (code {code}) after {_decoded} frames");
            Failed?.Invoke("The video decoder (ffmpeg) exited.");
        }
    }

    public void Dispose()
    {
        _stopping = true;
        try { if (!_process.HasExited) _process.Kill(true); }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception) { }
        try { _input.Close(); }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException) { }
        if (Thread.CurrentThread != _reader) _reader.Join(TimeSpan.FromSeconds(1));
        Log.Write("video", $"decoder pid {ProcessId} stopped: {_pushed} access units ({_bytesIn / 1048576.0:0.0} MB) in, " +
                           $"{_decoded} frames out, {_clock.Elapsed:hh\\:mm\\:ss}");
        _process.Dispose();
    }
}
