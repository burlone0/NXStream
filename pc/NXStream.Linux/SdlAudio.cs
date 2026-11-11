using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Media;

namespace NXStream.Linux;

internal sealed unsafe class SdlAudio : IAudioSink
{
    private const int Samples = 512;

    private static bool _initFailureLogged;
    private readonly uint _device;
    private readonly JitterBuffer _buffer = new();
    private readonly GCHandle _self;
    private byte[] _scratch = new byte[Samples * 4];
    private long _callbacks;
    private bool _disposed;

    public SdlAudio(float volume)
    {
        Volume = volume;
        if (Sdl.SDL_InitSubSystem(Sdl.InitAudio) != 0)
        {
            var error = Sdl.Error();
            if (!_initFailureLogged) Log.Write("audio", "SDL audio subsystem unavailable: " + error);
            _initFailureLogged = true;
            throw new InvalidOperationException(error);
        }
        _self = GCHandle.Alloc(this);
        var want = new SdlAudioSpec
        {
            Freq = 48000,
            Format = Sdl.AudioS16,
            Channels = 2,
            Samples = Samples,
            Callback = (IntPtr)(delegate* unmanaged<IntPtr, byte*, int, void>)&Fill,
            UserData = GCHandle.ToIntPtr(_self),
        };
        _device = Sdl.SDL_OpenAudioDevice(IntPtr.Zero, 0, ref want, out var have, 0);
        if (_device == 0)
        {
            var error = Sdl.Error();
            _self.Free();
            Sdl.SDL_QuitSubSystem(Sdl.InitAudio);
            throw new InvalidOperationException(error);
        }
        Log.Write("audio", $"SDL audio device {_device}: {have.Freq} Hz {have.Channels} ch format 0x{have.Format:x}, " +
                           $"{have.Samples} samples callback, adaptive jitter buffer 50-200 ms");
        Sdl.SDL_PauseAudioDevice(_device, 0);
    }

    public float Volume
    {
        set => _buffer.Gain = value;
    }

    public double BufferedMs => _buffer.BufferedMs;
    public long Underruns => _buffer.Underruns;
    public long Trims => _buffer.Trims;
    public bool AdaptationFrozen { set => _buffer.Frozen = value; }
    public int AudioTargetMs => _buffer.TargetMs;

    public void Push(ReadOnlySpan<byte> pcm)
    {
        var length = pcm.Length - pcm.Length % 4;
        if (length > 0 && !_disposed) _buffer.Write(pcm[..length]);
    }

    [UnmanagedCallersOnly]
    private static void Fill(IntPtr userData, byte* stream, int length)
    {
        var self = (SdlAudio)GCHandle.FromIntPtr(userData).Target!;
        if (self._scratch.Length < length) self._scratch = new byte[length];
        self._buffer.Read(self._scratch, 0, length);
        Marshal.Copy(self._scratch, 0, (IntPtr)stream, length);
        if (++self._callbacks == 1) Log.Write("audio", $"first SDL audio callback, {length} bytes");
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Sdl.SDL_PauseAudioDevice(_device, 1);
        Sdl.SDL_CloseAudioDevice(_device);
        _self.Free();
        Sdl.SDL_QuitSubSystem(Sdl.InitAudio);
        Log.Write("audio", $"stopped: {_callbacks} callbacks, {Underruns} underruns, {Trims} trims");
    }
}
