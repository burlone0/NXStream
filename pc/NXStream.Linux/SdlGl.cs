using System.Runtime.InteropServices;

namespace NXStream.Linux;

internal static partial class Sdl
{
    public const int GlDoubleBuffer = 5;
    public const int GlContextMajorVersion = 17, GlContextMinorVersion = 18, GlContextProfileMask = 21;
    public const int GlProfileCore = 1;
    public const uint WindowOpenGl = 0x2;
    public const uint WindowAllowHighDpi = 0x2000;

    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GL_SetAttribute(int attribute, int value);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GL_GetAttribute(int attribute, out int value);
    [DllImport("libSDL2-2.0.so.0")] public static extern IntPtr SDL_GL_CreateContext(IntPtr window);
    [DllImport("libSDL2-2.0.so.0")] public static extern void SDL_GL_DeleteContext(IntPtr context);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GL_MakeCurrent(IntPtr window, IntPtr context);
    [DllImport("libSDL2-2.0.so.0")] public static extern void SDL_GL_SwapWindow(IntPtr window);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GL_SetSwapInterval(int interval);
    [DllImport("libSDL2-2.0.so.0")] public static extern void SDL_GL_GetDrawableSize(IntPtr window, out int w, out int h);
    [DllImport("libSDL2-2.0.so.0")] public static extern IntPtr SDL_GL_GetProcAddress([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
}
