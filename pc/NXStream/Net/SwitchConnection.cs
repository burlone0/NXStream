using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using NXStream.Core;
using NXStream.Protocol;

namespace NXStream.Net;

public sealed record ConsoleInfo(string Name, Version Firmware);

public sealed class SwitchConnection : IConsoleConnection
{
    private readonly ILinkStream _stream;
    private readonly object _writeLock = new();
    private readonly byte[] _readBuffer = new byte[1024 * 1024];
    private int _readStart, _readEnd;
    private readonly Thread _receiver;
    private readonly System.Threading.Timer _pingTimer;
    private readonly Stopwatch _uptime = Stopwatch.StartNew();
    private readonly long[] _messages = new long[256];
    private readonly long[] _messageBytes = new long[256];
    private int _closed;
    private long _lastReceiveTick = Environment.TickCount64;
    private int _silenceMs = 5000;
    private int _ignored;
    private long _bytesReceived;
    private long _pings;
    private long _lastProbeTick;
    private long _probes, _probeAcks, _probeBytes;
    private long _probeBaseCount, _probeBaseAcks, _probeBaseBytes;
    private double _rttMs = -1;
    private double _rttMin = double.MaxValue, _rttMax;

    public ConsoleInfo Console { get; }
    public string TransportName { get; }

    public event MediaHandler? VideoH264;
    public event MediaHandler? VideoJpeg;
    public event MediaHandler? Audio;
    public event Action<string>? Failed;
    public event Action<AppInfo>? AppInfoChanged;

    public long BytesReceived => Interlocked.Read(ref _bytesReceived);
    public double RttMs => Volatile.Read(ref _rttMs);
    public double RttMin => _rttMax > 0 ? _rttMin : -1;
    public double RttMax => _rttMax > 0 ? _rttMax : -1;
    public TimeSpan Uptime => _uptime.Elapsed;
    public bool ProbeActive
    {
        get
        {
            var last = Interlocked.Read(ref _lastProbeTick);
            return last != 0 && Environment.TickCount64 - last < 2000;
        }
    }
    public long ProbeCount => Interlocked.Read(ref _probes);
    public long ProbeAckCount => Interlocked.Read(ref _probeAcks);
    public long ProbeBytes => Interlocked.Read(ref _probeBytes);
    public long ProbeBaseCount => Interlocked.Read(ref _probeBaseCount);
    public long ProbeBaseAckCount => Interlocked.Read(ref _probeBaseAcks);
    public long ProbeBaseBytes => Interlocked.Read(ref _probeBaseBytes);

    private SwitchConnection(ILinkStream stream, ConsoleInfo console)
    {
        _stream = stream;
        Console = console;
        TransportName = stream.Name;
        _receiver = new Thread(ReceiveLoop)
        {
            IsBackground = true,
            Name = "NXStream receiver",
            Priority = ThreadPriority.AboveNormal,
        };
        _pingTimer = new System.Threading.Timer(_ => SendPing(), null, Timeout.Infinite, Timeout.Infinite);
    }

    internal static async Task<Socket> OpenSocketAsync(IPEndPoint remote, CancellationToken ct)
    {
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
        {
            NoDelay = true,
            ReceiveBufferSize = 1024 * 1024,
        };
        var lan = LocalNetwork.LanFor(remote.Address);
        var local = lan?.Address;
        if (lan is not null)
        {
            LocalNetwork.BindToInterface(socket, lan);
            try { socket.Bind(new IPEndPoint(lan.Address, 0)); }
            catch (SocketException ex)
            {
                Log.Write("net", $"bind to the LAN address failed ({ex.SocketErrorCode}), using the routing table");
            }
        }
        Log.Write("net", $"connecting, {(lan is null ? "default route" : "LAN interface bound")}");
        var timer = Stopwatch.StartNew();
        using (var cts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            cts.CancelAfter(TimeSpan.FromSeconds(4));
            try
            {
                await socket.ConnectAsync(remote, cts.Token);
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
                socket.Dispose();
                Log.Write("net", $"connect timed out after {timer.ElapsedMilliseconds} ms");
                throw new TimeoutException($"No response from {remote.Address}. Check the IP (the NXStream app on the Switch shows it) and that " +
                      "the PC and the Switch are on the same network, not a guest network. The console must be on, not in standby.");
            }
            catch (Exception ex)
            {
                socket.Dispose();
                Log.Write("net", $"connect failed: {Describe(ex)}");
                throw;
            }
        }
        Log.Write("net", $"tcp connected in {timer.ElapsedMilliseconds} ms, " +
                         $"rcvbuf {socket.ReceiveBufferSize}, nodelay {socket.NoDelay}");
        return socket;
    }

    public static async Task<SwitchConnection> ConnectAsync(IPEndPoint remote, CancellationToken ct)
    {
        var timer = Stopwatch.StartNew();
        var socket = await OpenSocketAsync(remote, ct);
        socket.ReceiveTimeout = 5000;
        var stream = new TcpLinkStream(socket);
        SwitchConnection connection;
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
        Log.Write("net", $"handshake done in {timer.ElapsedMilliseconds} ms, firmware {connection.Console.Firmware}");
        return connection;
    }

    public static async Task<SwitchConnection> ConnectUsbAsync(CancellationToken ct)
    {
        var timer = Stopwatch.StartNew();
        var stream = await Task.Run(UsbLinkStream.Open, ct);
        Log.Write("usb", $"opened in {timer.ElapsedMilliseconds} ms");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        SwitchConnection connection;
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
            Log.Write("usb", "handshake timed out");
            throw new TimeoutException(UsbText.NoAnswer);
        }
        Log.Write("usb", $"handshake done in {timer.ElapsedMilliseconds} ms, firmware {connection.Console.Firmware}");
        return connection;
    }

    private static SwitchConnection Handshake(ILinkStream socket)
    {
        try
        {
            Send(socket, Messages.Hello("NXStream"));
            var header = new byte[Wire.HeaderSize];
            ReceiveExact(socket, header);
            var size = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
            if (header[0] != (byte)MessageType.Welcome || size is < 8 or > 1024)
                throw new InvalidDataException("Invalid response: not an NXStream console.");
            var payload = new byte[size];
            ReceiveExact(socket, payload);
            var version = BinaryPrimitives.ReadUInt32LittleEndian(payload);
            Log.Write("net", $"welcome: protocol {version} (ours {Wire.Version}), {size} bytes");
            if (version != Wire.Version)
                throw new InvalidDataException($"Switch module version ({version}) differs from the PC app version " +
                                               $"({Wire.Version}): update both.");
            var fw = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4));
            var name = Encoding.UTF8.GetString(payload, 8, (int)size - 8);
            var console = new ConsoleInfo(name, new Version((int)(fw >> 16) & 0xFF, (int)(fw >> 8) & 0xFF, (int)fw & 0xFF));
            return new SwitchConnection(socket, console);
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private static void ReceiveExact(ILinkStream socket, Span<byte> buffer)
    {
        while (!buffer.IsEmpty)
        {
            var n = socket.Read(buffer);
            if (n <= 0) throw new EndOfStreamException();
            buffer = buffer[n..];
        }
    }

    private static void Send(ILinkStream socket, ReadOnlySpan<byte> data) => socket.Write(data);

    public void Start()
    {
        Interlocked.Exchange(ref _lastReceiveTick, Environment.TickCount64);
        _receiver.Start();
        _pingTimer.Change(0, 1000);
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
        var header = new byte[Wire.HeaderSize];
        var payload = new byte[512 * 1024];
        try
        {
            while (true)
            {
                ReadExact(header);
                var type = (MessageType)header[0];
                var keyframe = (header[1] & Wire.FlagKeyframe) != 0;
                var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
                if (size < 0 || size > Wire.MaxPayload)
                    throw new InvalidDataException($"bad message size {size} (type 0x{header[0]:x2})");
                if (payload.Length < size)
                {
                    payload = new byte[Math.Max(size, payload.Length * 2)];
                    Log.Write("net", $"receive buffer grown to {payload.Length / 1024} KB for a 0x{header[0]:x2} message of {size} bytes");
                }
                ReadExact(payload.AsSpan(0, size));
                Interlocked.Add(ref _bytesReceived, Wire.HeaderSize + size);
                _messages[header[0]]++;
                _messageBytes[header[0]] += size;

                var body = payload.AsSpan(0, size);
                switch (type)
                {
                    case MessageType.VideoH264 when size > 8: VideoH264?.Invoke(body[8..], keyframe); break;
                    case MessageType.VideoJpeg when size > 8: VideoJpeg?.Invoke(body[8..], false); break;
                    case MessageType.Audio when size > 8: Audio?.Invoke(body[8..], false); break;
                    case MessageType.AppInfo:
                        if (AppInfo.TryParse(body, out var app) && app is not null)
                        {
                            Log.Write("net", $"app info: state {app.State}, title {app.ProgramId:X16}");
                            AppInfoChanged?.Invoke(app);
                        }
                        else Log.Write("net", $"invalid app info ignored ({size} bytes)");
                        break;
                    case MessageType.Probe when size >= 4:
                        if (!ProbeActive)
                        {
                            Interlocked.Exchange(ref _probeBaseCount, Interlocked.Read(ref _probes));
                            Interlocked.Exchange(ref _probeBaseAcks, Interlocked.Read(ref _probeAcks));
                            Interlocked.Exchange(ref _probeBaseBytes, Interlocked.Read(ref _probeBytes));
                        }
                        Interlocked.Exchange(ref _lastProbeTick, Math.Max(1, Environment.TickCount64));
                        Interlocked.Increment(ref _probes);
                        Interlocked.Add(ref _probeBytes, Wire.HeaderSize + size);
                        if ((header[1] & Wire.FlagReplyRequested) != 0)
                        {
                            Write(Messages.ProbeAck(BinaryPrimitives.ReadUInt32LittleEndian(body)), "probe ack");
                            Interlocked.Increment(ref _probeAcks);
                        }
                        break;
                    case MessageType.Bye:
                        Log.Write("net", "console closed the session (bye)");
                        Fail("The Switch closed the connection.");
                        return;
                    case MessageType.Pong when size >= 8:
                        var rtt = Stopwatch.GetElapsedTime(BinaryPrimitives.ReadInt64LittleEndian(body)).TotalMilliseconds;
                        Volatile.Write(ref _rttMs, rtt);
                        _rttMin = Math.Min(_rttMin, rtt);
                        _rttMax = Math.Max(_rttMax, rtt);
                        break;
                    default:
                        if (++_ignored <= 5) Log.Write("net", $"ignored message 0x{header[0]:x2}, {size} bytes");
                        break;
                }
            }
        }
        catch (EndOfStreamException)
        {
            Fail("The Switch closed the connection.");
        }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException or InvalidDataException)
        {
            if (Volatile.Read(ref _closed) == 0) Log.Write("net", $"receive error: {Describe(ex)}");
            Fail("Connection to the Switch lost.");
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("net", "unexpected error while receiving", ex);
            Fail("NXStream internal error (details in the log).");
        }
    }

    private void Write(byte[] message, string what)
    {
        try
        {
            lock (_writeLock) Send(_stream, message);
        }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException)
        {
            if (Volatile.Read(ref _closed) == 0) Log.Write("net", $"send {what} failed: {Describe(ex)}");
            Fail("Connection to the Switch lost.");
        }
    }

    private static string Describe(Exception ex) => ex is UsbException ue
        ? $"usb {ue.ErrorName}"
        : (ex as SocketException ?? ex.InnerException as SocketException) is { } se
            ? $"socket {se.SocketErrorCode}"
            : ex.GetType().Name;

    private void SendPing()
    {
        if (Volatile.Read(ref _closed) != 0) return;
        if (Environment.TickCount64 - Interlocked.Read(ref _lastReceiveTick) >= _silenceMs)
        {
            Fail("no data from the console for 5 s");
            return;
        }
        Interlocked.Increment(ref _pings);
        Write(Messages.Ping(Stopwatch.GetTimestamp()), "ping");
    }

    public void RequestKeyframe()
    {
        Log.Write("net", "asking the console for a keyframe");
        Write(Messages.KeyframeRequest(), "keyframe request");
    }

    private void Fail(string reason)
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        Log.Write("net", $"link lost: {reason}");
        Shutdown();
        try { Failed?.Invoke(reason); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { Log.Error("net", "link lost handler failed", ex); }
    }

    public void Close()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        try { lock (_writeLock) Send(_stream, Messages.Bye()); }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException) { }
        Log.Write("net", "closed by the viewer (bye sent)");
        Shutdown();
    }

    private void Shutdown()
    {
        _pingTimer.Dispose();
        _stream.Close();
        _stream.Dispose();
        LogTotals();
    }

    private void LogTotals()
    {
        var seconds = Math.Max(0.001, _uptime.Elapsed.TotalSeconds);
        Log.Write("net", $"session totals: {_uptime.Elapsed:hh\\:mm\\:ss}, {BytesReceived / 1048576.0:0.0} MB received, " +
                         $"average {BytesReceived * 8 / seconds / 1e6:0.00} Mbit/s, {Interlocked.Read(ref _pings)} pings, " +
                         (_rttMax > 0 ? $"rtt min {_rttMin:0.0} / max {_rttMax:0.0} ms" : "no pong"));
        foreach (var type in Enum.GetValues<MessageType>())
        {
            var count = _messages[(byte)type];
            if (count > 0)
                Log.Write("net", $"  {type,-10} {count,8} messages {_messageBytes[(byte)type] / 1048576.0,9:0.00} MB" +
                                 $"  avg {_messageBytes[(byte)type] / count,7} bytes  {count / seconds,6:0.0}/s");
        }
    }
}
