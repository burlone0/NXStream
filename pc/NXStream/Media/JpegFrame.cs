using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace NXStream.Media;

public sealed class JpegFrame : IJpegDecoder
{
    private byte[] _pixels = [];
    private byte[] _buffer = [];

    public int Width { get; private set; }
    public int Height { get; private set; }
    public ReadOnlySpan<byte> Pixels => _pixels.AsSpan(0, Width * Height * 4);

    public bool Decode(ReadOnlySpan<byte> jpeg)
    {
        Bitmap bitmap;
        try
        {
            if (_buffer.Length < jpeg.Length) _buffer = new byte[Math.Max(jpeg.Length, _buffer.Length * 2)];
            jpeg.CopyTo(_buffer);
            bitmap = new Bitmap(new MemoryStream(_buffer, 0, jpeg.Length, false), false);
        }
        catch (ArgumentException)
        {
            return false;
        }

        using (bitmap)
        {
            Width = bitmap.Width;
            Height = bitmap.Height;
            var rowBytes = Width * 4;
            if (_pixels.Length < rowBytes * Height) _pixels = new byte[rowBytes * Height];

            var data = bitmap.LockBits(new Rectangle(0, 0, Width, Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            try
            {
                if (data.Stride == rowBytes)
                    Marshal.Copy(data.Scan0, _pixels, 0, rowBytes * Height);
                else
                    for (var y = 0; y < Height; y++)
                        Marshal.Copy(data.Scan0 + y * data.Stride, _pixels, y * rowBytes, rowBytes);
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
        }
        return true;
    }
}
