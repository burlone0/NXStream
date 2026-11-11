using System;
using NXStream.Net;
using NXStream.Render;

namespace NXStream.Core;

public static class MainText
{
    public const string Searching = "Searching for the Switch…";
    public const string NoConsole = "No console found. Turn on the Switch or enter its IP.";
    public const string DoubleClickHint = "Double-click a console, or click Connect.";
    public const string InvalidAddress = "Invalid IP address. The NXStream app on the Switch shows it.";
    public const string FoundLabel = "Consoles found on the network:";
    public const string FoundLabelUsb = "Consoles found on USB:";
    public const string AddressLabel = "IP address:";
    public const string AddressPlaceholder = "e.g. 192.168.1.14";
    public const string ConnectButton = "Connect";
    public const string OptionsButton = "Options…";
    public const string NameColumn = "Name";
    public const string AddressColumn = "Address";
    public const string StateColumn = "Status";

    public const string OptionsTitle = "Options";
    public const string ImageGroup = "Image quality";
    public const string AudioGroup = "NXStream audio";
    public const string GeneralGroup = "General";
    public const string SharpnessLabel = "Extra sharpening";
    public const string SharpnessHint = "Applies in full screen or when the window is larger than the video (720p).";
    public const string MuteLabel = "Mute";
    public const string LogFolderLabel = "Log folder";
    public const string OkButton = "OK";

    public static string FfmpegMissing => OperatingSystem.IsWindows()
        ? FfmpegMissingFor("winget install Gyan.FFmpeg")
        : FfmpegMissingFor("your package manager, e.g. sudo apt install ffmpeg");

    public static string FfmpegMissingFor(string installHint) => $"FFmpeg is missing: install it with {installHint} and reopen NXStream.";

    public static string Connecting(string label) => $"Connecting to {label}…";

    public static string[] DeviceCells(SwitchDevice device) =>
        [device.Name, UsbTarget.Is(device.Address) ? UsbText.Usb : device.Address.ToString(), device.Busy ? "In use" : "Available"];

    public static (Scaler Scaler, string Text)[] Scalers(bool superResolution)
    {
        (Scaler Scaler, string Text)[] common =
        [
            (Scaler.Sharp, "Sharp (Catmull-Rom), recommended"),
            (Scaler.Smooth, "Smooth (bilinear)"),
            (Scaler.Fsr, "AMD FidelityFX FSR 1, any graphics card"),
        ];
        return superResolution
            ? [.. common, (Scaler.SuperResolution, "Super Resolution, NVIDIA RTX or Intel")]
            : common;
    }

    public static Scaler ShownScaler(AppSettings settings, bool superResolution) =>
        !superResolution && settings.Scaler == Scaler.SuperResolution ? Scaler.Sharp : settings.Scaler;

    public static string SharpnessText(int value) => value == 0 ? "No" : $"{value}%";

    public static string PercentText(int value) => $"{value}%";

    public static (string Text, bool Value, Action<bool> Apply)[] GeneralChecks(AppSettings settings) =>
    [
        ("Open the video in full screen", settings.StartFullscreen, v => settings.StartFullscreen = v),
        ("Reconnect automatically if the Switch restarts", settings.AutoReconnect, v => settings.AutoReconnect = v),
        ("Keep the PC awake while watching", settings.KeepAwake, v => settings.KeepAwake = v),
        ("Show what I'm playing on Discord", settings.DiscordPresence, v => settings.DiscordPresence = v),
    ];
}
