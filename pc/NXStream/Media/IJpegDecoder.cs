using System;

namespace NXStream.Media;

public interface IJpegDecoder
{
    int Width { get; }
    int Height { get; }
    ReadOnlySpan<byte> Pixels { get; }
    bool Decode(ReadOnlySpan<byte> jpeg);
}
