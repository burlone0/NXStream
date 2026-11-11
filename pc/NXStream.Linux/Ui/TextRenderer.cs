using System.Drawing;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Render;

namespace NXStream.Linux.Ui;

internal enum UiFontKind { Sans, Semibold, Mono }

internal readonly record struct UiFont(UiFontKind Kind, float Pt)
{
    public static readonly UiFont Body = new(UiFontKind.Sans, 9f);
    public static readonly UiFont Heading = new(UiFontKind.Semibold, 14f);
    public static readonly UiFont OverlayBody = new(UiFontKind.Sans, 10f);
    public static readonly UiFont OverlayMono = new(UiFontKind.Mono, 10.5f);
}

internal static class TextRenderer
{
    private static readonly Dictionary<(UiFontKind, int), (IntPtr Primary, IntPtr Fallback)> Fonts = [];
    private static readonly Dictionary<(IntPtr, string), (byte[] Alpha, int W, int H)> Bitmaps = [];
    private static bool _ready;

    private static readonly Color OverlayAccent = Color.FromArgb(120, 170, 255);
    private static readonly Color OverlayWarning = Color.FromArgb(255, 190, 90);
    private static readonly Color OverlayText = Color.FromArgb(240, 240, 240);
    private static readonly Color OverlayMuted = Color.FromArgb(170, 170, 170);

    private static (IntPtr Primary, IntPtr Fallback) Get(UiFont font, float scale)
    {
        if (!_ready)
        {
            _ready = true;
            if (Sdl.TTF_Init() != 0) Log.Write("ui", "TTF_Init failed: " + Sdl.Error());
        }
        var px = Math.Max(6, (int)Math.Round(font.Pt * scale * 96 / 72));
        if (Fonts.TryGetValue((font.Kind, px), out var cached)) return cached;
        var path = font.Kind switch
        {
            UiFontKind.Semibold => FontLocator.SansSemibold,
            UiFontKind.Mono => FontLocator.Mono,
            _ => FontLocator.Sans,
        };
        var primary = Open(path, px, font.Kind.ToString());
        var fallbackPath = FontLocator.Fallback;
        var fallback = fallbackPath is not null && fallbackPath != path ? Open(fallbackPath, px, "Fallback") : IntPtr.Zero;
        if (primary == IntPtr.Zero) primary = fallback;
        Fonts[(font.Kind, px)] = (primary, fallback);
        return (primary, fallback);
    }

    private static IntPtr Open(string? path, int px, string kind)
    {
        if (path is null) return IntPtr.Zero;
        var handle = Sdl.TTF_OpenFont(path, px);
        if (handle == IntPtr.Zero) Log.Write("ui", $"TTF_OpenFont {kind} {px}px failed");
        else Sdl.TTF_SetFontHinting(handle, Sdl.HintingLight);
        return handle;
    }

    private static List<(string Text, IntPtr Font)> Split(string text, (IntPtr Primary, IntPtr Fallback) fonts)
    {
        var runs = new List<(string, IntPtr)>();
        if (text.Length == 0 || fonts.Primary == IntPtr.Zero) return runs;
        var current = new System.Text.StringBuilder();
        var currentFont = IntPtr.Zero;
        foreach (var rune in text.EnumerateRunes())
        {
            var provided = fonts.Fallback == IntPtr.Zero || Sdl.TTF_GlyphIsProvided32(fonts.Primary, (uint)rune.Value) != 0;
            var font = provided ? fonts.Primary : fonts.Fallback;
            if (font != currentFont && current.Length > 0)
            {
                runs.Add((current.ToString(), currentFont));
                current.Clear();
            }
            currentFont = font;
            current.Append(rune.ToString());
        }
        if (current.Length > 0) runs.Add((current.ToString(), currentFont));
        return runs;
    }

    public static int Height(UiFont font, float scale)
    {
        var fonts = Get(font, scale);
        return fonts.Primary == IntPtr.Zero ? 0 : Sdl.TTF_FontHeight(fonts.Primary);
    }

    public static int Width(string text, UiFont font, float scale)
    {
        var total = 0;
        foreach (var (run, handle) in Split(text, Get(font, scale)))
        {
            Sdl.TTF_SizeUTF8(handle, run, out var w, out _);
            total += w;
        }
        return total;
    }

    public static string Fit(string text, UiFont font, float scale, int maxWidth)
    {
        if (Width(text, font, scale) <= maxWidth) return text;
        var lo = 0;
        var hi = text.Length;
        while (lo < hi)
        {
            var mid = (lo + hi + 1) / 2;
            if (Width(text[..mid] + "…", font, scale) <= maxWidth) lo = mid;
            else hi = mid - 1;
        }
        return lo == 0 ? "…" : text[..lo] + "…";
    }

    public static List<string> Wrap(string text, UiFont font, float scale, int maxWidth)
    {
        var lines = new List<string>();
        foreach (var paragraph in text.Split('\n'))
        {
            var line = "";
            foreach (var word in paragraph.Split(' '))
            {
                var candidate = line.Length == 0 ? word : line + " " + word;
                if (line.Length > 0 && Width(candidate, font, scale) > maxWidth)
                {
                    lines.Add(line);
                    line = word;
                }
                else
                {
                    line = candidate;
                }
            }
            lines.Add(line);
        }
        return lines;
    }

    public static void Draw(Canvas canvas, string text, UiFont font, float scale, int x, int y, Color color)
    {
        foreach (var (run, handle) in Split(text, Get(font, scale)))
        {
            var (alpha, w, h) = Bitmap(handle, run);
            if (w == 0) continue;
            canvas.BlitAlpha(alpha, w, h, x, y, color);
            x += w;
        }
    }

    private static (byte[] Alpha, int W, int H) Bitmap(IntPtr font, string text)
    {
        if (Bitmaps.TryGetValue((font, text), out var cached)) return cached;
        if (Bitmaps.Count > 800) Bitmaps.Clear();
        var surface = Sdl.TTF_RenderUTF8_Blended(font, text, new SdlColor { R = 255, G = 255, B = 255, A = 255 });
        if (surface == IntPtr.Zero) return (Array.Empty<byte>(), 0, 0);
        var w = Marshal.ReadInt32(surface, 16);
        var h = Marshal.ReadInt32(surface, 20);
        var pitch = Marshal.ReadInt32(surface, 24);
        var pixels = Marshal.ReadIntPtr(surface, 32);
        var alpha = new byte[w * h];
        for (var j = 0; j < h; j++)
            for (var i = 0; i < w; i++)
                alpha[j * w + i] = Marshal.ReadByte(pixels, j * pitch + i * 4 + 3);
        Sdl.SDL_FreeSurface(surface);
        return Bitmaps[(font, text)] = (alpha, w, h);
    }

    public static OverlayImage RasterizeStats(string[] lines, float scale = 1f)
    {
        var font = UiFont.OverlayMono;
        var lineHeight = Height(font, scale) + (int)Math.Round(3 * scale);
        var width = lines.Max(l => Width(l, font, scale)) + (int)Math.Round(28 * scale);
        var height = lineHeight * lines.Length + (int)Math.Round(18 * scale);
        var canvas = new Canvas(width, height);
        canvas.Clear(Color.Black);
        for (var i = 0; i < lines.Length; i++)
        {
            var color = i == 0 ? OverlayAccent : lines[i].StartsWith('!') ? OverlayWarning : OverlayText;
            Draw(canvas, lines[i], font, scale, (int)Math.Round(14 * scale), (int)Math.Round(9 * scale) + i * lineHeight, color);
        }
        return new OverlayImage(canvas.ToBgra(), width, height, false, 0.82f);
    }

    public static OverlayImage RasterizeMessage(string title, string body, float scale = 1f)
    {
        var rest = string.IsNullOrEmpty(body) ? [] : body.Split('\n');
        var titleHeight = Height(UiFont.Heading, scale);
        var bodyHeight = Height(UiFont.OverlayBody, scale);
        var widestBody = rest.Length == 0 ? 0 : rest.Max(l => Width(l, UiFont.OverlayBody, scale));
        var widest = Math.Max(Width(title, UiFont.Heading, scale), widestBody);
        var width = widest + (int)Math.Round(16 * scale);
        var gap = (int)Math.Round(4 * scale);
        var height = titleHeight + rest.Length * (bodyHeight + gap) + (int)Math.Round(16 * scale);
        var canvas = new Canvas(width, height);
        canvas.Clear(Color.Black);
        var y = (int)Math.Round(4 * scale);
        Draw(canvas, title, UiFont.Heading, scale, (width - Width(title, UiFont.Heading, scale)) / 2, y, OverlayText);
        y += titleHeight + (int)Math.Round(8 * scale);
        foreach (var line in rest)
        {
            Draw(canvas, line, UiFont.OverlayBody, scale, (width - Width(line, UiFont.OverlayBody, scale)) / 2, y, OverlayMuted);
            y += bodyHeight + gap;
        }
        return new OverlayImage(canvas.ToBgra(), width, height, true, 1f);
    }
}
