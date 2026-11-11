namespace NXStream.Media;

public sealed class WindowsMedia : IMediaFactory
{
    public IAudioSink CreateAudio(float volume) => new AudioPlayer { Volume = volume };
    public IJpegDecoder CreateJpeg() => new JpegFrame();
}
