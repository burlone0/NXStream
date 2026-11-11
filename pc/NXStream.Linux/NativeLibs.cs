using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NXStream.Linux;

internal static class NativeLibs
{
    private const string Sdl2 = "libSDL2-2.0.so.0";

    [ModuleInitializer]
    internal static void Register() => NativeLibrary.SetDllImportResolver(typeof(NativeLibs).Assembly, Resolve);

    private static IntPtr Resolve(string name, Assembly assembly, DllImportSearchPath? path)
    {
        if (name == "opengl32.dll")
            return NativeLibrary.TryLoad("libGL.so.1", out var gl) ? gl
                 : NativeLibrary.TryLoad("libOpenGL.so.0", out var opengl) ? opengl
                 : IntPtr.Zero;

        if (name == "libusb-1.0")
        {
            const string usb = "libusb-1.0.so.0";
            var root = Environment.GetEnvironmentVariable("APPDIR");
            if (!string.IsNullOrEmpty(root) && NativeLibrary.TryLoad(Path.Combine(root, "usr", "lib", usb), out var bundledUsb))
                return bundledUsb;
            return NativeLibrary.TryLoad(usb, out var systemUsb) ? systemUsb : IntPtr.Zero;
        }

        if (!name.StartsWith("libSDL2", StringComparison.Ordinal)) return IntPtr.Zero;
        if (name != Sdl2) Resolve(Sdl2, assembly, path);
        var appDir = Environment.GetEnvironmentVariable("APPDIR");
        if (!string.IsNullOrEmpty(appDir) && NativeLibrary.TryLoad(Path.Combine(appDir, "usr", "lib", name), out var bundled))
            return bundled;
        return NativeLibrary.TryLoad(name, assembly, path, out var system) ? system : IntPtr.Zero;
    }
}
