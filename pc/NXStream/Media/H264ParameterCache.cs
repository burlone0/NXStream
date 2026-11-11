namespace NXStream.Media;

public sealed class H264ParameterCache
{
    private const int NalSps = 7, NalPps = 8;
    private byte[]? _sps, _pps;

    public byte[] AnnexB { get; private set; } = [];

    public void Update(ReadOnlySpan<byte> accessUnit)
    {
        var changed = false;
        var pos = 0;
        while (NextNal(accessUnit, ref pos, out var type, out var nal))
        {
            if (type == NalSps && (_sps is null || !nal.SequenceEqual(_sps))) { _sps = nal.ToArray(); changed = true; }
            else if (type == NalPps && (_pps is null || !nal.SequenceEqual(_pps))) { _pps = nal.ToArray(); changed = true; }
        }
        if (changed && _sps is not null && _pps is not null)
            AnnexB = [0, 0, 0, 1, .. _sps, 0, 0, 0, 1, .. _pps];
    }

    public bool Contains(ReadOnlySpan<byte> accessUnit)
    {
        var pos = 0;
        while (NextNal(accessUnit, ref pos, out var type, out _))
            if (type == NalSps) return true;
        return false;
    }

    private static int StartCodeEnd(ReadOnlySpan<byte> data, int from)
    {
        for (var i = from; i + 2 < data.Length; i++)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i + 3;
        return -1;
    }

    private static bool NextNal(ReadOnlySpan<byte> data, ref int pos, out int type, out ReadOnlySpan<byte> nal)
    {
        type = 0;
        nal = default;
        while (true)
        {
            var start = StartCodeEnd(data, pos);
            if (start < 0)
            {
                pos = data.Length;
                return false;
            }
            var next = StartCodeEnd(data, start);
            var end = next < 0 ? data.Length : next - 3;
            pos = end;
            while (end > start && data[end - 1] == 0) end--;
            if (end <= start) continue;
            nal = data[start..end];
            type = nal[0] & 0x1F;
            return true;
        }
    }
}
