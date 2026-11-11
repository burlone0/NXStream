using System.Drawing;
using NXStream.Core;
using NXStream.Linux.Ui;

namespace NXStream.Linux;

internal sealed class UpdateWindow
{
    private readonly UiWindow _window;
    private bool _closed;

    public event Action? Accepted;

    public UpdateWindow(UpdateOffer offer, IntPtr parent)
    {
        _window = new UiWindow(UpdateText.Title, 380, 120, App.Palette);
        _window.Closed += Close;

        _window.Add(new Label
        {
            Text = UpdateText.Body(offer.Version),
            Location = new Point(14, 18),
            Size = new Size(352, 48),
            Wrap = true,
        });
        var update = new Button { Text = UpdateText.Update, Location = new Point(194, 80), Size = new Size(84, 27) };
        var cancel = new Button { Text = UpdateText.Cancel, Location = new Point(286, 80), Size = new Size(84, 27) };
        update.Click += () =>
        {
            Close();
            Accepted?.Invoke();
        };
        cancel.Click += Close;
        _window.Add(update);
        _window.Add(cancel);
        _window.AcceptButton = update;
        _window.CancelButton = cancel;

        _window.SetModalFor(parent);
        App.Modal = _window;
    }

    public void Show() => _window.Show();

    public void Close()
    {
        if (_closed) return;
        _closed = true;
        if (App.Modal == _window) App.Modal = null;
        _window.Hide();
        App.Post(_window.Dispose);
    }
}
