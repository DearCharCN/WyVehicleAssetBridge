using System;
using System.Drawing;
using System.Windows.Forms;

namespace WyVehicleAssetBridge.Setup
{
    internal enum PublishCloseChoice { Cancel, Automatic, Manual }

    internal sealed class PublishCloseDialog : Form
    {
        private static readonly Color Ink = Color.FromArgb(30, 44, 58);
        private static readonly Color Muted = Color.FromArgb(79, 96, 108);
        private static readonly Color Good = Color.FromArgb(29, 111, 82);
        private static readonly Color Attention = Color.FromArgb(150, 74, 22);
        private readonly Label status;
        private readonly Button proceed;
        private readonly Button manual;
        private readonly CheckBox reopen;

        private PublishCloseDialog(bool forMax, bool reopenInitially)
        {
            Name = forMax ? "MaxPublishCloseDialog" : "UEPublishCloseDialog";
            Text = "发布插件";
            Font = new Font("Microsoft YaHei UI", 9F);
            ForeColor = Ink;
            BackColor = Color.White;
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            ShowInTaskbar = false;
            MinimizeBox = false;
            MaximizeBox = false;

            var body = new TableLayoutPanel {
                Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1, RowCount = 4, Padding = new Padding(24), Margin = Padding.Empty,
                MinimumSize = new Size(500, 0)
            };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            for (int row = 0; row < body.RowCount; ++row)
                body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(body);

            var title = new Label {
                Name = "PublishCloseTitle", AutoSize = true, Dock = DockStyle.Fill,
                Text = forMax ? "发布Max插件需要先关闭Max进程" : "发布UE插件需要先关闭UE进程",
                Font = new Font(Font.FontFamily, 12F, FontStyle.Bold),
                Margin = new Padding(0, 0, 0, 12)
            };
            body.Controls.Add(title, 0, 0);
            var explanation = new Label {
                AutoSize = true, Dock = DockStyle.Fill, ForeColor = Muted,
                MaximumSize = new Size(452, 0), Margin = new Padding(0, 0, 0, 16),
                Text = forMax
                    ? "请先在 3ds Max 中保存当前文件并关闭 3ds Max，然后点击“我已关闭”。"
                    : "保存未保存的资源后关闭 UE，关闭完成后继续发布插件。也可手动保存并关闭 UE，再点击“我已手动关闭”；本次不会自动重开。"
            };
            body.Controls.Add(explanation, 0, 1);
            status = new Label {
                Name = "PublishCloseStatus", AutoSize = true, Dock = DockStyle.Fill,
                MaximumSize = new Size(452, 0), ForeColor = Attention,
                Margin = new Padding(0, 0, 0, 16), Visible = false
            };
            body.Controls.Add(status, 0, 2);

            var actions = new TableLayoutPanel {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Fill,
                ColumnCount = 3, RowCount = forMax ? 1 : 2, Margin = Padding.Empty
            };
            actions.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            actions.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            actions.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            proceed = CreateButton(forMax ? "我已关闭" : "保存文件并关闭", true);
            proceed.Name = "ConfirmPublishClose";
            actions.Controls.Add(proceed, 0, 0);
            if (!forMax)
            {
                manual = CreateButton("我已手动关闭", false);
                manual.Name = "ConfirmManualUEClose";
                manual.Anchor = AnchorStyles.None;
                manual.Margin = new Padding(10, 0, 10, 0);
                actions.Controls.Add(manual, 1, 0);
            }
            var cancel = CreateButton(forMax ? "不发布" : "否", false);
            cancel.Name = "CancelPublishClose";
            cancel.DialogResult = DialogResult.Cancel;
            actions.Controls.Add(cancel, 2, 0);
            if (!forMax)
            {
                actions.RowStyles.Add(new RowStyle(SizeType.AutoSize));
                reopen = new CheckBox {
                    Name = "ReopenUEAfterPublish", Text = "发布完成后打开UE", AutoSize = true,
                    Checked = reopenInitially, Margin = new Padding(0, 10, 0, 0)
                };
                actions.Controls.Add(reopen, 0, 1);
                actions.SetColumnSpan(reopen, 3);
            }
            body.Controls.Add(actions, 0, 3);
            AcceptButton = proceed;
            CancelButton = cancel;
        }

        private static Button CreateButton(string caption, bool primary)
        {
            var button = new Button {
                Text = caption, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                MinimumSize = new Size(primary ? 140 : 88, 36), Padding = new Padding(12, 4, 12, 4),
                Margin = Padding.Empty, FlatStyle = FlatStyle.Flat, Cursor = Cursors.Hand,
                BackColor = primary ? Good : Color.White, ForeColor = primary ? Color.White : Ink
            };
            button.FlatAppearance.BorderColor = primary ? Good : Color.FromArgb(192, 202, 207);
            return button;
        }

        private void ShowStatus(string message)
        {
            status.Text = message;
            status.Visible = true;
        }

        internal static bool ConfirmMax(IWin32Window owner, Func<bool> isStillRunning)
        {
            if (isStillRunning == null) throw new ArgumentNullException("isStillRunning");
            using (var dialog = new PublishCloseDialog(true, false))
            {
                dialog.proceed.Click += delegate {
                    try
                    {
                        if (isStillRunning())
                        {
                            dialog.ShowStatus("仍检测到 3ds Max 进程，请关闭后再点击“我已关闭”。");
                            return;
                        }
                        dialog.DialogResult = DialogResult.OK;
                    }
                    catch (Exception ex)
                    {
                        dialog.ShowStatus("检查 3ds Max 进程失败：" + ex.Message);
                    }
                };
                return dialog.ShowDialog(owner) == DialogResult.OK;
            }
        }

        internal static PublishCloseChoice ConfirmUE(IWin32Window owner, bool reopenInitially,
            Action<bool> savePreference, Func<bool> isStillRunning, out bool reopenAfterPublish)
        {
            if (savePreference == null) throw new ArgumentNullException("savePreference");
            if (isStillRunning == null) throw new ArgumentNullException("isStillRunning");
            using (var dialog = new PublishCloseDialog(false, reopenInitially))
            {
                PublishCloseChoice choice = PublishCloseChoice.Cancel;
                dialog.reopen.CheckedChanged += delegate {
                    try
                    {
                        savePreference(dialog.reopen.Checked);
                        dialog.status.Visible = false;
                        dialog.proceed.Enabled = true;
                    }
                    catch (Exception ex)
                    {
                        dialog.ShowStatus("保存重开设置失败，请重新勾选后重试：" + ex.Message);
                        dialog.proceed.Enabled = false;
                    }
                };
                dialog.proceed.Click += delegate {
                    choice = PublishCloseChoice.Automatic;
                    dialog.DialogResult = DialogResult.OK;
                };
                dialog.manual.Click += delegate {
                    try
                    {
                        if (isStillRunning())
                        {
                            dialog.ShowStatus("仍检测到当前工程的 UE 进程，请关闭后再点击“我已手动关闭”。");
                            return;
                        }
                        choice = PublishCloseChoice.Manual;
                        dialog.DialogResult = DialogResult.OK;
                    }
                    catch (Exception ex)
                    {
                        dialog.ShowStatus("检查 UE 进程失败：" + ex.Message);
                    }
                };
                bool confirmed = dialog.ShowDialog(owner) == DialogResult.OK;
                reopenAfterPublish = confirmed && choice == PublishCloseChoice.Automatic && dialog.reopen.Checked;
                return confirmed ? choice : PublishCloseChoice.Cancel;
            }
        }
    }
}
