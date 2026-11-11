using System;

namespace NXStream.Media;

public interface IAudioSink : IDisposable
{
    float Volume { set; }
    double BufferedMs { get; }
    long Underruns { get; }
    long Trims { get; }
    bool AdaptationFrozen { set; }
    int AudioTargetMs { get; }
    void Push(ReadOnlySpan<byte> pcm);
}
