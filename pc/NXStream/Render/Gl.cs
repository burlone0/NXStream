using System.Runtime.InteropServices;
using System.Text;

namespace NXStream.Render;

internal static unsafe partial class Gl
{
    public const uint TEXTURE_2D = 0x0DE1, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800;
    private const uint TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803;
    public const int LINEAR = 0x2601, NEAREST = 0x2600, RGBA8 = 0x8058;
    private const int CLAMP_TO_EDGE = 0x812F;
    public const uint BGRA = 0x80E1, UNSIGNED_BYTE = 0x1401;
    private const uint FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31;
    private const uint COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84;
    public const uint TEXTURE0 = 0x84C0, TRIANGLES = 0x0004, COLOR_BUFFER_BIT = 0x4000;
    public const uint BLEND = 0x0BE2, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303;
    public const uint UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05;
    public const uint VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02;
    public const uint FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5;

    [DllImport("opengl32.dll")] public static extern void glClearColor(float r, float g, float b, float a);
    [DllImport("opengl32.dll")] public static extern void glClear(uint mask);
    [DllImport("opengl32.dll")] public static extern void glViewport(int x, int y, int width, int height);
    [DllImport("opengl32.dll")] public static extern void glEnable(uint cap);
    [DllImport("opengl32.dll")] public static extern void glDisable(uint cap);
    [DllImport("opengl32.dll")] public static extern void glBlendFunc(uint src, uint dst);
    [DllImport("opengl32.dll")] public static extern void glFinish();
    [DllImport("opengl32.dll")] private static extern nint glGetString(uint name);
    [DllImport("opengl32.dll")] public static extern void glGenTextures(int n, uint* textures);
    [DllImport("opengl32.dll")] private static extern void glDeleteTextures(int n, uint* textures);
    [DllImport("opengl32.dll")] public static extern void glBindTexture(uint target, uint texture);
    [DllImport("opengl32.dll")] public static extern void glTexParameteri(uint target, uint name, int value);
    [DllImport("opengl32.dll")] public static extern void glPixelStorei(uint name, int value);
    [DllImport("opengl32.dll")]
    private static extern void glTexImage2D(uint target, int level, int internalFormat, int width, int height, int border,
                                            uint format, uint type, void* pixels);
    [DllImport("opengl32.dll")]
    public static extern void glTexSubImage2D(uint target, int level, int x, int y, int width, int height,
                                              uint format, uint type, void* pixels);
    [DllImport("opengl32.dll")]
    public static extern void glReadPixels(int x, int y, int width, int height, uint format, uint type, void* pixels);
    [DllImport("opengl32.dll")] public static extern void glDrawArrays(uint mode, int first, int count);

    public static string GetString(uint name) => Marshal.PtrToStringAnsi(glGetString(name)) ?? "";

    private static delegate* unmanaged<uint, uint> CreateShader;
    private static delegate* unmanaged<uint, int, byte**, int*, void> ShaderSource;
    private static delegate* unmanaged<uint, void> CompileShader;
    private static delegate* unmanaged<uint, uint, int*, void> GetShaderiv;
    private static delegate* unmanaged<uint, int, int*, byte*, void> GetShaderInfoLog;
    private static delegate* unmanaged<uint, void> DeleteShader;
    private static delegate* unmanaged<uint> CreateProgram;
    private static delegate* unmanaged<uint, uint, void> AttachShader;
    private static delegate* unmanaged<uint, void> LinkProgram;
    private static delegate* unmanaged<uint, uint, int*, void> GetProgramiv;
    private static delegate* unmanaged<uint, int, int*, byte*, void> GetProgramInfoLog;
    public static delegate* unmanaged<uint, void> UseProgram;
    private static delegate* unmanaged<uint, void> DeleteProgram;
    private static delegate* unmanaged<uint, byte*, int> GetUniformLocation;
    public static delegate* unmanaged<int, int, void> Uniform1i;
    public static delegate* unmanaged<int, float, void> Uniform1f;
    public static delegate* unmanaged<int, float, float, void> Uniform2f;
    public static delegate* unmanaged<uint, void> ActiveTexture;
    public static delegate* unmanaged<int, uint*, void> GenVertexArrays;
    public static delegate* unmanaged<uint, void> BindVertexArray;
    public static delegate* unmanaged<int, uint*, void> GenFramebuffers;
    public static delegate* unmanaged<int, uint*, void> DeleteFramebuffers;
    public static delegate* unmanaged<uint, uint, void> BindFramebuffer;
    public static delegate* unmanaged<uint, uint, uint, uint, int, void> FramebufferTexture2D;
    public static delegate* unmanaged<uint, uint> CheckFramebufferStatus;

    public static Func<string, nint>? ProcLoader;

    public static void Load()
    {
        CreateShader = (delegate* unmanaged<uint, uint>)Proc("glCreateShader");
        ShaderSource = (delegate* unmanaged<uint, int, byte**, int*, void>)Proc("glShaderSource");
        CompileShader = (delegate* unmanaged<uint, void>)Proc("glCompileShader");
        GetShaderiv = (delegate* unmanaged<uint, uint, int*, void>)Proc("glGetShaderiv");
        GetShaderInfoLog = (delegate* unmanaged<uint, int, int*, byte*, void>)Proc("glGetShaderInfoLog");
        DeleteShader = (delegate* unmanaged<uint, void>)Proc("glDeleteShader");
        CreateProgram = (delegate* unmanaged<uint>)Proc("glCreateProgram");
        AttachShader = (delegate* unmanaged<uint, uint, void>)Proc("glAttachShader");
        LinkProgram = (delegate* unmanaged<uint, void>)Proc("glLinkProgram");
        GetProgramiv = (delegate* unmanaged<uint, uint, int*, void>)Proc("glGetProgramiv");
        GetProgramInfoLog = (delegate* unmanaged<uint, int, int*, byte*, void>)Proc("glGetProgramInfoLog");
        UseProgram = (delegate* unmanaged<uint, void>)Proc("glUseProgram");
        DeleteProgram = (delegate* unmanaged<uint, void>)Proc("glDeleteProgram");
        GetUniformLocation = (delegate* unmanaged<uint, byte*, int>)Proc("glGetUniformLocation");
        Uniform1i = (delegate* unmanaged<int, int, void>)Proc("glUniform1i");
        Uniform1f = (delegate* unmanaged<int, float, void>)Proc("glUniform1f");
        Uniform2f = (delegate* unmanaged<int, float, float, void>)Proc("glUniform2f");
        ActiveTexture = (delegate* unmanaged<uint, void>)Proc("glActiveTexture");
        GenVertexArrays = (delegate* unmanaged<int, uint*, void>)Proc("glGenVertexArrays");
        BindVertexArray = (delegate* unmanaged<uint, void>)Proc("glBindVertexArray");
        GenFramebuffers = (delegate* unmanaged<int, uint*, void>)Proc("glGenFramebuffers");
        DeleteFramebuffers = (delegate* unmanaged<int, uint*, void>)Proc("glDeleteFramebuffers");
        BindFramebuffer = (delegate* unmanaged<uint, uint, void>)Proc("glBindFramebuffer");
        FramebufferTexture2D = (delegate* unmanaged<uint, uint, uint, uint, int, void>)Proc("glFramebufferTexture2D");
        CheckFramebufferStatus = (delegate* unmanaged<uint, uint>)Proc("glCheckFramebufferStatus");
    }

    private static nint Proc(string name) =>
        TryProc(name) is var p && p != 0 ? p : throw new NotSupportedException($"OpenGL: missing {name}");

    public static nint TryProc(string name) => ProcLoader?.Invoke(name) ?? 0;

    public static uint Texture(int width, int height, int filter)
    {
        uint tex;
        glGenTextures(1, &tex);
        glBindTexture(TEXTURE_2D, tex);
        glTexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
        glTexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
        glTexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        glTexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        glTexImage2D(TEXTURE_2D, 0, RGBA8, width, height, 0, BGRA, UNSIGNED_BYTE, null);
        return tex;
    }

    public static void DeleteTexture(uint tex)
    {
        if (tex != 0) glDeleteTextures(1, &tex);
    }

    public static uint Program(string vertex, string fragment)
    {
        var vs = Shader(VERTEX_SHADER, vertex);
        var fs = Shader(FRAGMENT_SHADER, fragment);
        var prog = CreateProgram();
        AttachShader(prog, vs);
        AttachShader(prog, fs);
        LinkProgram(prog);
        DeleteShader(vs);
        DeleteShader(fs);
        int ok;
        GetProgramiv(prog, LINK_STATUS, &ok);
        if (ok == 0)
        {
            var log = InfoLog(prog, true);
            DeleteProgram(prog);
            throw new InvalidOperationException("link: " + log);
        }
        return prog;
    }

    private static uint Shader(uint type, string source)
    {
        var shader = CreateShader(type);
        var bytes = Encoding.UTF8.GetBytes(source);
        fixed (byte* p = bytes)
        {
            var ptr = p;
            var len = bytes.Length;
            ShaderSource(shader, 1, &ptr, &len);
        }
        CompileShader(shader);
        int ok;
        GetShaderiv(shader, COMPILE_STATUS, &ok);
        if (ok == 0)
        {
            var log = InfoLog(shader, false);
            DeleteShader(shader);
            throw new InvalidOperationException("compile: " + log);
        }
        return shader;
    }

    private static string InfoLog(uint obj, bool program)
    {
        int len;
        if (program) GetProgramiv(obj, INFO_LOG_LENGTH, &len);
        else GetShaderiv(obj, INFO_LOG_LENGTH, &len);
        if (len <= 1) return "(no details)";
        var buf = new byte[len];
        fixed (byte* p = buf)
        {
            if (program) GetProgramInfoLog(obj, len, null, p);
            else GetShaderInfoLog(obj, len, null, p);
        }
        return Encoding.UTF8.GetString(buf).TrimEnd('\0', '\n', '\r');
    }

    public static int Uniform(uint program, string name)
    {
        var bytes = Encoding.ASCII.GetBytes(name + "\0");
        fixed (byte* p = bytes) return GetUniformLocation(program, p);
    }
}
