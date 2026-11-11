namespace NXStream.Media;

public static class H264Sps
{
    private const int MaxDimension = 8192;

    public static bool TryGetSize(ReadOnlySpan<byte> annexB, out int width, out int height)
    {
        width = height = 0;
        for (var i = 0; i + 3 < annexB.Length; i++)
        {
            if (annexB[i] != 0 || annexB[i + 1] != 0 || annexB[i + 2] != 1 || (annexB[i + 3] & 0x1F) != 7) continue;
            var start = i + 4;
            var end = start;
            while (end + 2 < annexB.Length && !(annexB[end] == 0 && annexB[end + 1] == 0 && annexB[end + 2] is 0 or 1)) end++;
            if (end + 2 >= annexB.Length) end = annexB.Length;
            return Parse(Unescape(annexB[start..end]), out width, out height);
        }
        return false;
    }

    private static byte[] Unescape(ReadOnlySpan<byte> rbsp)
    {
        var output = new List<byte>(rbsp.Length);
        var zeros = 0;
        foreach (var b in rbsp)
        {
            if (zeros >= 2 && b == 3)
            {
                zeros = 0;
                continue;
            }
            output.Add(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
        return [.. output];
    }

    private sealed class Bits(byte[] data)
    {
        private int _pos;

        public bool Failed { get; private set; }

        public uint Read(int count)
        {
            uint value = 0;
            for (var i = 0; i < count; i++)
            {
                var index = _pos >> 3;
                if (index >= data.Length)
                {
                    Failed = true;
                    return 0;
                }
                value = (value << 1) | (uint)((data[index] >> (7 - (_pos & 7))) & 1);
                _pos++;
            }
            return value;
        }

        public uint Ue()
        {
            var zeros = 0;
            while (!Failed && Read(1) == 0)
                if (++zeros > 31) { Failed = true; return 0; }
            return Failed ? 0 : (uint)((1UL << zeros) - 1 + Read(zeros));
        }

        public void Se() => Ue();
    }

    private static bool Parse(byte[] sps, out int width, out int height)
    {
        width = height = 0;
        var bits = new Bits(sps);
        var profile = bits.Read(8);
        bits.Read(16);
        bits.Ue();
        uint chroma = 1;
        if (profile is 100 or 110 or 122 or 244 or 44 or 83 or 86 or 118 or 128 or 138 or 139 or 134 or 135)
        {
            chroma = bits.Ue();
            if (chroma == 3) bits.Read(1);
            bits.Ue();
            bits.Ue();
            bits.Read(1);
            if (bits.Read(1) == 1)
            {
                var lists = chroma != 3 ? 8 : 12;
                for (var i = 0; i < lists && !bits.Failed; i++)
                {
                    if (bits.Read(1) == 0) continue;
                    var size = i < 6 ? 16 : 64;
                    long last = 8, next = 8;
                    for (var j = 0; j < size && !bits.Failed; j++)
                    {
                        if (next != 0)
                        {
                            var delta = bits.Ue();
                            var signed = (delta & 1) == 1 ? (long)((delta + 1) / 2) : -(long)(delta / 2);
                            next = (last + signed + 256) % 256;
                        }
                        last = next == 0 ? last : next;
                    }
                }
            }
        }
        bits.Ue();
        var pocType = bits.Ue();
        if (pocType == 0) bits.Ue();
        else if (pocType == 1)
        {
            bits.Read(1);
            bits.Se();
            bits.Se();
            var cycle = bits.Ue();
            for (var i = 0; i < cycle && !bits.Failed; i++) bits.Se();
        }
        bits.Ue();
        bits.Read(1);
        var widthMbs = bits.Ue() + 1;
        var heightUnits = bits.Ue() + 1;
        var frameMbsOnly = bits.Read(1);
        if (frameMbsOnly == 0) bits.Read(1);
        bits.Read(1);
        uint left = 0, right = 0, top = 0, bottom = 0;
        if (bits.Read(1) == 1)
        {
            left = bits.Ue();
            right = bits.Ue();
            top = bits.Ue();
            bottom = bits.Ue();
        }
        if (bits.Failed) return false;
        var unitX = chroma is 0 or 3 ? 1 : 2;
        var unitY = (chroma is 0 or 3 ? 1 : 2) * (frameMbsOnly == 1 ? 1 : 2);
        var w = (long)widthMbs * 16 - unitX * ((long)left + right);
        var h = (long)(2 - (int)frameMbsOnly) * heightUnits * 16 - unitY * ((long)top + bottom);
        if (w is <= 0 or > MaxDimension || h is <= 0 or > MaxDimension) return false;
        width = (int)w;
        height = (int)h;
        return true;
    }
}
