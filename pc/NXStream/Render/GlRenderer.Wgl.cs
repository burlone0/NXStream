using System.Drawing;

namespace NXStream.Render;

public sealed partial class GlRenderer
{
    private nint _hdc, _context;
    private int _pixelFormat;
    private Gl.PixelFormatDescriptor _pfd;
    private VideoSuperResolution? _vsr;
    private string? _vsrError;
    private bool _vsrTried;
    private Size _vsrFor;
    private VideoSuperResolution? _vsrNow;
    private Size _vsrPending;
    private double _vsrPendingSince;
    private System.Threading.Timer? _vsrSettle;
    private const double VsrSettleMs = 150;

    private partial bool ContextReady() => _context != 0 && _hdc != 0;

    private partial bool AttachWindow(nint hwnd)
    {
        ReleaseWindow();
        _hwnd = hwnd;
        _hdc = Gl.GetDC(hwnd);
        if (_hdc == 0) return Fail("GetDC failed");

        if (_context == 0)
        {
            _pfd = new Gl.PixelFormatDescriptor
            {
                nSize = (ushort)System.Runtime.InteropServices.Marshal.SizeOf<Gl.PixelFormatDescriptor>(),
                nVersion = 1,
                dwFlags = Gl.PFD_DRAW_TO_WINDOW | Gl.PFD_SUPPORT_OPENGL | Gl.PFD_DOUBLEBUFFER,
                cColorBits = 32,
            };
            _pixelFormat = Gl.ChoosePixelFormat(_hdc, ref _pfd);
        }
        if (_pixelFormat == 0 || (Gl.GetPixelFormat(_hdc) != _pixelFormat && !Gl.SetPixelFormat(_hdc, _pixelFormat, ref _pfd)))
            return Fail("pixel format not supported");

        if (_context == 0) return CreateContext();
        if (!Gl.wglMakeCurrent(_hdc, _context)) return Fail("wglMakeCurrent failed");
        _wake.Set();
        return true;
    }

    private unsafe bool CreateContext()
    {
        var legacy = Gl.wglCreateContext(_hdc);
        if (legacy == 0) return Fail("no OpenGL driver");
        if (!Gl.wglMakeCurrent(_hdc, legacy))
        {
            Gl.wglDeleteContext(legacy);
            return Fail("no OpenGL driver");
        }

        var createAttribs = (delegate* unmanaged<nint, nint, int*, nint>)Gl.WglProc("wglCreateContextAttribsARB");
        var profile = "legacy";
        _context = legacy;
        if (createAttribs != null)
        {
            var attribs = stackalloc int[]
            {
                Gl.WGL_CONTEXT_MAJOR_VERSION_ARB, 3, Gl.WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                Gl.WGL_CONTEXT_PROFILE_MASK_ARB, Gl.WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0,
            };
            var core = createAttribs(_hdc, 0, attribs);
            if (core != 0 && Gl.wglMakeCurrent(_hdc, core))
            {
                Gl.wglDeleteContext(legacy);
                _context = core;
                profile = "3.3 core";
            }
            else
            {
                Gl.wglMakeCurrent(_hdc, legacy);
            }
        }

        Gl.ProcLoader = Gl.WglProc;
        return InitGl(profile);
    }

    private partial string PrepareContext()
    {
        Gl.LoadWgl();
        unsafe { if (Gl.SwapIntervalEXT != null) Gl.SwapIntervalEXT(0); }
        unsafe
        {
            return $"pixel format {_pixelFormat}, swap interval 0 {(Gl.SwapIntervalEXT != null ? "set" : "unavailable")}, " +
                   $"WGL_NV_DX_interop {(Gl.HasDxInterop ? "yes" : "no")}";
        }
    }

    private partial void ReleaseWindow()
    {
        if (_hdc == 0) return;
        Gl.wglMakeCurrent(0, 0);
        Gl.ReleaseDC(_hwnd, _hdc);
        _hdc = 0;
        _hwnd = 0;
    }

    private partial void DestroyContext()
    {
        var current = _context != 0 && _hdc != 0 && Gl.wglMakeCurrent(_hdc, _context);
        if (current)
        {
            DisposeVsr();
            DeleteGlResources();
        }
        _vsrSettle?.Dispose();
        _vsrSettle = null;
        ReleaseWindow();
        if (_context == 0) return;
        Gl.wglDeleteContext(_context);
        _context = 0;
    }

    private partial void SwapAndFinish()
    {
        Gl.SwapBuffers(_hdc);
        Gl.glFinish();
    }

    private partial bool PrepareVsr(bool wanted, bool newFrame, Size dest)
    {
        var vsr = wanted ? EnsureVsr() : null;
        var now = _clock.Elapsed.TotalMilliseconds;
        if (vsr is not null && dest != _vsrPending)
        {
            _vsrPending = dest;
            _vsrPendingSince = now;
        }
        var settled = vsr is null || _vsrFor.IsEmpty || _vsrFor == dest || now - _vsrPendingSince >= VsrSettleMs;
        if (!settled) (_vsrSettle ??= new System.Threading.Timer(_ => _wake.Set())).Change((int)VsrSettleMs + 10, Timeout.Infinite);
        if (vsr is not null && settled && (newFrame || _vsrFor != dest))
        {
            try
            {
                vsr.Process(newFrame ? _uploading : null, _frameWidth, _frameHeight, dest.Width, dest.Height,
                            now);
                _vsrFor = dest;
            }
            catch (Exception ex) when (ex is SharpGen.Runtime.SharpGenException or NotSupportedException
                                           or System.Runtime.InteropServices.COMException)
            {
                _vsrError = FirstLine(ex.Message);
                Core.Log.Error("render", "super resolution disabled after an error", ex);
                DisposeVsr();
                vsr = null;
            }
        }
        _vsrNow = vsr;
        return vsr is not null && vsr.Active != false && _vsrFor == dest && vsr.Texture != 0;
    }

    private partial void DrawVsr(Size dest)
    {
        _vsrNow!.Lock();
        DrawTexture(_vsrNow.Texture, dest.Width, dest.Height, 0, 1f);
        _vsrNow.Unlock();
    }

    private partial string? VsrName() => _vsr?.Name;

    private partial bool VsrDriverOff() => _vsrNow is { Active: false };

    private partial bool? VsrActive() => _vsr?.Active;

    private partial string? VsrProblem()
    {
        var vsr = _vsr;
        if (_vsrError is not null) return "Super Resolution unavailable: " + _vsrError;
        if (vsr?.Active == false)
            return vsr.Name.StartsWith("RTX")
                ? "RTX Video Super Resolution is off: turn it on in the NVIDIA app (System → Video → RTX Video Enhancement)"
                : "Video Super Resolution is off in the Intel control panel";
        return null;
    }

    private VideoSuperResolution? EnsureVsr()
    {
        if (_vsrTried) return _vsr;
        _vsrTried = true;
        _vsr = VideoSuperResolution.TryCreate(_glVendor, out _vsrError);
        Core.Log.Write("render", _vsr is not null ? $"{_vsr.Name} ready on '{_vsr.AdapterName}'"
                                                  : $"super resolution unavailable: {_vsrError}");
        return _vsr;
    }

    private void DisposeVsr()
    {
        try { _vsr?.Dispose(); }
        catch (Exception ex) when (ex is SharpGen.Runtime.SharpGenException or System.Runtime.InteropServices.COMException) { }
        _vsr = null;
        _vsrFor = Size.Empty;
        _vsrShown = false;
    }

    private static string FirstLine(string text)
    {
        var line = text.Split('\n')[0].Trim();
        return line.Length > 120 ? line[..120] + "…" : line;
    }
}
