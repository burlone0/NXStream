namespace NXStream.Media;

public interface IMediaFactory
{
    IAudioSink CreateAudio(float volume);
    IJpegDecoder CreateJpeg();
}
