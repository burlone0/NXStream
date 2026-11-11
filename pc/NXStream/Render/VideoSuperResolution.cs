using System.Runtime.InteropServices;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace NXStream.Render;

internal sealed unsafe class VideoSuperResolution : IDisposable
{
    private enum Maker { Nvidia, Intel }

    private static readonly Guid NvidiaPpe = new(0xd43ce1b3, 0x1f4b, 0x48ac, 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7);
    private static readonly Guid IntelVpe = new(0xedd1d4b9, 0x8659, 0x4cbc, 0xa4, 0xd6, 0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3);

    private const int ProbeSize = 64;
    private const double ProbeEveryMsOff = 2000;
    private const double ProbeEveryMsOn = 10000;

    private readonly Maker _maker;
    private readonly ID3D11Device _device;
    private readonly ID3D11DeviceContext _context;
    private readonly ID3D11VideoDevice _videoDevice;
    private readonly ID3D11VideoContext _videoContext;
    private readonly nint _interopDevice;

    private ID3D11Texture2D? _input;
    private int _inputWidth, _inputHeight;
    private ID3D11VideoProcessorEnumerator? _enumerator;
    private ID3D11VideoProcessor? _processor;
    private ID3D11VideoProcessorInputView? _inputView;
    private ID3D11Texture2D? _output, _probeOutput, _probeA, _probeB;
    private ID3D11VideoProcessorOutputView? _outputView, _probeView;
    private int _outputWidth, _outputHeight;
    private uint _glTexture;
    private nint _interopObject;
    private uint _frameIndex;
    private double _lastProbeMs = double.NegativeInfinity;

    public string Name { get; }
    public string AdapterName { get; }

    public bool? Active { get; private set; }

    public uint Texture => _glTexture;

    private VideoSuperResolution(Maker maker, IDXGIAdapter1 adapter, string adapterName)
    {
        _maker = maker;
        AdapterName = adapterName;
        Name = maker == Maker.Nvidia ? "RTX Video Super Resolution (NVIDIA)" : "Video Super Resolution (Intel)";
        D3D11.D3D11CreateDevice(adapter, DriverType.Unknown, DeviceCreationFlags.BgraSupport | DeviceCreationFlags.VideoSupport,
            [FeatureLevel.Level_11_1, FeatureLevel.Level_11_0], out ID3D11Device device, out ID3D11DeviceContext context).CheckError();
        _device = device;
        _context = context;
        _videoDevice = device.QueryInterface<ID3D11VideoDevice>();
        _videoContext = context.QueryInterface<ID3D11VideoContext>();
        _interopDevice = Gl.DXOpenDeviceNV(device.NativePointer);
        if (_interopDevice == 0) throw new NotSupportedException("OpenGL cannot read Direct3D textures (WGL_NV_DX_interop)");
    }

    public static VideoSuperResolution? TryCreate(string glVendor, out string? why)
    {
        why = null;
        var maker = glVendor.Contains("NVIDIA", StringComparison.OrdinalIgnoreCase) ? Maker.Nvidia
                  : glVendor.Contains("Intel", StringComparison.OrdinalIgnoreCase) ? Maker.Intel
                  : (Maker?)null;
        if (maker is null)
        {
            why = "needs an NVIDIA RTX or Intel GPU (the monitor is connected to a " + glVendor.Split(' ')[0] + " GPU)";
            return null;
        }
        if (!Gl.HasDxInterop)
        {
            why = "the OpenGL driver does not support WGL_NV_DX_interop";
            return null;
        }
        var vendorId = maker == Maker.Nvidia ? 0x10DEu : 0x8086u;
        try
        {
            using var factory = DXGI.CreateDXGIFactory1<IDXGIFactory1>();
            for (uint i = 0; factory.EnumAdapters1(i, out var adapter).Success; i++)
            {
                using (adapter)
                {
                    var desc = adapter.Description1;
                    if (desc.VendorId != vendorId) continue;
                    var vsr = new VideoSuperResolution(maker.Value, adapter, desc.Description.Trim());
                    if (vsr.Supported()) return vsr;
                    vsr.Dispose();
                    why = maker == Maker.Nvidia
                        ? "the NVIDIA driver does not offer it (needs an RTX 20 series or newer and a recent driver)"
                        : "the Intel driver does not offer it";
                    return null;
                }
            }
            why = "no " + (maker == Maker.Nvidia ? "NVIDIA" : "Intel") + " GPU found by Direct3D";
        }
        catch (Exception ex) when (ex is SharpGen.Runtime.SharpGenException or NotSupportedException or COMException)
        {
            why = "Direct3D 11: " + ex.Message.Split('\n')[0].Trim();
        }
        return null;
    }

    private bool Supported()
    {
        if (_maker == Maker.Intel) return true;
        try
        {
            Configure(1280, 720, 1920, 1080);
            uint available = 0;
            var hr = _videoContext.VideoProcessorGetStreamExtension(_processor!, 0, NvidiaPpe, 4, (nint)(&available));
            NXStream.Core.Log.Write("render", $"NVIDIA super resolution extension query: hr 0x{hr.Code:x8}, available {available}");
            return hr.Success && available != 0;
        }
        catch (SharpGen.Runtime.SharpGenException)
        {
            return false;
        }
    }

    public void Process(byte[]? frame, int width, int height, int outWidth, int outHeight, double nowMs)
    {
        if (frame is not null)
        {
            EnsureInput(width, height);
            fixed (byte* p = frame) _context.UpdateSubresource(_input!, 0, null, (nint)p, (uint)(width * 4), 0);
        }
        if (_input is null) return;
        Configure(_inputWidth, _inputHeight, outWidth, outHeight);
        EnsureOutput(outWidth, outHeight);

        _videoContext.VideoProcessorBlt(_processor!, _outputView!, _frameIndex++, 1,
            [new VideoProcessorStream { Enable = true, InputSurface = _inputView! }]).CheckError();

        var every = Active == true ? ProbeEveryMsOn : ProbeEveryMsOff;
        if (nowMs - _lastProbeMs >= every)
        {
            _lastProbeMs = nowMs;
            var active = Probe();
            if (active != Active)
                NXStream.Core.Log.Write("render", active
                    ? $"{Name}: the driver is upscaling ({_inputWidth}x{_inputHeight} -> {outWidth}x{outHeight})"
                    : $"{Name}: the driver returns plain scaling, the feature is off in the driver settings");
            Active = active;
        }
        _context.Flush();
    }

    public void Lock()
    {
        var o = _interopObject;
        Gl.DXLockObjectsNV(_interopDevice, 1, &o);
    }

    public void Unlock()
    {
        var o = _interopObject;
        Gl.DXUnlockObjectsNV(_interopDevice, 1, &o);
    }

    private void EnsureInput(int width, int height)
    {
        if (_input is not null && width == _inputWidth && height == _inputHeight) return;
        _inputView?.Dispose();
        _inputView = null;
        _input?.Dispose();
        _input = _device.CreateTexture2D(new Texture2DDescription(Format.B8G8R8A8_UNorm, (uint)width, (uint)height, 1, 1,
            BindFlags.RenderTarget | BindFlags.ShaderResource));
        _inputWidth = width;
        _inputHeight = height;
    }

    private int _cfgInW, _cfgInH, _cfgOutW, _cfgOutH;

    private void Configure(int inW, int inH, int outW, int outH)
    {
        if (_processor is not null && (inW, inH, outW, outH) == (_cfgInW, _cfgInH, _cfgOutW, _cfgOutH))
        {
            if (_inputView is null && _input is not null) CreateInputView();
            return;
        }
        ReleaseProcessor();
        var content = new VideoProcessorContentDescription
        {
            InputFrameFormat = VideoFrameFormat.Progressive,
            InputFrameRate = new Rational(60, 1),
            InputWidth = (uint)inW,
            InputHeight = (uint)inH,
            OutputFrameRate = new Rational(60, 1),
            OutputWidth = (uint)outW,
            OutputHeight = (uint)outH,
            Usage = VideoUsage.PlaybackNormal,
        };
        _videoDevice.CreateVideoProcessorEnumerator(ref content, out _enumerator).CheckError();
        _videoDevice.CreateVideoProcessor(_enumerator!, 0, out _processor).CheckError();
        if ((inW, inH) != (_cfgInW, _cfgInH))
            NXStream.Core.Log.Write("render", $"D3D11 video processor {inW}x{inH} -> {outW}x{outH}, " +
                                              $"rate conversion caps {_enumerator!.VideoProcessorCaps.RateConversionCapsCount}");
        (_cfgInW, _cfgInH, _cfgOutW, _cfgOutH) = (inW, inH, outW, outH);

        var p = _processor!;
        _videoContext.VideoProcessorSetStreamFrameFormat(p, 0, VideoFrameFormat.Progressive);
        _videoContext.VideoProcessorSetStreamAutoProcessingMode(p, 0, false);
        _videoContext.VideoProcessorSetStreamSourceRect(p, 0, true, new Vortice.RawRect(0, 0, inW, inH));
        _videoContext.VideoProcessorSetStreamDestRect(p, 0, true, new Vortice.RawRect(0, 0, outW, outH));
        _videoContext.VideoProcessorSetOutputTargetRect(p, true, new Vortice.RawRect(0, 0, outW, outH));
        SetExtension(true);
        if (_input is not null) CreateInputView();
        Active = null;
        _lastProbeMs = double.NegativeInfinity;
    }

    private void CreateInputView()
    {
        _inputView?.Dispose();
        _videoDevice.CreateVideoProcessorInputView(_input!, _enumerator!,
            new VideoProcessorInputViewDescription { FourCC = 0, ViewDimension = VideoProcessorInputViewDimension.Texture2D },
            out _inputView).CheckError();
    }

    private void EnsureOutput(int width, int height)
    {
        if (_output is not null && width == _outputWidth && height == _outputHeight) return;
        ReleaseOutput();
        var desc = new Texture2DDescription(Format.B8G8R8A8_UNorm, (uint)width, (uint)height, 1, 1,
            BindFlags.RenderTarget | BindFlags.ShaderResource);
        _output = _device.CreateTexture2D(desc with { MiscFlags = ResourceOptionFlags.Shared });
        _probeOutput = _device.CreateTexture2D(desc);
        var viewDesc = new VideoProcessorOutputViewDescription { ViewDimension = VideoProcessorOutputViewDimension.Texture2D };
        _videoDevice.CreateVideoProcessorOutputView(_output, _enumerator!, viewDesc, out _outputView).CheckError();
        _videoDevice.CreateVideoProcessorOutputView(_probeOutput, _enumerator!, viewDesc, out _probeView).CheckError();
        var staging = new Texture2DDescription(Format.B8G8R8A8_UNorm, ProbeSize, ProbeSize, 1, 1, BindFlags.None,
            ResourceUsage.Staging, CpuAccessFlags.Read);
        _probeA = _device.CreateTexture2D(staging);
        _probeB = _device.CreateTexture2D(staging);

        uint tex;
        Gl.glGenTextures(1, &tex);
        _glTexture = tex;
        _interopObject = Gl.DXRegisterObjectNV(_interopDevice, _output.NativePointer, tex, Gl.TEXTURE_2D, Gl.WGL_ACCESS_READ_ONLY_NV);
        if (_interopObject == 0)
        {
            ReleaseOutput();
            throw new NotSupportedException("OpenGL cannot read the Direct3D image");
        }
        Gl.glBindTexture(Gl.TEXTURE_2D, tex);
        Gl.glTexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_MIN_FILTER, Gl.LINEAR);
        Gl.glTexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_MAG_FILTER, Gl.LINEAR);
        _outputWidth = width;
        _outputHeight = height;
    }

    private void SetExtension(bool on)
    {
        var p = _processor!;
        if (_maker == Maker.Nvidia)
        {
            var ext = stackalloc uint[] { 1, 2, on ? 1u : 0u };
            _videoContext.VideoProcessorSetStreamExtension(p, 0, NvidiaPpe, 12, (nint)ext);
            return;
        }
        uint value;
        var call = stackalloc nint[2];
        call[1] = (nint)(&value);
        value = 3;
        ((uint*)call)[0] = 0x01;
        _videoContext.VideoProcessorSetOutputExtension(p, IntelVpe, (uint)(sizeof(nint) * 2), (nint)call);
        value = on ? 1u : 0u;
        ((uint*)call)[0] = 0x20;
        _videoContext.VideoProcessorSetOutputExtension(p, IntelVpe, (uint)(sizeof(nint) * 2), (nint)call);
        value = on ? 2u : 0u;
        ((uint*)call)[0] = 0x37;
        _videoContext.VideoProcessorSetStreamExtension(p, 0, IntelVpe, (uint)(sizeof(nint) * 2), (nint)call);
    }

    private bool Probe()
    {
        var x = Math.Max(0, _outputWidth / 2 - ProbeSize / 2);
        var y = Math.Max(0, _outputHeight / 2 - ProbeSize / 2);
        var box = new Vortice.Mathematics.Box(x, y, 0, x + ProbeSize, y + ProbeSize, 1);
        var stream = new[] { new VideoProcessorStream { Enable = true, InputSurface = _inputView! } };

        _videoContext.VideoProcessorBlt(_processor!, _probeView!, _frameIndex, 1, stream).CheckError();
        _context.CopySubresourceRegion(_probeA!, 0, 0, 0, 0, _probeOutput!, 0, box);
        SetExtension(false);
        _videoContext.VideoProcessorBlt(_processor!, _probeView!, _frameIndex, 1, stream).CheckError();
        SetExtension(true);
        _context.CopySubresourceRegion(_probeB!, 0, 0, 0, 0, _probeOutput!, 0, box);
        var a = _context.Map(_probeA!, 0, MapMode.Read);
        var b = _context.Map(_probeB!, 0, MapMode.Read);
        try
        {
            for (var row = 0; row < ProbeSize; row++)
            {
                var ra = new ReadOnlySpan<byte>((byte*)a.DataPointer + row * a.RowPitch, ProbeSize * 4);
                var rb = new ReadOnlySpan<byte>((byte*)b.DataPointer + row * b.RowPitch, ProbeSize * 4);
                if (!ra.SequenceEqual(rb)) return true;
            }
            return false;
        }
        finally
        {
            _context.Unmap(_probeA!, 0);
            _context.Unmap(_probeB!, 0);
        }
    }

    private void ReleaseOutput()
    {
        if (_interopObject != 0) Gl.DXUnregisterObjectNV(_interopDevice, _interopObject);
        _interopObject = 0;
        Gl.DeleteTexture(_glTexture);
        _glTexture = 0;
        _outputView?.Dispose();
        _probeView?.Dispose();
        _output?.Dispose();
        _probeOutput?.Dispose();
        _probeA?.Dispose();
        _probeB?.Dispose();
        _outputView = _probeView = null;
        _output = _probeOutput = _probeA = _probeB = null;
        _outputWidth = _outputHeight = 0;
    }

    private void ReleaseProcessor()
    {
        ReleaseOutput();
        _inputView?.Dispose();
        _processor?.Dispose();
        _enumerator?.Dispose();
        _inputView = null;
        _processor = null;
        _enumerator = null;
    }

    public void Dispose()
    {
        ReleaseProcessor();
        _input?.Dispose();
        if (_interopDevice != 0) Gl.DXCloseDeviceNV(_interopDevice);
        _videoContext.Dispose();
        _videoDevice.Dispose();
        _context.Dispose();
        _device.Dispose();
    }
}
