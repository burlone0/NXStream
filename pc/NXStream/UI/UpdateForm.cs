using NXStream.Core;

namespace NXStream.UI;

public sealed class UpdateForm : Form
{
    public UpdateForm(UpdateOffer offer)
    {
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Text = UpdateText.Title;
        Font = SystemFonts.MessageBoxFont ?? Font;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(380, 120);

        Controls.Add(new Label
        {
            Text = UpdateText.Body(offer.Version),
            Location = new Point(14, 18),
            Size = new Size(352, 48),
        });
        var update = new Button { Text = UpdateText.Update, DialogResult = DialogResult.OK, Location = new Point(194, 80), Size = new Size(84, 27) };
        var cancel = new Button { Text = UpdateText.Cancel, DialogResult = DialogResult.Cancel, Location = new Point(286, 80), Size = new Size(84, 27) };
        Controls.Add(update);
        Controls.Add(cancel);
        AcceptButton = update;
        CancelButton = cancel;
        WindowsTheme.Apply(this);
    }
}
