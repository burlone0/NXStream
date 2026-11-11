using System.Globalization;
using System.Text;

namespace NXStream.Core;

public static class PresenceMath
{
    public const int MaxNameBytes = 128;
    public const long StartTolerance = 10;

    public static long StartUnix(DateTimeOffset now, uint elapsedSec) => now.ToUnixTimeSeconds() - elapsedSec;

    public static bool IsRetailBase(ulong tid)
    {
        var baseTid = tid & ~0xFFFUL;
        return (baseTid >> 56) == 0x01;
    }

    public static string LargeImage(ulong tid)
    {
        var baseTid = tid & ~0xFFFUL;
        if (!IsRetailBase(tid)) return "nxstream";
        return "https://api.nlib.cc/nx/" + baseTid.ToString("X16", CultureInfo.InvariantCulture) + "/icon/256/256";
    }

    public static string? ClampName(string? name)
    {
        if (string.IsNullOrEmpty(name)) return null;
        var clean = new StringBuilder(name.Length);
        foreach (var c in name)
        {
            if (!char.IsControl(c)) clean.Append(c);
        }
        var text = clean.ToString().Trim();
        var result = new StringBuilder();
        var bytes = 0;
        foreach (var rune in text.EnumerateRunes())
        {
            var size = rune.Utf8SequenceLength;
            if (bytes + size > MaxNameBytes) break;
            bytes += size;
            result.Append(rune.ToString());
        }
        var final = result.ToString().TrimEnd();
        return final.Length < 2 ? null : final;
    }

    public static long? StableStart(long? previousStart, ulong previousTid, ulong tid, long newStart)
    {
        if (previousStart.HasValue && previousTid == tid && Math.Abs(newStart - previousStart.Value) <= StartTolerance)
            return previousStart;
        return newStart;
    }
}
