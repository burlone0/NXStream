using System.Buffers.Binary;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Text;

namespace NXStream.Core;

public sealed class DiscordIpc : IDisposable
{
    public const int OpHandshake = 0;
    public const int OpFrame = 1;
    public const int OpClose = 2;
    public const int OpPing = 3;
    public const int OpPong = 4;
    public const int MaxPayload = 65536;
    private const int ConnectTimeoutMs = 500;

    private readonly Stream stream;
    private readonly Socket? socket;
    private readonly object writeLock = new();
    private int disposed;

    private DiscordIpc(Stream stream, Socket? socket)
    {
        this.stream = stream;
        this.socket = socket;
    }

    public static byte[] EncodeFrame(int op, string json)
    {
        var body = Encoding.UTF8.GetBytes(json);
        var frame = new byte[8 + body.Length];
        BinaryPrimitives.WriteInt32LittleEndian(frame, op);
        BinaryPrimitives.WriteInt32LittleEndian(frame.AsSpan(4), body.Length);
        body.CopyTo(frame, 8);
        return frame;
    }

    public static bool TryReadFrame(Stream s, out int op, out string json)
    {
        op = 0;
        json = string.Empty;
        try
        {
            var header = new byte[8];
            if (!ReadExactly(s, header)) return false;
            op = BinaryPrimitives.ReadInt32LittleEndian(header);
            var length = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
            if (length > MaxPayload) return false;
            var body = new byte[length];
            if (!ReadExactly(s, body)) return false;
            json = Encoding.UTF8.GetString(body);
            return true;
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or SocketException or InvalidOperationException)
        {
            return false;
        }
    }

    public static string HandshakeJson(string clientId) => "{\"v\":1,\"client_id\":\"" + clientId + "\"}";

    private static bool ReadExactly(Stream s, byte[] buffer)
    {
        var done = 0;
        while (done < buffer.Length)
        {
            var n = s.Read(buffer, done, buffer.Length - done);
            if (n <= 0) return false;
            done += n;
        }
        return true;
    }

    public static DiscordIpc? Connect(CancellationToken ct)
    {
        for (var i = 0; i < 10 && !ct.IsCancellationRequested; i++)
        {
            var ipc = OperatingSystem.IsWindows() ? ConnectPipe(i, ct) : ConnectSocket(i, ct);
            if (ipc != null) return ipc;
        }
        return null;
    }

    private static DiscordIpc? ConnectPipe(int index, CancellationToken ct)
    {
        NamedPipeClientStream? pipe = null;
        try
        {
            pipe = new NamedPipeClientStream(".", "discord-ipc-" + index, PipeDirection.InOut, PipeOptions.Asynchronous);
            pipe.ConnectAsync(150, ct).GetAwaiter().GetResult();
            return new DiscordIpc(pipe, null);
        }
        catch (Exception ex) when (ex is TimeoutException or IOException or UnauthorizedAccessException or OperationCanceledException)
        {
            pipe?.Dispose();
            return null;
        }
    }

    private static IEnumerable<string> SocketDirectories()
    {
        var runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!string.IsNullOrEmpty(runtime))
        {
            yield return runtime;
            yield return Path.Combine(runtime, "app", "com.discordapp.Discord");
            yield return Path.Combine(runtime, "snap.discord");
            yield return Path.Combine(runtime, ".flatpak", "dev.vencord.Vesktop", "xdg-run");
        }
        var tmp = Environment.GetEnvironmentVariable("TMPDIR");
        if (!string.IsNullOrEmpty(tmp)) yield return tmp;
        yield return "/tmp";
    }

    private static DiscordIpc? ConnectSocket(int index, CancellationToken ct)
    {
        foreach (var dir in SocketDirectories())
        {
            var path = Path.Combine(dir, "discord-ipc-" + index);
            if (!File.Exists(path)) continue;
            Socket? s = null;
            try
            {
                s = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                timeout.CancelAfter(ConnectTimeoutMs);
                s.ConnectAsync(new UnixDomainSocketEndPoint(path), timeout.Token).AsTask().GetAwaiter().GetResult();
                return new DiscordIpc(new NetworkStream(s, false), s);
            }
            catch (Exception ex) when (ex is SocketException or IOException or UnauthorizedAccessException or OperationCanceledException)
            {
                s?.Dispose();
                if (ct.IsCancellationRequested) return null;
            }
        }
        return null;
    }

    public bool Send(int op, string json)
    {
        try
        {
            var frame = EncodeFrame(op, json);
            lock (writeLock)
            {
                stream.Write(frame, 0, frame.Length);
                stream.Flush();
            }
            return true;
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or SocketException or InvalidOperationException)
        {
            return false;
        }
    }

    public bool Read(out int op, out string json) => TryReadFrame(stream, out op, out json);

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        try { socket?.Shutdown(SocketShutdown.Both); } catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { }
        try { stream.Dispose(); } catch (Exception ex) when (ex is IOException or ObjectDisposedException) { }
        try { socket?.Dispose(); } catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { }
    }
}
