using System.Drawing;
using NXStream.Linux;

namespace NXStream.Render;

public sealed partial class GlRenderer
{
    private nint _context;

    private partial bool ContextReady() => _context != 0 && _hwnd != 0;

    private partial bool AttachWindow(nint hwnd)
    {
        ReleaseWindow();
        _hwnd = hwnd;
        if (_context == 0)
        {
            _context = Sdl.SDL_GL_CreateContext(hwnd);
            if (_context == 0) return Fail("SDL_GL_CreateContext: " + Sdl.Error());
            Gl.ProcLoader = Sdl.SDL_GL_GetProcAddress;
            return InitGl("3.3 core");
        }
        if (Sdl.SDL_GL_MakeCurrent(hwnd, _context) != 0) return Fail("SDL_GL_MakeCurrent: " + Sdl.Error());
        _wake.Set();
        return true;
    }

    private partial string PrepareContext()
    {
        var swap = Sdl.SDL_GL_SetSwapInterval(0) == 0;
        Sdl.SDL_GL_GetAttribute(Sdl.GlContextProfileMask, out var profile);
        return $"video driver {Sdl.VideoDriver()}, swap interval 0 {(swap ? "set" : "unavailable")}, profile mask {profile}";
    }

    private partial void ReleaseWindow()
    {
        if (_hwnd == 0) return;
        Sdl.SDL_GL_MakeCurrent(0, 0);
        _hwnd = 0;
    }

    private partial void DestroyContext()
    {
        if (_context == 0) return;
        if (_hwnd != 0 && Sdl.SDL_GL_MakeCurrent(_hwnd, _context) == 0) DeleteGlResources();
        ReleaseWindow();
        Sdl.SDL_GL_DeleteContext(_context);
        _context = 0;
    }

    private partial void SwapAndFinish()
    {
        Sdl.SDL_GL_SwapWindow(_hwnd);
        Gl.glFinish();
    }

    private partial bool PrepareVsr(bool wanted, bool newFrame, Size dest) => false;

    private partial void DrawVsr(Size dest)
    {
    }

    private partial string? VsrName() => null;

    private partial bool VsrDriverOff() => false;

    private partial bool? VsrActive() => null;

    private partial string? VsrProblem() => "not available on Linux";
}
