using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using NXStream.Core;
using NXStream.Protocol;

namespace NXStream.Net;

public readonly record struct NativeHeader(ushort Version, ushort Flags, uint Width, uint Height, uint Stride, uint Length, uint Block, uint Kind)
{
    public static bool TryParse(ReadOnlySpan<byte> data, out NativeHeader header)
    {
        header = default;
        if (data.Length < Wire.NativeHeaderSize || BinaryPrimitives.ReadUInt32LittleEndian(data) != Wire.NativeMagic) return false;
        header = new NativeHeader(
            BinaryPrimitives.ReadUInt16LittleEndian(data[4..]),
            BinaryPrimitives.ReadUInt16LittleEndian(data[6..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[20..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[24..]),
            BinaryPrimitives.ReadUInt32LittleEndian(data[28..]));
        return true;
    }

    public bool IsInfo => (Flags & Wire.NativeFlagInfo) != 0;
    public bool IsStatus => !IsInfo && (Flags & Wire.NativeFlagStatus) != 0;
    public bool IsAudio => !IsInfo && !IsStatus && (Flags & Wire.NativeFlagAudio) != 0;
    public bool IsVideo => !IsInfo && !IsStatus && !IsAudio && (Flags & Wire.NativeFlagVideo) != 0;
    public bool IsKeyframe => (Flags & Wire.NativeFlagKeyframe) != 0;
}

public sealed class NativeConnection : IConsoleConnection
{
    private const int SilenceMs = 5000;
    private const int InfoWaitMs = 1000;
    private const string NoAnswer = "The native module did not answer. Check that it is running on the Switch.";

    private readonly ILinkStream _stream;
    private readonly object _writeLock = new();
    private readonly byte[] _readBuffer = new byte[1024 * 1024];
    private int _readStart, _readEnd;
    private readonly byte[] _firstHeader;
    private readonly Thread _receiver;
    private readonly System.Threading.Timer _watchdog;
    private readonly Stopwatch _uptime = Stopwatch.StartNew();
    private int _closed;
    private long _lastReceiveTick = Environment.TickCount64;
    private long _bytesReceived;
    private long _video, _videoBytes, _audio, _audioBytes, _status, _empty, _other, _skippedBytes, _ageUs, _ageMax;
    private int _lastReason = -1;

    private ConsoleInfo _console = new(NativeDiscovery.KnownName ?? NativeDiscovery.Label, new Version(0, 0, 0));
    private Task<(byte[] Header, byte[]? Body)>? _prefetch;
    public ConsoleInfo Console => Volatile.Read(ref _console);
    public string TransportName { get; }

    public event MediaHandler? VideoH264;
    public event MediaHandler? VideoJpeg { add { } remove { } }
    public event MediaHandler? Audio;
    public event Action<string>? Failed;
    public event Action<AppInfo>? AppInfoChanged;

    public long BytesReceived => Interlocked.Read(ref _bytesReceived);
    public double RttMs => -1;
    public double RttMin => -1;
    public double RttMax => -1;
    public TimeSpan Uptime => _uptime.Elapsed;
    public bool ProbeActive => false;
    public long ProbeCount => 0;
    public long ProbeAckCount => 0;
    public long ProbeBytes => 0;
    public long ProbeBaseCount => 0;
    public long ProbeBaseAckCount => 0;
    public long ProbeBaseBytes => 0;

    private NativeConnection(ILinkStream stream, byte[] firstHeader)
    {
        _stream = stream;
        _firstHeader = firstHeader;
        TransportName = stream.Name;
        _receiver = new Thread(ReceiveLoop)
        {
            IsBackground = true,
            Name = "NXStream native receiver",
            Priority = ThreadPriority.AboveNormal,
        };
        _watchdog = new System.Threading.Timer(_ => CheckSilence(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public static async Task<NativeConnection> ConnectAsync(IPEndPoint remote, CancellationToken ct)
    {
        var timer = Stopwatch.StartNew();
        var socket = await SwitchConnection.OpenSocketAsync(remote, ct);
        socket.ReceiveTimeout = 5000;
        var stream = new TcpLinkStream(socket);
        NativeConnection connection;
        try
        {
            using var reg = ct.Register(stream.Dispose);
            connection = await Task.Run(() => Handshake(stream));
            ct.ThrowIfCancellationRequested();
        }
        catch (Exception) when (ct.IsCancellationRequested)
        {
            stream.Dispose();
            throw new OperationCanceledException(ct);
        }
        socket.ReceiveTimeout = 0;
        Log.Write("native", $"handshake done in {timer.ElapsedMilliseconds} ms");
        return connection;
    }

    public static async Task<NativeConnection> ConnectUsbAsync(CancellationToken ct)
    {
        var timer = Stopwatch.StartNew();
        var stream = await Task.Run(() => UsbLinkStream.Open(Wire.NativeUsbProductId, Wire.NativeUsbProductName), ct);
        Log.Write("usb", $"native opened in {timer.ElapsedMilliseconds} ms");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        NativeConnection connection;
        try
        {
            using var reg = ct.Register(stream.Dispose);
            using var timeoutReg = timeout.Token.Register(stream.Dispose);
            connection = await Task.Run(() => Handshake(stream));
            ct.ThrowIfCancellationRequested();
        }
        catch (Exception) when (ct.IsCancellationRequested)
        {
            stream.Dispose();
            throw new OperationCanceledException(ct);
        }
        catch (EndOfStreamException) when (timeout.IsCancellationRequested)
        {
            Log.Write("usb", "native handshake timed out");
            throw new TimeoutException(NoAnswer);
        }
        Log.Write("native", $"handshake done in {timer.ElapsedMilliseconds} ms");
        return connection;
    }

    private static NativeConnection Handshake(ILinkStream stream)
    {
        try
        {
            var header = new byte[Wire.NativeHeaderSize];
            ReceiveExact(stream, header);
            if (!NativeHeader.TryParse(header, out var parsed))
                throw new InvalidDataException("Invalid response: not an NXStream Native console.");
            Log.Write("native", $"first header: protocol {parsed.Version}, flags 0x{parsed.Flags:x}, {parsed.Length} bytes");
            var connection = new NativeConnection(stream, header);
            if (parsed.Length == 0 && parsed.Flags == 0)
            {
                connection._prefetch = Task.Run(() => ReadInfoCandidate(stream));
                connection._prefetch.Wait(InfoWaitMs);
            }
            return connection;
        }
        catch (SocketException ex) when (ex.SocketErrorCode == SocketError.TimedOut)
        {
            stream.Dispose();
            throw new TimeoutException(NoAnswer);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    private static (byte[] Header, byte[]? Body) ReadInfoCandidate(ILinkStream stream)
    {
        var header = new byte[Wire.NativeHeaderSize];
        ReceiveExact(stream, header);
        if (!NativeHeader.TryParse(header, out var h) || !h.IsInfo || h.Length > Wire.NativeMaxInfoPayload) return (header, null);
        var body = new byte[h.Length];
        ReceiveExact(stream, body);
        return (header, body);
    }

    public static bool TryParseInfo(ReadOnlySpan<byte> body, out string? name, out Version? firmware)
    {
        name = null;
        firmware = null;
        if (body.Length > Wire.NativeMaxInfoPayload) return false;
        foreach (var line in Encoding.UTF8.GetString(body).Split('\n'))
        {
            var eq = line.IndexOf('=');
            if (eq <= 0) continue;
            var key = line[..eq].Trim();
            var value = line[(eq + 1)..].Trim('\r', ' ', '\0');
            if (key == "name")
            {
                value = new string(value.Where(c => !char.IsControl(c)).ToArray());
                if (value.Length > 0) name = value.Length > 64 ? value[..64] : value;
            }
            else if (key == "firmware" && Version.TryParse(value, out var v) && v.Major >= 0 && v.Minor >= 0) firmware = v.Build < 0 ? new Version(v.Major, v.Minor, 0) : new Version(v.Major, v.Minor, v.Build);
        }
        return name is not null || firmware is not null;
    }

    private void ApplyInfo(ReadOnlySpan<byte> body)
    {
        var app = body.Length <= Wire.NativeMaxInfoPayload ? AppInfo.FromNativeInfo(Encoding.UTF8.GetString(body)) : null;
        if (app is not null)
        {
            Log.Write("native", $"app info: state {app.State}, title {app.ProgramId:X16}");
            AppInfoChanged?.Invoke(app);
        }
        if (!TryParseInfo(body, out var name, out var firmware))
        {
            if (app is null) Interlocked.Increment(ref _other);
            return;
        }
        var current = Console;
        var updated = new ConsoleInfo(name ?? current.Name, firmware ?? current.Firmware);
        Volatile.Write(ref _console, updated);
        if (name is not null) NativeDiscovery.Remember(name);
        Log.Write("native", $"console info received, firmware {updated.Firmware.ToString(3)}");
    }

    private static void ReceiveExact(ILinkStream stream, Span<byte> buffer)
    {
        while (!buffer.IsEmpty)
        {
            var n = stream.Read(buffer);
            if (n <= 0) throw new EndOfStreamException();
            buffer = buffer[n..];
        }
    }

    public void Start()
    {
        Interlocked.Exchange(ref _lastReceiveTick, Environment.TickCount64);
        _receiver.Start();
        _watchdog.Change(1000, 1000);
        Write(Wire.NativeKeyframeRequest, "keyframe request");
    }

    private void ReadExact(Span<byte> destination)
    {
        while (!destination.IsEmpty)
        {
            if (_readStart == _readEnd)
            {
                _readStart = 0;
                _readEnd = _stream.Read(_readBuffer);
                if (_readEnd <= 0)
                {
                    _readEnd = 0;
                    throw new EndOfStreamException();
                }
                Interlocked.Exchange(ref _lastReceiveTick, Environment.TickCount64);
            }
            var n = Math.Min(destination.Length, _readEnd - _readStart);
            _readBuffer.AsSpan(_readStart, n).CopyTo(destination);
            _readStart += n;
            destination = destination[n..];
        }
    }

    private void ReceiveLoop()
    {
        var header = new byte[Wire.NativeHeaderSize];
        var payload = new byte[512 * 1024];
        var first = true;
        try
        {
            while (true)
            {
                if (first)
                {
                    _firstHeader.CopyTo(header, 0);
                    first = false;
                }
                else if (_prefetch is { } pending)
                {
                    _prefetch = null;
                    var (prefetchedHeader, prefetchedBody) = pending.GetAwaiter().GetResult();
                    Interlocked.Exchange(ref _lastReceiveTick, Environment.TickCount64);
                    prefetchedHeader.CopyTo(header, 0);
                    if (prefetchedBody is not null)
                    {
                        NativeHeader.TryParse(header, out var info);
                        Interlocked.Add(ref _bytesReceived, Wire.NativeHeaderSize + prefetchedBody.Length);
                        Dispatch(info, prefetchedBody);
                        continue;
                    }
                }
                else
                {
                    ReadExact(header);
                }
                NativeHeader h;
                while (!NativeHeader.TryParse(header, out h))
                {
                    if (Interlocked.Increment(ref _skippedBytes) == 1) Log.Write("native", "bad header magic, resynchronizing");
                    Buffer.BlockCopy(header, 1, header, 0, header.Length - 1);
                    ReadExact(header.AsSpan(header.Length - 1));
                }
                if (h.Length > Wire.NativeMaxPayload) throw new InvalidDataException($"bad payload size {h.Length}");
                var size = (int)h.Length;
                if (payload.Length < size)
                {
                    payload = new byte[Math.Max(size, payload.Length * 2)];
                    Log.Write("native", $"receive buffer grown to {payload.Length / 1024} KB");
                }
                ReadExact(payload.AsSpan(0, size));
                Interlocked.Add(ref _bytesReceived, Wire.NativeHeaderSize + size);
                Dispatch(h, payload.AsSpan(0, size));
            }
        }
        catch (EndOfStreamException)
        {
            Fail("The Switch closed the connection.");
        }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException or InvalidDataException)
        {
            if (Volatile.Read(ref _closed) == 0) Log.Write("native", $"receive error: {Describe(ex)}");
            Fail("Connection to the Switch lost.");
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("native", "unexpected error while receiving", ex);
            Fail("NXStream internal error (details in the log).");
        }
    }

    private void Dispatch(NativeHeader h, ReadOnlySpan<byte> body)
    {
        if (h.IsInfo)
        {
            ApplyInfo(body);
        }
        else if (h.IsStatus)
        {
            Interlocked.Increment(ref _status);
            if (body.Length >= 8)
            {
                var reason = (int)BinaryPrimitives.ReadUInt32LittleEndian(body[4..]);
                if (reason != _lastReason)
                {
                    _lastReason = reason;
                    var head = body.Length >= 72 ? Encoding.UTF8.GetString(body.Slice(8, 64)).TrimEnd('\0') : "";
                    Log.Write("native", $"console status {reason}: {head}");
                }
            }
        }
        else if (h.IsAudio)
        {
            if (body.IsEmpty) return;
            if (h.Width != Wire.NativeAudioRate || h.Height != Wire.NativeAudioChannels)
            {
                Interlocked.Increment(ref _other);
                return;
            }
            Interlocked.Increment(ref _audio);
            Interlocked.Add(ref _audioBytes, body.Length);
            Audio?.Invoke(body, false);
        }
        else if (h.IsVideo && !body.IsEmpty)
        {
            Interlocked.Increment(ref _video);
            Interlocked.Add(ref _videoBytes, body.Length);
            Interlocked.Add(ref _ageUs, h.Stride);
            if (h.Stride > Interlocked.Read(ref _ageMax)) Interlocked.Exchange(ref _ageMax, h.Stride);
            VideoH264?.Invoke(body, h.IsKeyframe);
        }
        else if (body.IsEmpty)
        {
            Interlocked.Increment(ref _empty);
        }
        else
        {
            Interlocked.Increment(ref _other);
        }
    }

    private void Write(byte[] message, string what)
    {
        try
        {
            lock (_writeLock) _stream.Write(message);
        }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException)
        {
            if (Volatile.Read(ref _closed) == 0) Log.Write("native", $"send {what} failed: {Describe(ex)}");
            Fail("Connection to the Switch lost.");
        }
    }

    private static string Describe(Exception ex) => ex is UsbException ue
        ? $"usb {ue.ErrorName}"
        : (ex as SocketException ?? ex.InnerException as SocketException) is { } se
            ? $"socket {se.SocketErrorCode}"
            : ex.GetType().Name;

    private void CheckSilence()
    {
        if (Volatile.Read(ref _closed) != 0) return;
        if (Environment.TickCount64 - Interlocked.Read(ref _lastReceiveTick) >= SilenceMs) Fail("no data from the console for 5 s");
    }

    public void RequestKeyframe()
    {
        Log.Write("native", "asking the console for a keyframe");
        Write(Wire.NativeKeyframeRequest, "keyframe request");
    }

    private void Fail(string reason)
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        Log.Write("native", $"link lost: {reason}");
        Shutdown();
        try { Failed?.Invoke(reason); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { Log.Error("native", "link lost handler failed", ex); }
    }

    public void Close()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        try { lock (_writeLock) _stream.Write(Wire.NativeBye); }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException) { }
        Log.Write("native", "closed by the viewer (bye sent)");
        Shutdown();
    }

    private void Shutdown()
    {
        _watchdog.Dispose();
        _stream.Close();
        _stream.Dispose();
        LogTotals();
    }

    private void LogTotals()
    {
        var seconds = Math.Max(0.001, _uptime.Elapsed.TotalSeconds);
        var video = Interlocked.Read(ref _video);
        Log.Write("native", $"session totals: {_uptime.Elapsed:hh\\:mm\\:ss}, {BytesReceived / 1048576.0:0.0} MB received, " +
                            $"average {BytesReceived * 8 / seconds / 1e6:0.00} Mbit/s");
        Log.Write("native", $"  video {video} frames {_videoBytes / 1048576.0:0.00} MB, console age avg " +
                            $"{(video > 0 ? _ageUs / video / 1000.0 : 0):0.0} ms max {_ageMax / 1000.0:0.0} ms");
        Log.Write("native", $"  audio {_audio} packets {_audioBytes / 1048576.0:0.00} MB, status {_status}, empty {_empty}, " +
                            $"other {_other}, skipped bytes {_skippedBytes}");
    }
}
