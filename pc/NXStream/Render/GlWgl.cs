using System.Runtime.InteropServices;

namespace NXStream.Render;

internal static unsafe partial class Gl
{
    public const uint WGL_ACCESS_READ_ONLY_NV = 0x0000;

    public static delegate* unmanaged<int, int> SwapIntervalEXT;

    public static delegate* unmanaged<nint, nint> DXOpenDeviceNV;
    public static delegate* unmanaged<nint, int> DXCloseDeviceNV;
    public static delegate* unmanaged<nint, nint, uint, uint, uint, nint> DXRegisterObjectNV;
    public static delegate* unmanaged<nint, nint, int> DXUnregisterObjectNV;
    public static delegate* unmanaged<nint, int, nint*, int> DXLockObjectsNV;
    public static delegate* unmanaged<nint, int, nint*, int> DXUnlockObjectsNV;

    public static bool HasDxInterop => DXOpenDeviceNV != null && DXRegisterObjectNV != null && DXLockObjectsNV != null &&
                                       DXUnlockObjectsNV != null && DXUnregisterObjectNV != null && DXCloseDeviceNV != null;

    public static void LoadWgl()
    {
        SwapIntervalEXT = (delegate* unmanaged<int, int>)WglProc("wglSwapIntervalEXT");
        DXOpenDeviceNV = (delegate* unmanaged<nint, nint>)WglProc("wglDXOpenDeviceNV");
        DXCloseDeviceNV = (delegate* unmanaged<nint, int>)WglProc("wglDXCloseDeviceNV");
        DXRegisterObjectNV = (delegate* unmanaged<nint, nint, uint, uint, uint, nint>)WglProc("wglDXRegisterObjectNV");
        DXUnregisterObjectNV = (delegate* unmanaged<nint, nint, int>)WglProc("wglDXUnregisterObjectNV");
        DXLockObjectsNV = (delegate* unmanaged<nint, int, nint*, int>)WglProc("wglDXLockObjectsNV");
        DXUnlockObjectsNV = (delegate* unmanaged<nint, int, nint*, int>)WglProc("wglDXUnlockObjectsNV");
    }

    public static nint WglProc(string name)
    {
        var p = wglGetProcAddress(name);
        return p is 1 or 2 or 3 or -1 ? 0 : p;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PixelFormatDescriptor
    {
        public ushort nSize, nVersion;
        public uint dwFlags;
        public byte iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift,
                    cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits,
                    cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved;
        public uint dwLayerMask, dwVisibleMask, dwDamageMask;
    }

    public const uint PFD_DOUBLEBUFFER = 0x1, PFD_DRAW_TO_WINDOW = 0x4, PFD_SUPPORT_OPENGL = 0x20;

    [DllImport("opengl32.dll", CharSet = CharSet.Ansi, BestFitMapping = false)]
    private static extern nint wglGetProcAddress(string name);
    [DllImport("opengl32.dll")] public static extern nint wglCreateContext(nint hdc);
    [DllImport("opengl32.dll")] public static extern bool wglMakeCurrent(nint hdc, nint hglrc);
    [DllImport("opengl32.dll")] public static extern bool wglDeleteContext(nint hglrc);
    [DllImport("gdi32.dll")] public static extern int ChoosePixelFormat(nint hdc, ref PixelFormatDescriptor pfd);
    [DllImport("gdi32.dll")] public static extern bool SetPixelFormat(nint hdc, int format, ref PixelFormatDescriptor pfd);
    [DllImport("gdi32.dll")] public static extern int GetPixelFormat(nint hdc);
    [DllImport("gdi32.dll")] public static extern bool SwapBuffers(nint hdc);
    [DllImport("user32.dll")] public static extern nint GetDC(nint hwnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(nint hwnd, nint hdc);

    public const int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091, WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
    public const int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126, WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x1;
}