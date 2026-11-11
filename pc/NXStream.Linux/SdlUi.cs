using System.Runtime.InteropServices;

namespace NXStream.Linux;

[StructLayout(LayoutKind.Sequential)]
internal struct SdlMessageBoxButton
{
    public uint Flags;
    public int Id;
    public IntPtr Text;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SdlMessageBoxData
{
    public uint Flags;
    public IntPtr Window;
    public IntPtr Title;
    public IntPtr Message;
    public int ButtonCount;
    public IntPtr Buttons;
    public IntPtr ColorScheme;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SdlColor
{
    public byte R, G, B, A;
}

[StructLayout(LayoutKind.Explicit, Size = 56)]
internal unsafe struct SdlEvent
{
    [FieldOffset(0)] public uint Type;
    [FieldOffset(8)] public uint WindowId;
    [FieldOffset(12)] public byte WindowEventKind;
    [FieldOffset(12)] public fixed byte Text[32];
    [FieldOffset(20)] public int Sym;
    [FieldOffset(24)] public ushort Mod;
    [FieldOffset(16)] public byte Button;
    [FieldOffset(18)] public byte Clicks;
    [FieldOffset(20)] public int X;
    [FieldOffset(24)] public int Y;
    [FieldOffset(20)] public int WheelY;

    public static SdlEvent Read(IntPtr buffer) => Marshal.PtrToStructure<SdlEvent>(buffer);

    public string TextInput()
    {
        fixed (byte* p = Text) return Marshal.PtrToStringUTF8((IntPtr)p) ?? "";
    }
}

internal static partial class Sdl
{
    private const string TtfLib = "libSDL2_ttf-2.0.so.0";
    private const string Core = "libSDL2-2.0.so.0";

    public const uint WindowHidden = 0x8;
    public const uint RendererSoftware = 0x1;
    public const uint EventWindow = 0x200;
    public const uint EventTextInput = 0x303;
    public const uint EventMouseMotion = 0x400;
    public const uint EventMouseButtonDown = 0x401;
    public const uint EventMouseButtonUp = 0x402;
    public const uint EventMouseWheel = 0x403;
    public const byte WindowEventShown = 1;
    public const byte WindowEventExposed = 3;
    public const byte WindowEventSizeChanged = 6;
    public const byte WindowEventRestored = 9;
    public const byte WindowEventLeave = 10;
    public const byte WindowEventFocusGained = 12;
    public const byte WindowEventFocusLost = 13;
    public const byte WindowEventClose = 14;
    public const int CursorArrow = 0, CursorIBeam = 1, CursorHand = 11;
    public const int HintingLight = 1;

    [DllImport(Core)] public static extern int SDL_GetWindowSize(IntPtr window, out int w, out int h);
    [DllImport(Core)] public static extern uint SDL_GetWindowID(IntPtr window);
    [DllImport(Core)] public static extern void SDL_ShowWindow(IntPtr window);
    [DllImport(Core)] public static extern void SDL_HideWindow(IntPtr window);
    [DllImport(Core)] public static extern void SDL_SetWindowSize(IntPtr window, int w, int h);
    [DllImport(Core)] public static extern int SDL_ShowMessageBox(ref SdlMessageBoxData data, out int buttonId);
    [DllImport(Core)] public static extern void SDL_RaiseWindow(IntPtr window);
    [DllImport(Core)] public static extern int SDL_SetWindowModalFor(IntPtr modal, IntPtr parent);
    [DllImport(Core)] public static extern void SDL_SetWindowIcon(IntPtr window, IntPtr icon);
    [DllImport(Core)]
    public static extern unsafe int SDL_RenderReadPixels(IntPtr renderer, IntPtr rect, uint format, byte* pixels, int pitch);
    [DllImport(Core)] public static extern void SDL_StartTextInput();
    [DllImport(Core)] public static extern void SDL_StopTextInput();
    [DllImport(Core)] private static extern IntPtr SDL_GetClipboardText();
    [DllImport(Core)] public static extern int SDL_SetClipboardText([MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Core)] private static extern void SDL_free(IntPtr memory);
    [DllImport(Core)] public static extern IntPtr SDL_CreateSystemCursor(int id);
    [DllImport(Core)] public static extern void SDL_SetCursor(IntPtr cursor);
    [DllImport(Core)] public static extern ulong SDL_GetTicks64();

    [DllImport(TtfLib)] public static extern int TTF_Init();
    [DllImport(TtfLib)] public static extern IntPtr TTF_OpenFont([MarshalAs(UnmanagedType.LPUTF8Str)] string file, int size);
    [DllImport(TtfLib)] public static extern void TTF_SetFontHinting(IntPtr font, int hinting);
    [DllImport(TtfLib)] public static extern int TTF_FontHeight(IntPtr font);
    [DllImport(TtfLib)] public static extern int TTF_GlyphIsProvided32(IntPtr font, uint ch);
    [DllImport(TtfLib)]
    public static extern int TTF_SizeUTF8(IntPtr font, [MarshalAs(UnmanagedType.LPUTF8Str)] string text, out int w, out int h);
    [DllImport(TtfLib)]
    public static extern IntPtr TTF_RenderUTF8_Blended(IntPtr font, [MarshalAs(UnmanagedType.LPUTF8Str)] string text, SdlColor color);

    public static string Clipboard()
    {
        var p = SDL_GetClipboardText();
        if (p == IntPtr.Zero) return "";
        var text = Marshal.PtrToStringUTF8(p) ?? "";
        SDL_free(p);
        return text;
    }
}
