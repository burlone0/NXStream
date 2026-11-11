using System.Runtime.InteropServices;

namespace NXStream.Linux;

[StructLayout(LayoutKind.Sequential)]
internal struct SdlAudioSpec
{
    public int Freq;
    public ushort Format;
    public byte Channels;
    public byte Silence;
    public ushort Samples;
    public ushort Padding;
    public uint Size;
    public IntPtr Callback;
    public IntPtr UserData;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SdlRect
{
    public int X, Y, W, H;
}

internal static partial class Sdl
{
    private const string Lib = "libSDL2-2.0.so.0";
    private const string ImageLib = "libSDL2_image-2.0.so.0";

    public const uint InitAudio = 0x10;
    public const uint InitVideo = 0x20;
    public const uint InitEvents = 0x4000;
    public const uint WindowShown = 0x4;
    public const uint WindowResizable = 0x20;
    public const uint WindowFullscreenDesktop = 0x1001;
    public const int WindowPosCentered = 0x2FFF0000;
    public const uint RendererAccelerated = 0x2;
    public const uint RendererPresentVsync = 0x4;
    public const uint PixelFormatArgb8888 = 0x16362004;
    public const int TextureAccessStreaming = 1;
    public const uint EventQuit = 0x100;
    public const uint EventKeyDown = 0x300;
    public const ushort AudioS16 = 0x8010;
    public const int EventSize = 56;

    public const int KeyF3 = 0x4000003C;
    public const int KeyF11 = 0x40000044;

    [DllImport(Lib)] public static extern int SDL_Init(uint flags);
    [DllImport(Lib)] public static extern int SDL_InitSubSystem(uint flags);
    [DllImport(Lib)] public static extern void SDL_QuitSubSystem(uint flags);
    [DllImport(Lib)] public static extern void SDL_Quit();
    [DllImport(Lib)] private static extern IntPtr SDL_GetError();
    [DllImport(Lib)]
    public static extern int SDL_SetHint([MarshalAs(UnmanagedType.LPUTF8Str)] string name,
                                         [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Lib)] public static extern void SDL_DisableScreenSaver();
    [DllImport(Lib)] public static extern void SDL_EnableScreenSaver();
    [DllImport(Lib)] private static extern IntPtr SDL_GetCurrentVideoDriver();

    [DllImport(Lib)]
    public static extern IntPtr SDL_CreateWindow([MarshalAs(UnmanagedType.LPUTF8Str)] string title, int x, int y, int w, int h, uint flags);
    [DllImport(Lib)] public static extern void SDL_DestroyWindow(IntPtr window);
    [DllImport(Lib)] public static extern int SDL_SetWindowFullscreen(IntPtr window, uint flags);
    [DllImport(Lib)] public static extern uint SDL_GetWindowFlags(IntPtr window);

    [DllImport(Lib)] public static extern IntPtr SDL_CreateRenderer(IntPtr window, int index, uint flags);
    [DllImport(Lib)] public static extern void SDL_DestroyRenderer(IntPtr renderer);
    [DllImport(Lib)] public static extern int SDL_GetRendererOutputSize(IntPtr renderer, out int w, out int h);
    [DllImport(Lib)] public static extern int SDL_SetRenderDrawColor(IntPtr renderer, byte r, byte g, byte b, byte a);
    [DllImport(Lib)] public static extern int SDL_RenderClear(IntPtr renderer);
    [DllImport(Lib)] public static extern int SDL_RenderCopy(IntPtr renderer, IntPtr texture, IntPtr source, ref SdlRect destination);
    [DllImport(Lib)] public static extern void SDL_RenderPresent(IntPtr renderer);

    [DllImport(Lib)] public static extern IntPtr SDL_CreateTexture(IntPtr renderer, uint format, int access, int w, int h);
    [DllImport(Lib)] public static extern void SDL_DestroyTexture(IntPtr texture);
    [DllImport(Lib)] public static extern unsafe int SDL_UpdateTexture(IntPtr texture, IntPtr rect, byte* pixels, int pitch);

    [DllImport(Lib)] public static extern int SDL_PollEvent(IntPtr ev);
    [DllImport(Lib)] public static extern int SDL_WaitEventTimeout(IntPtr ev, int timeoutMs);

    [DllImport(Lib)]
    public static extern uint SDL_OpenAudioDevice(IntPtr device, int isCapture, ref SdlAudioSpec desired,
                                                  out SdlAudioSpec obtained, int allowedChanges);
    [DllImport(Lib)] public static extern void SDL_CloseAudioDevice(uint device);
    [DllImport(Lib)] public static extern void SDL_PauseAudioDevice(uint device, int pause);

    [DllImport(Lib)] public static extern unsafe IntPtr SDL_RWFromConstMem(byte* mem, int size);
    [DllImport(Lib)] public static extern IntPtr SDL_ConvertSurfaceFormat(IntPtr surface, uint format, uint flags);
    [DllImport(Lib)] public static extern void SDL_FreeSurface(IntPtr surface);

    [DllImport(ImageLib)] public static extern int IMG_Init(int flags);
    [DllImport(ImageLib)] public static extern IntPtr IMG_Load_RW(IntPtr source, int freeSource);

    public static string Error() => Marshal.PtrToStringUTF8(SDL_GetError()) ?? "";
    public static string VideoDriver() => Marshal.PtrToStringUTF8(SDL_GetCurrentVideoDriver()) ?? "?";
}
