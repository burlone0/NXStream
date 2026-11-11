using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace NXStream.UI;

internal static class WindowsTheme
{
    private static readonly Color Back = Color.FromArgb(32, 32, 32);
    private static readonly Color Surface = Color.FromArgb(43, 43, 43);
    private static readonly Color Button = Color.FromArgb(55, 55, 55);
    private static readonly Color Border = Color.FromArgb(85, 85, 85);
    private static readonly Color Text = Color.FromArgb(242, 242, 242);
    private static readonly Color Link = Color.FromArgb(120, 175, 255);

    public static bool Dark { get; } = ReadDark();

    private static bool ReadDark()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("AppsUseLightTheme") is int light && light == 0;
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            return false;
        }
    }

    public static void Apply(Form form)
    {
        if (form.IsHandleCreated) TitleBar(form.Handle);
        form.HandleCreated += (_, _) => TitleBar(form.Handle);
        if (!Dark) return;
        form.BackColor = Back;
        form.ForeColor = Text;
        Style(form.Controls);
    }

    public static void Apply(ContextMenuStrip menu)
    {
        if (!Dark) return;
        menu.Renderer = new ToolStripProfessionalRenderer(new DarkColors());
        menu.BackColor = Surface;
        menu.ForeColor = Text;
    }

    private static void TitleBar(IntPtr hwnd)
    {
        var dark = Dark ? 1 : 0;
        if (DwmSetWindowAttribute(hwnd, 20, ref dark, sizeof(int)) != 0) DwmSetWindowAttribute(hwnd, 19, ref dark, sizeof(int));
    }

    private static void Style(Control.ControlCollection controls)
    {
        foreach (Control c in controls)
        {
            switch (c)
            {
                case TextBox box:
                    box.BackColor = Surface;
                    box.ForeColor = Text;
                    box.BorderStyle = BorderStyle.FixedSingle;
                    break;
                case System.Windows.Forms.Button button:
                    button.FlatStyle = FlatStyle.Flat;
                    button.BackColor = Button;
                    button.ForeColor = Text;
                    button.FlatAppearance.BorderColor = Border;
                    break;
                case ListView list:
                    StyleList(list);
                    break;
                case LinkLabel link:
                    link.LinkColor = Link;
                    link.ActiveLinkColor = Text;
                    break;
                case TrackBar track:
                    track.BackColor = Back;
                    break;
                default:
                    c.ForeColor = Text;
                    break;
            }
            Style(c.Controls);
        }
    }

    private static void StyleList(ListView list)
    {
        list.BackColor = Surface;
        list.ForeColor = Text;
        list.BorderStyle = BorderStyle.FixedSingle;
        list.OwnerDraw = true;
        list.DrawItem += (_, e) => e.DrawDefault = true;
        list.DrawSubItem += (_, e) => e.DrawDefault = true;
        list.DrawColumnHeader += (_, e) =>
        {
            using var back = new SolidBrush(Back);
            e.Graphics.FillRectangle(back, e.Bounds);
            using var line = new Pen(Border);
            e.Graphics.DrawLine(line, e.Bounds.Left, e.Bounds.Bottom - 1, e.Bounds.Right, e.Bounds.Bottom - 1);
            e.Graphics.DrawLine(line, e.Bounds.Right - 1, e.Bounds.Top + 4, e.Bounds.Right - 1, e.Bounds.Bottom - 4);
            TextRenderer.DrawText(e.Graphics, e.Header?.Text, list.Font, Rectangle.Inflate(e.Bounds, -6, 0), Text,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        };
        if (list.IsHandleCreated) SetWindowTheme(list.Handle, "DarkMode_Explorer", null);
        list.HandleCreated += (_, _) => SetWindowTheme(list.Handle, "DarkMode_Explorer", null);
    }

    private sealed class DarkColors : ProfessionalColorTable
    {
        public override Color ToolStripDropDownBackground => Surface;
        public override Color MenuBorder => Border;
        public override Color MenuItemBorder => Border;
        public override Color MenuItemSelected => Button;
        public override Color ImageMarginGradientBegin => Surface;
        public override Color ImageMarginGradientMiddle => Surface;
        public override Color ImageMarginGradientEnd => Surface;
        public override Color SeparatorDark => Border;
        public override Color SeparatorLight => Border;
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    private static extern int SetWindowTheme(IntPtr hwnd, string? appName, string? idList);
}
