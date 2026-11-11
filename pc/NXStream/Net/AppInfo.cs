using System.Buffers.Binary;
using System.Globalization;
using System.Text;

namespace NXStream.Net;

public enum AppState : byte { Home = 0, Running = 1, Suspended = 2, Homebrew = 3 }

public sealed record AppInfo(AppState State, ulong ProgramId, string? Name, uint? ElapsedSec)
{
    private const int HeaderSize = 16;
    private const int MaxName = 128;

    public static bool TryParse(ReadOnlySpan<byte> payload, out AppInfo? info)
    {
        info = null;
        if (payload.Length < HeaderSize || payload[0] != 1 || payload[1] > 3) return false;
        int nameLen = BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]);
        if (nameLen > MaxName || payload.Length != HeaderSize + nameLen) return false;
        var tid = BinaryPrimitives.ReadUInt64LittleEndian(payload[4..]);
        var elapsed = BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]);
        var name = Clean(Encoding.UTF8.GetString(payload.Slice(HeaderSize, nameLen)));
        info = new AppInfo((AppState)payload[1], tid, name, elapsed == uint.MaxValue ? null : elapsed);
        return true;
    }

    public static AppInfo? FromNativeInfo(string body)
    {
        string? app = null, appName = null, age = null, fg = null, hb = null;
        foreach (var line in body.Split('\n'))
        {
            var eq = line.IndexOf('=');
            if (eq <= 0) continue;
            var key = line[..eq].Trim();
            var value = line[(eq + 1)..].Trim('\r', ' ', '\0');
            switch (key)
            {
                case "app": app = value; break;
                case "appname": appName = value; break;
                case "appage": age = value; break;
                case "appfg": fg = value; break;
                case "apphb": hb = value; break;
            }
        }
        if (app is null || app.Length != 16 || !ulong.TryParse(app, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var tid)) return null;
        if (fg is not null && fg != "0" && fg != "1") return null;
        if (hb is not null && hb != "0" && hb != "1") return null;
        if (hb == "1")
        {
            uint? hbElapsed = uint.TryParse(age, NumberStyles.None, CultureInfo.InvariantCulture, out var h) && h != uint.MaxValue ? h : null;
            return new AppInfo(AppState.Homebrew, 0, null, hbElapsed);
        }
        if (tid == 0) return new AppInfo(AppState.Home, 0, null, null);
        var state = fg == "0" ? AppState.Suspended : AppState.Running;
        uint? elapsed = uint.TryParse(age, NumberStyles.None, CultureInfo.InvariantCulture, out var a) && a != uint.MaxValue ? a : null;
        return new AppInfo(state, tid, Clean(appName), elapsed);
    }

    private static string? Clean(string? name)
    {
        if (name is null) return null;
        var cleaned = new string(name.Where(c => !char.IsControl(c)).ToArray());
        return cleaned.Length == 0 ? null : cleaned;
    }
}
