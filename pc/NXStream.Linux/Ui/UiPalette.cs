using System.Diagnostics;
using System.Drawing;
using System.Text.RegularExpressions;
using NXStream.Core;

namespace NXStream.Linux.Ui;

internal sealed class UiPalette
{
    public bool Dark { get; init; }
    public Color Back { get; init; }
    public Color Surface { get; init; }
    public Color Text { get; init; }
    public Color Muted { get; init; }
    public Color Disabled { get; init; }
    public Color Border { get; init; }
    public Color ControlBorder { get; init; }
    public Color ButtonFill { get; init; }
    public Color ButtonHover { get; init; }
    public Color ButtonPressed { get; init; }
    public Color Accent { get; init; }
    public Color AccentHover { get; init; }
    public Color OnAccent { get; init; }
    public Color Link { get; init; }
    public Color Hover { get; init; }
    public Color Selection { get; init; }
    public Color SelectionText { get; init; }
    public Color Rail { get; init; }
    public Color HeaderBack { get; init; }

    public static readonly UiPalette Light = new()
    {
        Dark = false,
        Back = Color.FromArgb(240, 240, 240),
        Surface = Color.FromArgb(255, 255, 255),
        Text = Color.FromArgb(0, 0, 0),
        Muted = Color.FromArgb(109, 109, 109),
        Disabled = Color.FromArgb(160, 160, 160),
        Border = Color.FromArgb(218, 218, 218),
        ControlBorder = Color.FromArgb(138, 138, 138),
        ButtonFill = Color.FromArgb(253, 253, 253),
        ButtonHover = Color.FromArgb(243, 243, 243),
        ButtonPressed = Color.FromArgb(232, 232, 232),
        Accent = Color.FromArgb(0, 103, 192),
        AccentHover = Color.FromArgb(25, 117, 198),
        OnAccent = Color.FromArgb(255, 255, 255),
        Link = Color.FromArgb(0, 102, 204),
        Hover = Color.FromArgb(229, 243, 255),
        Selection = Color.FromArgb(204, 232, 255),
        SelectionText = Color.FromArgb(0, 0, 0),
        Rail = Color.FromArgb(138, 138, 138),
        HeaderBack = Color.FromArgb(255, 255, 255),
    };

    public static readonly UiPalette DarkPalette = new()
    {
        Dark = true,
        Back = Color.FromArgb(32, 32, 32),
        Surface = Color.FromArgb(43, 43, 43),
        Text = Color.FromArgb(242, 242, 242),
        Muted = Color.FromArgb(170, 170, 170),
        Disabled = Color.FromArgb(110, 110, 110),
        Border = Color.FromArgb(85, 85, 85),
        ControlBorder = Color.FromArgb(160, 160, 160),
        ButtonFill = Color.FromArgb(55, 55, 55),
        ButtonHover = Color.FromArgb(66, 66, 66),
        ButtonPressed = Color.FromArgb(48, 48, 48),
        Accent = Color.FromArgb(96, 205, 255),
        AccentHover = Color.FromArgb(120, 214, 255),
        OnAccent = Color.FromArgb(0, 0, 0),
        Link = Color.FromArgb(120, 175, 255),
        Hover = Color.FromArgb(58, 58, 58),
        Selection = Color.FromArgb(0, 84, 140),
        SelectionText = Color.FromArgb(255, 255, 255),
        Rail = Color.FromArgb(160, 160, 160),
        HeaderBack = Color.FromArgb(32, 32, 32),
    };

    public static UiPalette For(bool dark) => dark ? DarkPalette : Light;
}

internal static class ThemeDetect
{
    public static bool IsDark()
    {
        var portal = Run("gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
            "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.portal.Settings.Read",
            "org.freedesktop.appearance", "color-scheme");
        var match = portal is null ? null : Regex.Match(portal, @"uint32 (\d)");
        if (match is { Success: true } && match.Groups[1].Value != "0")
        {
            var dark = match.Groups[1].Value == "1";
            Log.Write("ui", $"theme from portal color-scheme {match.Groups[1].Value}: {(dark ? "dark" : "light")}");
            return dark;
        }

        var gsettings = Run("gsettings", "get", "org.gnome.desktop.interface", "color-scheme");
        if (gsettings is not null && gsettings.Contains("prefer-dark"))
        {
            Log.Write("ui", "theme from gsettings color-scheme: dark");
            return true;
        }
        if (gsettings is not null && gsettings.Contains("prefer-light"))
        {
            Log.Write("ui", "theme from gsettings color-scheme: light");
            return false;
        }

        var gtk = Environment.GetEnvironmentVariable("GTK_THEME");
        if (!string.IsNullOrEmpty(gtk))
        {
            var dark = gtk.Contains(":dark", StringComparison.OrdinalIgnoreCase) ||
                       gtk.EndsWith("-dark", StringComparison.OrdinalIgnoreCase);
            Log.Write("ui", $"theme from GTK_THEME '{gtk}': {(dark ? "dark" : "light")}");
            return dark;
        }

        Log.Write("ui", "theme not detected, using light");
        return false;
    }

    private static string? Run(string file, params string[] args)
    {
        try
        {
            var info = new ProcessStartInfo(file, args)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            info.Environment.Remove("LD_LIBRARY_PATH");
            using var process = Process.Start(info);
            if (process is null) return null;
            var output = process.StandardOutput.ReadToEndAsync();
            if (!process.WaitForExit(1000))
            {
                process.Kill();
                Log.Write("ui", $"{file} timed out");
                return null;
            }
            return process.ExitCode == 0 ? output.Result : null;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            return null;
        }
    }
}
