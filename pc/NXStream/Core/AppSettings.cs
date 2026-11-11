using System.Text.Json;
using NXStream.Render;

namespace NXStream.Core;

public enum ConnectionMode { Network, Usb }

public sealed class AppSettings
{
    public string? LastTarget { get; set; }
    public ConnectionMode Connection { get; set; } = ConnectionMode.Network;
    public Scaler Scaler { get; set; } = Scaler.Sharp;
    public int Sharpness { get; set; }
    public int VolumePercent { get; set; } = 100;
    public bool Muted { get; set; }
    public bool ShowStats { get; set; }
    public bool StartFullscreen { get; set; }
    public bool AutoReconnect { get; set; } = true;
    public bool KeepAwake { get; set; } = true;
    public bool DiscordPresence { get; set; }

    public RenderSettings Render => new(Scaler, Math.Clamp(Sharpness, 0, 100));

    public float EffectiveVolume => Muted ? 0f : Math.Clamp(VolumePercent, 0, 100) / 100f;

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "NXStream", "settings.json");

    private static void MigrateLegacy()
    {
        try
        {
            var legacy = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "NStream", "settings.json");
            if (File.Exists(FilePath) || !File.Exists(legacy)) return;
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.Copy(legacy, FilePath);
            Log.Write("app", "settings migrated from the old NStream folder");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Log.Write("app", $"settings migration failed: {ex.GetType().Name}");
        }
    }

    public static AppSettings Load()
    {
        MigrateLegacy();
        try
        {
            if (File.Exists(FilePath))
            {
                var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath), JsonOptions) ?? new();
                Log.Write("app", $"settings loaded: {settings}");
                return settings;
            }
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            Log.Write("app", $"settings unreadable ({ex.GetType().Name}), using defaults");
        }
        return new();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, JsonOptions));
            Log.Write("app", $"settings saved: {this}");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Log.Write("app", $"settings not saved: {ex.GetType().Name}");
        }
    }

    public override string ToString() =>
        $"target={(LastTarget is null ? "no" : "yes")} connection={Connection} scaler={Scaler} sharpness={Sharpness} " +
        $"volume={VolumePercent}{(Muted ? " muted" : "")} stats={ShowStats} " +
        $"fullscreen={StartFullscreen} reconnect={AutoReconnect} keepAwake={KeepAwake} discord={DiscordPresence}";
}
