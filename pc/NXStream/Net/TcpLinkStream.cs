using System.Net.Sockets;

namespace NXStream.Net;

public sealed class TcpLinkStream : ILinkStream
{
    private readonly Socket _socket;

    public string Name { get; }

    public TcpLinkStream(Socket socket)
    {
        _socket = socket;
        Name = socket.RemoteEndPoint?.ToString() ?? "network";
    }

    public int Read(Span<byte> buffer) => _socket.Receive(buffer);

    public void Write(ReadOnlySpan<byte> data)
    {
        while (!data.IsEmpty) data = data[_socket.Send(data)..];
    }

    public void Close()
    {
        try { _socket.Shutdown(SocketShutdown.Both); }
        catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { }
        _socket.Close();
    }

    public void Dispose() => _socket.Dispose();
}
