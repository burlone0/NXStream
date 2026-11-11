using NXStream.Media;

namespace NXStream.Linux;

public sealed class LinuxMedia : IMediaFactory
{
    public LinuxMedia() => SdlJpeg.Init();

    public IAudioSink CreateAudio(float volume) => new SdlAudio(volume);

    public IJpegDecoder CreateJpeg() => new SdlJpeg();
}
