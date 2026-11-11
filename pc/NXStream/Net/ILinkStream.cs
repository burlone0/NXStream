namespace NXStream.Net;

public interface ILinkStream : IDisposable
{
    int Read(Span<byte> buffer);
    void Write(ReadOnlySpan<byte> data);
    void Close();
    string Name { get; }
}
