namespace NXStream.Render;

public enum Scaler { Sharp, Smooth, Fsr, SuperResolution }

public sealed record RenderSettings(Scaler Scaler, int Sharpness);

public sealed record RenderInfo(string Scaler, string Size, string Gpu, double DrawMs, string? Problem);

public sealed record OverlayImage(byte[] Pixels, int Width, int Height, bool Centered, float Opacity, int X = -1, int Y = -1);
