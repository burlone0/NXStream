using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Media;

namespace NXStream.Linux;

internal sealed class SdlJpeg : IJpegDecoder
{
    private const int MaxPixels = 8192 * 8192;
    private static bool _initialised;
    private byte[] _pixels = [];

    public int Width { get; private set; }
    public int Height { get; private set; }
    public ReadOnlySpan<byte> Pixels => _pixels.AsSpan(0, Width * Height * 4);

    public static void Init()
    {
        if (_initialised) return;
        _initialised = true;
        var flags = Sdl.IMG_Init(1);
        Log.Write("video", $"SDL2_image initialised, jpeg support {(flags & 1) != 0}");
    }

    public unsafe bool Decode(ReadOnlySpan<byte> jpeg)
    {
        IntPtr loaded, converted = IntPtr.Zero;
        fixed (byte* data = jpeg)
        {
            var rw = Sdl.SDL_RWFromConstMem(data, jpeg.Length);
            if (rw == IntPtr.Zero) return false;
            loaded = Sdl.IMG_Load_RW(rw, 1);
        }
        if (loaded == IntPtr.Zero) return false;
        try
        {
            converted = Sdl.SDL_ConvertSurfaceFormat(loaded, Sdl.PixelFormatArgb8888, 0);
            if (converted == IntPtr.Zero) return false;
            var width = Marshal.ReadInt32(converted, 16);
            var height = Marshal.ReadInt32(converted, 20);
            var pitch = Marshal.ReadInt32(converted, 24);
            var source = Marshal.ReadIntPtr(converted, 32);
            if (width <= 0 || height <= 0 || width * height > MaxPixels || source == IntPtr.Zero) return false;
            var rowBytes = width * 4;
            if (_pixels.Length < rowBytes * height) _pixels = new byte[rowBytes * height];
            for (var y = 0; y < height; y++)
                Marshal.Copy(source + y * pitch, _pixels, y * rowBytes, rowBytes);
            Width = width;
            Height = height;
            return true;
        }
        finally
        {
            if (converted != IntPtr.Zero) Sdl.SDL_FreeSurface(converted);
            Sdl.SDL_FreeSurface(loaded);
        }
    }
}
