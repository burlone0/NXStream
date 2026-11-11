namespace NXStream.Linux;

internal sealed class FrameSlot
{
    private readonly object _sync = new();
    private byte[] _pending = [];
    private byte[] _display = [];
    private int _width, _height;
    private bool _fresh;

    public void Submit(ReadOnlySpan<byte> bgra, int width, int height)
    {
        lock (_sync)
        {
            if (_pending.Length < bgra.Length) _pending = new byte[bgra.Length];
            bgra.CopyTo(_pending);
            _width = width;
            _height = height;
            _fresh = true;
        }
    }

    public bool Take(out byte[] frame, out int width, out int height)
    {
        lock (_sync)
        {
            frame = _display;
            width = _width;
            height = _height;
            if (!_fresh) return false;
            (_pending, _display) = (_display, _pending);
            frame = _display;
            _fresh = false;
            return true;
        }
    }
}
