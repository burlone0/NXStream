using System.Diagnostics;
using System.Drawing;

namespace NXStream.Render;

public sealed partial class GlRenderer : IDisposable
{
    private enum Command { Attach, Detach }

    private readonly Thread _thread;
    private readonly AutoResetEvent _wake = new(false);
    private readonly object _lock = new();
    private readonly Queue<(Command Cmd, nint Hwnd, ManualResetEventSlim Done)> _commands = new();
    private volatile bool _stop;

    private byte[] _pending = [];
    private int _pendingWidth, _pendingHeight;
    private bool _hasPending;
    private double _pendingAt;
    private Size _viewSize;
    private RenderSettings _settings = new(Scaler.Sharp, 0);
    private OverlayImage? _overlay;
    private OverlayImage? _message;
    private OverlayImage? _menu;
    private bool _clearPicture;

    private nint _hwnd;
    private uint _vao, _present, _sharpen, _frameTex, _overlayTex, _messageTex, _menuTex;
    private int _uSrc, _uSize, _uMode, _uOpacity, _uDstSize, _uSharpSrc, _uSharpness;
    private uint _fbo, _fboTex;
    private Size _fboSize;
    private int _frameWidth, _frameHeight;
    private bool _hasFrame;
    private bool _vsrShown;
    private string _glVendor = "";
    private Scaler _shownScaler;
    private Size _shownSize;
    private (bool Vsr, Scaler Scaler, int Sharpness) _loggedKey;
    private double _statStart, _statSum, _statMax;
    private int _statFrames;
    private int _overlayW, _overlayH, _messageW, _messageH, _menuW, _menuH;
    private OverlayImage? _overlayUploaded, _messageUploaded, _menuUploaded;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private double _latencyAverage;
    private byte[] _uploading = [];

    public event Action<string>? Failed;

    public string GpuName { get; private set; } = "";

    public GlRenderer()
    {
        _thread = new Thread(Loop) { IsBackground = true, Name = "NXStream renderer", Priority = ThreadPriority.AboveNormal };
        _thread.Start();
    }

    public void Attach(nint hwnd) => Run(Command.Attach, hwnd, wait: false);

    public void Detach() => Run(Command.Detach, 0, wait: true);

    private void Run(Command cmd, nint hwnd, bool wait)
    {
        var done = new ManualResetEventSlim(false);
        lock (_lock) _commands.Enqueue((cmd, hwnd, done));
        _wake.Set();
        if (wait && _thread.IsAlive) done.Wait(TimeSpan.FromSeconds(2));
    }

    public void Present(ReadOnlySpan<byte> bgra, int width, int height)
    {
        lock (_lock)
        {
            var size = width * height * 4;
            if (_pending.Length != size) _pending = new byte[size];
            bgra[..size].CopyTo(_pending);
            _pendingWidth = width;
            _pendingHeight = height;
            _hasPending = true;
            _pendingAt = _clock.Elapsed.TotalMilliseconds;
        }
        _wake.Set();
    }

    public void SetViewSize(Size size)
    {
        lock (_lock) _viewSize = size;
        _wake.Set();
    }

    public void Configure(RenderSettings settings)
    {
        lock (_lock) _settings = settings;
        _wake.Set();
    }

    public void SetOverlay(OverlayImage? overlay)
    {
        lock (_lock) _overlay = overlay;
        _wake.Set();
    }

    public void SetMenu(OverlayImage? menu)
    {
        lock (_lock) _menu = menu;
        _wake.Set();
    }

    public void SetMessage(OverlayImage? message)
    {
        lock (_lock)
        {
            _message = message;
            if (message is not null) _clearPicture = true;
        }
        _wake.Set();
    }

    public void Redraw() => _wake.Set();

    private TaskCompletionSource<(byte[] Bgra, int Width, int Height)>? _snapshot;

    public Task<(byte[] Bgra, int Width, int Height)> SnapshotAsync()
    {
        var tcs = new TaskCompletionSource<(byte[], int, int)>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_lock) _snapshot = tcs;
        _wake.Set();
        return tcs.Task;
    }

    public RenderInfo Info
    {
        get
        {
            RenderSettings s;
            lock (_lock) s = _settings;
            var vsr = VsrName();
            var size = _shownSize.IsEmpty
                ? $"{_frameWidth}×{_frameHeight}"
                : $"{_frameWidth}×{_frameHeight} → {_shownSize.Width}×{_shownSize.Height}";
            var scaler = _vsrShown && vsr is not null ? vsr : ScalerName(_shownScaler);
            if (s.Sharpness > 0) scaler += $" + sharpening {s.Sharpness}%";
            var problem = s.Scaler == Scaler.SuperResolution ? VsrProblem() : null;
            return new RenderInfo(scaler, size, GpuName, _latencyAverage, problem);
        }
    }

    private static string ScalerName(Scaler scaler) => scaler switch
    {
        Scaler.Smooth => "Smooth (bilinear)",
        Scaler.Fsr => "AMD FidelityFX FSR 1",
        Scaler.SuperResolution => "Super Resolution",
        _ => "Sharp (Catmull-Rom)",
    };

    private void Loop()
    {
        try
        {
            while (!_stop)
            {
                _wake.WaitOne();
                if (_stop) break;
                if (!ProcessCommands()) return;
                if (ContextReady()) Render();
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Core.Log.Error("render", "render thread crashed", ex);
            Failed?.Invoke("OpenGL: " + ex.Message);
        }
        finally
        {
            DestroyContext();
        }
    }

    private bool ProcessCommands()
    {
        while (true)
        {
            (Command Cmd, nint Hwnd, ManualResetEventSlim Done) item;
            lock (_lock)
            {
                if (_commands.Count == 0) return true;
                item = _commands.Dequeue();
            }
            try
            {
                if (item.Cmd == Command.Detach)
                {
                    Core.Log.Write("render", $"detach from window 0x{_hwnd:x}");
                    ReleaseWindow();
                }
                else
                {
                    Core.Log.Write("render", $"attach to window 0x{item.Hwnd:x}");
                    if (!AttachWindow(item.Hwnd)) return false;
                }
            }
            finally
            {
                item.Done.Set();
            }
        }
    }

    private unsafe bool InitGl(string profile)
    {
        try
        {
            Gl.Load();
            var details = PrepareContext();
            GpuName = Gl.GetString(Gl.RENDERER).Split('/')[0].Trim();
            _glVendor = Gl.GetString(Gl.VENDOR);
            Core.Log.Write("render", $"OpenGL {profile} context: {Gl.GetString(Gl.VERSION)} | {_glVendor} | " +
                                     $"{Gl.GetString(Gl.RENDERER)} | {details}");

            _present = Gl.Program(GlShaders.VertexShader, GlShaders.PresentShader);
            _uSrc = Gl.Uniform(_present, "src");
            _uSize = Gl.Uniform(_present, "src_size");
            _uMode = Gl.Uniform(_present, "mode");
            _sharpen = Gl.Program(GlShaders.VertexShader, GlShaders.SharpenShader);
            _uSharpSrc = Gl.Uniform(_sharpen, "src");
            _uSharpness = Gl.Uniform(_sharpen, "sharpness");
            _uOpacity = Gl.Uniform(_present, "opacity");
            _uDstSize = Gl.Uniform(_present, "dst_size");
            uint vao;
            Gl.GenVertexArrays(1, &vao);
            _vao = vao;
            Gl.BindVertexArray(_vao);
            Gl.glPixelStorei(Gl.UNPACK_ALIGNMENT, 4);
        }
        catch (Exception ex) when (ex is NotSupportedException or InvalidOperationException)
        {
            return Fail(ex.Message);
        }
        _wake.Set();
        return true;
    }

    private bool Fail(string reason)
    {
        Core.Log.Write("render", $"OpenGL failure: {reason}");
        Failed?.Invoke("OpenGL unavailable: " + reason);
        return false;
    }

    private void DeleteGlResources()
    {
            Gl.DeleteTexture(_frameTex);
            Gl.DeleteTexture(_overlayTex);
            Gl.DeleteTexture(_messageTex);
            Gl.DeleteTexture(_menuTex);
            Gl.DeleteTexture(_fboTex);
            if (_fbo != 0)
            {
                var fbo = _fbo;
                unsafe { Gl.DeleteFramebuffers(1, &fbo); }
            }
    }

    private unsafe void Render()
    {
        bool newFrame;
        int width = 0, height = 0;
        double arrivedAt = 0;
        Size view;
        RenderSettings settings;
        OverlayImage? overlay, message, menu;
        lock (_lock)
        {
            newFrame = _hasPending;
            if (newFrame)
            {
                (_pending, _uploading) = (_uploading, _pending);
                width = _pendingWidth;
                height = _pendingHeight;
                arrivedAt = _pendingAt;
                _hasPending = false;
                _message = null;
            }
            if (_clearPicture && !newFrame) _hasFrame = false;
            _clearPicture = false;
            view = _viewSize;
            settings = _settings;
            overlay = _overlay;
            message = _message;
            menu = _menu;
        }
        if (view.Width <= 0 || view.Height <= 0) return;

        if (newFrame)
        {
            if (_frameTex == 0 || width != _frameWidth || height != _frameHeight)
            {
                Gl.DeleteTexture(_frameTex);
                _frameTex = Gl.Texture(width, height, Gl.LINEAR);
                _frameWidth = width;
                _frameHeight = height;
                Core.Log.Write("render", $"source frames {width}x{height}");
            }
            Gl.glBindTexture(Gl.TEXTURE_2D, _frameTex);
            fixed (byte* p = _uploading)
                Gl.glTexSubImage2D(Gl.TEXTURE_2D, 0, 0, 0, width, height, Gl.BGRA, Gl.UNSIGNED_BYTE, p);
            _hasFrame = true;
        }

        Rectangle dest = default;
        if (_hasFrame)
        {
            var scale = Math.Min((double)view.Width / _frameWidth, (double)view.Height / _frameHeight);
            var w = Math.Max(1, (int)Math.Round(_frameWidth * scale));
            var h = Math.Max(1, (int)Math.Round(_frameHeight * scale));
            dest = new Rectangle((view.Width - w) / 2, (view.Height - h) / 2, w, h);
        }

        var upscaling = _hasFrame && (dest.Width > _frameWidth || dest.Height > _frameHeight);
        var useVsr = PrepareVsr(settings.Scaler == Scaler.SuperResolution && upscaling, newFrame, dest.Size);
        var scaler = settings.Scaler == Scaler.SuperResolution ? Scaler.Sharp : settings.Scaler;
        var logKey = (useVsr, scaler, settings.Sharpness);
        if (_hasFrame && logKey != _loggedKey)
        {
            Core.Log.Write("render", $"output {dest.Width}x{dest.Height} at {dest.X},{dest.Y} in {view.Width}x{view.Height}: " +
                                     (useVsr ? VsrName()! : ScalerName(scaler)) + $", sharpness {settings.Sharpness}%" +
                                     (VsrDriverOff() ? " (driver super resolution off)" : ""));
            _loggedKey = logKey;
        }
        _vsrShown = useVsr;
        _shownScaler = scaler;
        _shownSize = _hasFrame ? dest.Size : Size.Empty;

        Gl.glViewport(0, 0, view.Width, view.Height);
        Gl.glClearColor(0, 0, 0, 1);
        Gl.glClear(Gl.COLOR_BUFFER_BIT);
        Gl.UseProgram(_present);

        if (_hasFrame)
        {
            var sharpen = settings.Sharpness > 0 && EnsureFbo(dest.Size);
            if (sharpen)
            {
                Gl.BindFramebuffer(Gl.FRAMEBUFFER, _fbo);
                Gl.glViewport(0, 0, dest.Width, dest.Height);
            }
            else
            {
                Gl.glViewport(dest.X, view.Height - dest.Bottom, dest.Width, dest.Height);
            }
            Gl.Uniform2f(_uDstSize, dest.Width, dest.Height);
            if (useVsr)
            {
                DrawVsr(dest.Size);
            }
            else
            {
                var mode = scaler switch { Scaler.Smooth => 0, Scaler.Fsr => 2, _ => 1 };
                DrawTexture(_frameTex, _frameWidth, _frameHeight, mode, 1f);
            }
            if (sharpen)
            {
                Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
                Gl.glViewport(dest.X, view.Height - dest.Bottom, dest.Width, dest.Height);
                Gl.UseProgram(_sharpen);
                Gl.ActiveTexture(Gl.TEXTURE0);
                Gl.glBindTexture(Gl.TEXTURE_2D, _fboTex);
                Gl.Uniform1i(_uSharpSrc, 0);
                Gl.Uniform1f(_uSharpness, MathF.Pow(2f, -2f * (1f - settings.Sharpness / 100f)));
                Gl.glDrawArrays(Gl.TRIANGLES, 0, 3);
                Gl.UseProgram(_present);
            }
        }

        Gl.glEnable(Gl.BLEND);
        Gl.glBlendFunc(Gl.SRC_ALPHA, Gl.ONE_MINUS_SRC_ALPHA);
        if (message is not null)
        {
            UploadOverlay(message, ref _messageTex, ref _messageW, ref _messageH, ref _messageUploaded);
            DrawOverlay(_messageTex, message, view);
        }
        if (overlay is not null)
        {
            UploadOverlay(overlay, ref _overlayTex, ref _overlayW, ref _overlayH, ref _overlayUploaded);
            DrawOverlay(_overlayTex, overlay, view);
        }
        if (menu is not null)
        {
            UploadOverlay(menu, ref _menuTex, ref _menuW, ref _menuH, ref _menuUploaded);
            DrawOverlay(_menuTex, menu, view);
        }
        Gl.glDisable(Gl.BLEND);

        TaskCompletionSource<(byte[], int, int)>? snapshot;
        lock (_lock) (snapshot, _snapshot) = (_snapshot, null);
        if (snapshot is not null)
        {
            var pixels = new byte[view.Width * view.Height * 4];
            Gl.glPixelStorei(Gl.PACK_ALIGNMENT, 4);
            fixed (byte* p = pixels) Gl.glReadPixels(0, 0, view.Width, view.Height, Gl.BGRA, Gl.UNSIGNED_BYTE, p);
            snapshot.SetResult((pixels, view.Width, view.Height));
        }

        SwapAndFinish();
        if (newFrame)
        {
            var latency = _clock.Elapsed.TotalMilliseconds - arrivedAt;
            _latencyAverage = _latencyAverage == 0 ? latency : _latencyAverage * 0.9 + latency * 0.1;
            TrackStats(latency);
        }
    }

    private void TrackStats(double latency)
    {
        _statFrames++;
        _statSum += latency;
        _statMax = Math.Max(_statMax, latency);
        var now = _clock.Elapsed.TotalMilliseconds;
        if (now - _statStart < 10000) return;
        Core.Log.Write("render", $"{_statFrames / ((now - _statStart) / 1000):0.0} fps drawn, " +
                                 $"frame to screen avg {_statSum / _statFrames:0.00} ms " +
                                 $"max {_statMax:0.00} ms, super resolution {(_vsrShown ? "applied" : "not applied")}" +
                                 (VsrActive() is { } active ? $" (driver {(active ? "on" : "off")})" : ""));
        _statStart = now;
        _statFrames = 0;
        _statSum = _statMax = 0;
    }

    private unsafe bool EnsureFbo(Size size)
    {
        if (_fbo != 0 && _fboSize == size) return true;
        if (_fbo == 0)
        {
            uint fbo;
            Gl.GenFramebuffers(1, &fbo);
            _fbo = fbo;
        }
        Gl.DeleteTexture(_fboTex);
        _fboTex = Gl.Texture(size.Width, size.Height, Gl.NEAREST);
        Gl.BindFramebuffer(Gl.FRAMEBUFFER, _fbo);
        Gl.FramebufferTexture2D(Gl.FRAMEBUFFER, Gl.COLOR_ATTACHMENT0, Gl.TEXTURE_2D, _fboTex, 0);
        var ok = Gl.CheckFramebufferStatus(Gl.FRAMEBUFFER) == Gl.FRAMEBUFFER_COMPLETE;
        Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
        _fboSize = size;
        Core.Log.Write("render", $"sharpening buffer {size.Width}x{size.Height}: {(ok ? "ok" : "incomplete, sharpening off")}");
        if (!ok) _fboSize = Size.Empty;
        return ok;
    }

    private unsafe void DrawTexture(uint tex, int w, int h, int mode, float opacity)
    {
        Gl.ActiveTexture(Gl.TEXTURE0);
        Gl.glBindTexture(Gl.TEXTURE_2D, tex);
        Gl.Uniform1i(_uSrc, 0);
        Gl.Uniform2f(_uSize, w, h);
        Gl.Uniform1i(_uMode, mode);
        Gl.Uniform1f(_uOpacity, opacity);
        Gl.glDrawArrays(Gl.TRIANGLES, 0, 3);
    }

    private static unsafe void UploadOverlay(OverlayImage image, ref uint tex, ref int w, ref int h, ref OverlayImage? uploaded)
    {
        if (ReferenceEquals(uploaded, image)) return;
        if (tex == 0 || w != image.Width || h != image.Height)
        {
            Gl.DeleteTexture(tex);
            tex = Gl.Texture(image.Width, image.Height, Gl.NEAREST);
            w = image.Width;
            h = image.Height;
        }
        Gl.glBindTexture(Gl.TEXTURE_2D, tex);
        fixed (byte* p = image.Pixels)
            Gl.glTexSubImage2D(Gl.TEXTURE_2D, 0, 0, 0, image.Width, image.Height, Gl.BGRA, Gl.UNSIGNED_BYTE, p);
        uploaded = image;
    }

    private void DrawOverlay(uint tex, OverlayImage image, Size view)
    {
        var x = image.Centered ? (view.Width - image.Width) / 2 : image.X >= 0 ? image.X : 12;
        var top = image.Centered ? (view.Height - image.Height) / 2 : image.Y >= 0 ? image.Y : 12;
        Gl.glViewport(x, view.Height - top - image.Height, image.Width, image.Height);
        DrawTexture(tex, image.Width, image.Height, 0, image.Opacity);
    }

    public void Dispose()
    {
        _stop = true;
        _wake.Set();
        if (Thread.CurrentThread != _thread) _thread.Join(TimeSpan.FromSeconds(2));
    }

    private partial bool ContextReady();
    private partial bool AttachWindow(nint hwnd);
    private partial void ReleaseWindow();
    private partial void DestroyContext();
    private partial string PrepareContext();
    private partial void SwapAndFinish();
    private partial bool PrepareVsr(bool wanted, bool newFrame, Size dest);
    private partial void DrawVsr(Size dest);
    private partial string? VsrName();
    private partial bool VsrDriverOff();
    private partial bool? VsrActive();
    private partial string? VsrProblem();
}
