using System;
using System.Collections.Generic;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

namespace Nebula.Launcher
{
    // Controller model, device list and report decoding, with the same vectors as the runtime's
    // controls_tests.cpp and pad_controller_tests.cpp. Run by LauncherTests.Main.
    internal static class ControllerTests
    {
        static void Check(bool ok, string what)
        {
            if (!ok) throw new Exception("FAIL: " + what);
            Console.WriteLine("PASS: " + what);
        }

        static string Bound(int scheme, int target)
        {
            return ControlsSettings.FormatBindings(scheme, ControlsSettings.DefaultTable(scheme)[target]);
        }

        static void Put16(byte[] r, int at, int value) { r[at] = (byte)(value & 0xFF); r[at + 1] = (byte)((value >> 8) & 0xFF); }
        static void Put32(byte[] r, int at, uint value) { for (int i = 0; i < 4; i++) r[at + i] = (byte)(value >> (8 * i)); }
        static int Bit(int index) { return 1 << index; }

        static void SealInput(byte[] r)
        {
            Put32(r, 74, PadReports.Crc32(0xA1, r, 74));
        }

        static byte[] DualShock4Usb()
        {
            var r = new byte[64];
            r[0] = 0x01;
            for (int i = 1; i <= 4; i++) r[i] = 128;
            r[5] = 0x08;
            return r;
        }

        static byte[] DualShock4Bluetooth()
        {
            var r = new byte[78];
            r[0] = 0x11; r[1] = 0xC4;
            for (int i = 3; i <= 6; i++) r[i] = 128;
            r[7] = 0x08;
            SealInput(r);
            return r;
        }

        static byte[] DualSenseUsb()
        {
            var r = new byte[64];
            r[0] = 0x01;
            for (int i = 1; i <= 4; i++) r[i] = 128;
            r[8] = 0x08;
            return r;
        }

        static PadSample Decoded(int model, bool bluetooth, byte[] report, int size)
        {
            var sample = new PadSample();
            return PadReports.DecodePlayStation(model, bluetooth, report, size, sample) ? sample : null;
        }

        static ControllerInfo Controller(int scheme, int slot, ControllerLink link, string model)
        {
            return new ControllerInfo { Scheme = scheme, Slot = slot, Link = link, Model = model };
        }

        static IEnumerable<Control> All(Control control)
        {
            foreach (Control child in control.Controls)
            {
                yield return child;
                foreach (Control grandchild in All(child)) yield return grandchild;
            }
        }

        // The Controls tab with controllers plugged and unplugged.
        public static void Form(LaunchSession session, string outDir)
        {
            var attached = new List<ControllerInfo>
            {
                Controller(ControlsSettings.Xbox, 0, ControllerLink.Unknown, "Xbox controller"),
                Controller(ControlsSettings.PlayStation, 0, ControllerLink.Bluetooth, "DualSense Edge"),
            };
            ControllerProbe.Source = delegate { return new List<ControllerInfo>(attached); };
            try
            {
                using (var form = new SettingsForm(session))
                {
                    foreach (Control child in All(form)) { IntPtr created = child.Handle; }
                    var tabs = form.Controls.OfType<TabControl>().Single();
                    var device = All(form).OfType<ComboBox>().Single(c => c.Items.Count > 0 && c.Items[0].ToString() == "Keyboard and mouse");
                    var bindings = All(form).OfType<ListView>().Single();
                    Func<string> aBinding = () => bindings.Items[0].SubItems[1].Text;
                    Check(device.Items.Cast<object>().Select(i => i.ToString()).SequenceEqual(new[] { "Keyboard and mouse", "Xbox controller", "DualSense Edge (Bluetooth)" }),
                        "the device list is the keyboard and the attached controllers");
                    Check(device.SelectedIndex == 0 && aBinding() == "Space, Enter, MouseLeft", "the keyboard is edited first");
                    device.SelectedIndex = 2;
                    Check(aBinding() == "Cross", "choosing a controller shows its family's bindings");
                    device.SelectedIndex = 1;
                    Check(aBinding() == "A", "the Xbox bindings use its names");
                    device.SelectedIndex = 2;

                    var refresh = typeof(SettingsForm).GetMethod("RefreshDevices", BindingFlags.NonPublic | BindingFlags.Instance);
                    attached.Add(Controller(ControlsSettings.Steam, 0, ControllerLink.Usb, "Steam Controller"));
                    refresh.Invoke(form, null);
                    Check(device.Items.Count == 4 && device.SelectedIndex == 2 && aBinding() == "Cross", "a plugged controller joins the list, the edit stays");
                    attached.RemoveAt(1);
                    refresh.Invoke(form, null);
                    Check(device.Items.Count == 3 && !device.Items.Cast<object>().Any(i => i.ToString().StartsWith("DualSense")), "an unplugged controller leaves the list");
                    Check(device.SelectedIndex == 0 && aBinding() == "Space, Enter, MouseLeft", "the edited controller left: the keyboard is edited");
                    attached.Add(Controller(ControlsSettings.PlayStation, 1, ControllerLink.Usb, "DualShock 4"));
                    refresh.Invoke(form, null);
                    Check(device.Items.Count == 4 && device.Items[2].ToString() == "DualShock 4 (USB)" && device.SelectedIndex == 0, "another PlayStation controller is listed by its name");
                    device.SelectedIndex = 2;
                    Check(aBinding() == "Cross", "it edits the PlayStation family's bindings");
                    tabs.SelectedIndex = 2;
                    using (var bitmap = new Bitmap(form.Width, form.Height))
                    {
                        form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                        bitmap.Save(Path.Combine(outDir, "controllers-controls.png"), ImageFormat.Png);
                    }
                }
            }
            finally { ControllerProbe.Source = null; }
        }

        public static void Run(string outDir)
        {
            Model(outDir);
            Roster();
            Reports();
            Generic();
        }

        // Generic HID controllers: the same vectors as pad_controller_tests.cpp test_generic.
        static void Generic()
        {
            Check(GenericHid.IsGameController(0x01, 0x05) && GenericHid.IsGameController(0x01, 0x04) && !GenericHid.IsGameController(0x01, 0x02),
                  "game pad and joystick collections are generic controllers, mice are not");
            Check(GenericHid.ClaimedElsewhere(0x045e, "hid#vid_045e&pid_02ff#7") && GenericHid.ClaimedElsewhere(0x0e6f, "hid#vid_0e6f&pid_02a1&IG_00#8") &&
                  GenericHid.ClaimedElsewhere(0x054c, "x") && !GenericHid.ClaimedElsewhere(0x0079, "hid#vid_0079&pid_0006#7"),
                  "Microsoft, XInput interfaces and the supported vendors are read elsewhere");
            Check(GenericHid.ButtonBit(1) == ControlsSettings.South && GenericHid.ButtonBit(2) == ControlsSettings.East &&
                  GenericHid.ButtonBit(9) == ControlsSettings.Back && GenericHid.ButtonBit(10) == ControlsSettings.Start &&
                  GenericHid.ButtonBit(0) == -1 && GenericHid.ButtonBit(20) == -1, "button numbers to unified bits");
            Check(GenericHid.AxisOf(0x01, 0x35) == GenericHid.Rz && GenericHid.AxisOf(0x02, 0xC5) == GenericHid.Brake &&
                  GenericHid.AxisOf(0x02, 0x30) == -1, "axis usages");

            var layout = new GenericLayout();
            foreach (int axis in new[] { GenericHid.X, GenericHid.Y, GenericHid.Z, GenericHid.Rz })
                layout.Axes[axis] = new GenericRange { Present = true, Min = 0, Max = 255, Bits = 8 };
            layout.Hat = new GenericRange { Present = true, Min = 0, Max = 7, Bits = 4 };
            var raw = new GenericRaw();
            raw.Axes[GenericHid.X] = 128; raw.Axes[GenericHid.Y] = 128; raw.Axes[GenericHid.Z] = 128; raw.Axes[GenericHid.Rz] = 128;
            raw.Hat = 8;
            var s = new PadSample();
            GenericHid.Map(layout, raw, s);
            Check(s.Connected && s.Buttons == 0 && Math.Abs(s.Axes[0]) < 0.01f && Math.Abs(s.Axes[3]) < 0.01f && s.Axes[4] == 0f, "neutral generic pad");
            raw.Axes[GenericHid.X] = 255; raw.Axes[GenericHid.Y] = 0; raw.Axes[GenericHid.Z] = 0; raw.Axes[GenericHid.Rz] = 255;
            raw.Hat = 1;
            raw.Buttons = (1u << 0) | (1u << 6) | (1u << 11);
            GenericHid.Map(layout, raw, s);
            Check(Math.Abs(s.Axes[0] - 1f) < 1e-3f && Math.Abs(s.Axes[1] - 1f) < 1e-3f && Math.Abs(s.Axes[2] + 1f) < 1e-3f && Math.Abs(s.Axes[3] + 1f) < 1e-3f,
                  "X/Y left stick up positive, Z/Rz right stick");
            Check(s.Buttons == (Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.DpadRight) | Bit(ControlsSettings.South) |
                                Bit(ControlsSettings.GripLeft) | Bit(ControlsSettings.RightStick)), "hat diagonal and buttons 1, 7, 12");

            var xboxLike = new GenericLayout();
            foreach (int axis in new[] { GenericHid.X, GenericHid.Y, GenericHid.Rx, GenericHid.Ry })
                xboxLike.Axes[axis] = new GenericRange { Present = true, Min = -32768, Max = 32767, Bits = 16 };
            xboxLike.Axes[GenericHid.Z] = new GenericRange { Present = true, Min = 0, Max = 1023, Bits = 10 };
            var signedRaw = new GenericRaw();
            signedRaw.Axes[GenericHid.Rx] = 0x8000u; signedRaw.Axes[GenericHid.Ry] = 0x7FFFu; signedRaw.Axes[GenericHid.Z] = 1023u;
            GenericHid.Map(xboxLike, signedRaw, s);
            Check(Math.Abs(s.Axes[2] + 1f) < 1e-3f && Math.Abs(s.Axes[3] + 1f) < 1e-3f && Math.Abs(s.Axes[4] - 1f) < 1e-3f && s.Axes[5] == 0f,
                  "signed Rx/Ry right stick, Z left trigger");
            Check(GenericHid.LogicalValue(new GenericRange { Present = true, Min = -128, Max = 127, Bits = 8 }, 0xFFu) == -1 &&
                  GenericHid.LogicalValue(new GenericRange { Present = true, Min = 0, Max = 255, Bits = 8 }, 0xFFu) == 255, "sign extension");
            var fourWay = new GenericLayout();
            fourWay.Hat = new GenericRange { Present = true, Min = 1, Max = 4, Bits = 4 };
            var hatRaw = new GenericRaw { Hat = 3 };
            GenericHid.Map(fourWay, hatRaw, s);
            Check(s.Buttons == Bit(ControlsSettings.DpadDown), "a four-position hat from 1: 3 is down");

            Check(ControlsSettings.SchemeIds[ControlsSettings.Generic] == "generic" && !ControlsSettings.HasMotion(ControlsSettings.Generic), "generic scheme");
            Check(Bound(ControlsSettings.Generic, 0) == "Button1" && Bound(ControlsSettings.Generic, ControlsSettings.JumpBack) == "Button2" &&
                  Bound(ControlsSettings.Generic, 1) == "Button5" && Bound(ControlsSettings.Generic, 16) == "LT, Button7" &&
                  Bound(ControlsSettings.Generic, 17) == "Button3, Button4, RT, Button8" && Bound(ControlsSettings.Generic, 7) == "DpadUp" &&
                  Bound(ControlsSettings.Generic, 23) == "Button12", "generic defaults");
            var tuning = ControlsSettings.DefaultTuning(ControlsSettings.Generic);
            Check(tuning.StickDeadzone == 0.15f && !tuning.GyroPointer && tuning.PointerHideSeconds == 4.0f, "generic tuning");

            var list = DeviceList.Build(new[] { new ControllerInfo { Scheme = ControlsSettings.Generic, Slot = 0, Model = "USB Gamepad", Link = ControllerLink.Usb } }, null);
            Check(list.Count == 2 && list[1].Label == "USB Gamepad (USB)", "a generic controller is listed by its product name");
        }

        static void Model(string outDir)
        {
            Check(ControlsSettings.SchemeCount == 7 && ControlsSettings.SchemeIds[4] == "playstation" && ControlsSettings.SchemeIds[5] == "steam" &&
                  ControlsSettings.SchemeIds[6] == "generic", "seven schemes");
            Check(ControlsSettings.HasMotion(ControlsSettings.PlayStation) && ControlsSettings.HasMotion(ControlsSettings.Steam) && !ControlsSettings.HasMotion(ControlsSettings.Xbox), "PlayStation and Steam have motion");
            Check(Bound(ControlsSettings.PlayStation, 0) == "Cross" && Bound(ControlsSettings.PlayStation, ControlsSettings.JumpBack) == "Circle" &&
                  Bound(ControlsSettings.PlayStation, 1) == "L1" && Bound(ControlsSettings.PlayStation, 16) == "L2" &&
                  Bound(ControlsSettings.PlayStation, 15) == "L3" && Bound(ControlsSettings.PlayStation, 17) == "Square, Triangle, R2" &&
                  Bound(ControlsSettings.PlayStation, 23) == "R1" && Bound(ControlsSettings.PlayStation, 4) == "Options" &&
                  Bound(ControlsSettings.PlayStation, 5) == "Share" && Bound(ControlsSettings.PlayStation, 6) == "PS" &&
                  Bound(ControlsSettings.PlayStation, 7) == "DpadUp, RY+", "PlayStation defaults");
            Check(Bound(ControlsSettings.Steam, 0) == "A" && Bound(ControlsSettings.Steam, ControlsSettings.JumpBack) == "B" &&
                  Bound(ControlsSettings.Steam, 1) == "LB" && Bound(ControlsSettings.Steam, 16) == "LT" &&
                  Bound(ControlsSettings.Steam, 17) == "X, Y, RT" && Bound(ControlsSettings.Steam, 23) == "RB" &&
                  Bound(ControlsSettings.Steam, 6) == "Steam", "Steam defaults");
            foreach (int scheme in new[] { ControlsSettings.PlayStation, ControlsSettings.Steam })
            {
                var table = ControlsSettings.DefaultTable(scheme);
                bool same = true;
                foreach (var older in new[] { ControlsSettings.LegacyTable(scheme), ControlsSettings.Version2Table(scheme), ControlsSettings.Version3Table(scheme) })
                    for (int t = 0; t < ControlsSettings.TargetCount; t++)
                        same &= ControlsSettings.FormatBindings(scheme, older[t]) == ControlsSettings.FormatBindings(scheme, table[t]);
                Check(same, "scheme " + scheme + " has no older defaults to migrate");
                var tuning = ControlsSettings.DefaultTuning(scheme);
                Check(tuning.GyroPointer && tuning.Motion == 1 && tuning.Vibration && tuning.PointerHideSeconds == 4.0f, "scheme " + scheme + " starts with gyro pointer, shake and vibration");
            }

            InputSource source;
            Check(ControlsSettings.TryParseSource(ControlsSettings.PlayStation, "create", out source) && source.Equals(new InputSource(SourceKind.Button, ControlsSettings.Back)), "Create is the Share button");
            Check(ControlsSettings.TryParseSource(ControlsSettings.PlayStation, "R2", out source) && source.Equals(new InputSource(SourceKind.AxisPositive, ControlsSettings.RightTrigger)) &&
                  !ControlsSettings.TryParseSource(ControlsSettings.PlayStation, "ZR", out source), "PlayStation trigger names");
            bool names = true;
            for (int scheme = 1; scheme < ControlsSettings.SchemeCount; scheme++)
                for (int bit = 0; bit < ControlsSettings.ButtonCount; bit++)
                {
                    var button = new InputSource(SourceKind.Button, bit);
                    if (!ControlsSettings.Allowed(scheme, 0, button)) continue;
                    InputSource parsed;
                    names &= ControlsSettings.TryParseSource(scheme, ControlsSettings.FormatSource(scheme, button), out parsed) && parsed.Equals(button);
                }
            Check(names, "every allowed button has a distinct name that parses back");
            Func<int, int, bool> allowed = (scheme, bit) => ControlsSettings.Allowed(scheme, 0, new InputSource(SourceKind.Button, bit));
            Check(allowed(ControlsSettings.PlayStation, ControlsSettings.Capture) && allowed(ControlsSettings.PlayStation, ControlsSettings.Chat) &&
                  allowed(ControlsSettings.PlayStation, ControlsSettings.GripLeft) && !allowed(ControlsSettings.PlayStation, ControlsSettings.LeftPad) &&
                  !allowed(ControlsSettings.PlayStation, ControlsSettings.GripLeft2), "PlayStation buttons");
            Check(allowed(ControlsSettings.Steam, ControlsSettings.LeftPad) && allowed(ControlsSettings.Steam, ControlsSettings.GripRight2) &&
                  allowed(ControlsSettings.Steam, ControlsSettings.GripLeft) && !allowed(ControlsSettings.Steam, ControlsSettings.Chat), "Steam buttons");
            Check(!allowed(ControlsSettings.Xbox, ControlsSettings.LeftPad) && !allowed(ControlsSettings.SwitchPro2, ControlsSettings.LeftPad) &&
                  allowed(ControlsSettings.SwitchPro2, ControlsSettings.Chat) && !allowed(ControlsSettings.SwitchPro, ControlsSettings.GripLeft) &&
                  !allowed(ControlsSettings.Xbox, ControlsSettings.Home) && allowed(ControlsSettings.SwitchPro, ControlsSettings.Home) &&
                  !allowed(ControlsSettings.Keyboard, ControlsSettings.South), "the other schemes keep their button sets");

            // The settings file: new schemes round trip, and a file from before them loads.
            var settings = new ControlsSettings();
            settings.Bindings[ControlsSettings.PlayStation][0] = new[] { new InputSource(SourceKind.Button, ControlsSettings.Capture) };
            settings.Bindings[ControlsSettings.Steam][16] = new[] { new InputSource(SourceKind.Button, ControlsSettings.LeftPad) };
            settings.Tuning[ControlsSettings.PlayStation].Rumble = 80;
            settings.Tuning[ControlsSettings.Steam].GyroSensitivity = 2.0f;
            settings.Save();
            var text = File.ReadAllText(ControlsSettings.SettingsPath);
            Check(text.Contains("bind.playstation.a=Touchpad\n") && text.Contains("bind.steam.z=LPad\n") && text.Contains("tune.playstation.rumble=80\n") &&
                  text.Contains("tune.steam.gyro_sensitivity=2\n"), "PlayStation and Steam lines are written");
            var loaded = ControlsSettings.Load();
            Check(ControlsSettings.FormatBindings(ControlsSettings.PlayStation, loaded.Bindings[ControlsSettings.PlayStation][0]) == "Touchpad" &&
                  ControlsSettings.FormatBindings(ControlsSettings.Steam, loaded.Bindings[ControlsSettings.Steam][16]) == "LPad" &&
                  loaded.Tuning[ControlsSettings.PlayStation].Rumble == 80 && loaded.Tuning[ControlsSettings.Steam].GyroSensitivity == 2.0f, "the new schemes survive the settings file");

            File.WriteAllText(ControlsSettings.SettingsPath, "controls_version=5\nbind.xbox.a=B\nbind.switch_pro.b=Y\ntune.switch_pro2.rumble=30\nbind.nes.a=A\n");
            var old = ControlsSettings.Load();
            Check(ControlsSettings.FormatBindings(ControlsSettings.Xbox, old.Bindings[ControlsSettings.Xbox][0]) == "B" && old.Tuning[ControlsSettings.SwitchPro2].Rumble == 30, "lines of the older schemes apply");
            bool defaults = true;
            for (int t = 0; t < ControlsSettings.TargetCount; t++)
                defaults &= ControlsSettings.FormatBindings(ControlsSettings.PlayStation, old.Bindings[ControlsSettings.PlayStation][t]) ==
                            ControlsSettings.FormatBindings(ControlsSettings.PlayStation, ControlsSettings.DefaultTable(ControlsSettings.PlayStation)[t]);
            Check(defaults, "a file without the new schemes leaves them at their defaults");

            // The same defaults as the runtime (galaxy_controls_tests --print-defaults), for run-launcher-tests.ps1.
            var dump = new StringBuilder();
            foreach (var pair in new ControlsSettings().Lines()) if (pair.Key != "input_mode" && pair.Key != "controls_version") dump.Append(pair.Key).Append('=').Append(pair.Value).Append('\n');
            File.WriteAllText(Path.Combine(outDir, "controls-defaults.txt"), dump.ToString());
            File.Delete(ControlsSettings.SettingsPath);
        }

        static void Roster()
        {
            var list = DeviceList.Build(new ControllerInfo[0], null);
            Check(list.Count == 1 && list[0].Label == "Keyboard and mouse" && !list[0].Active, "no controllers: keyboard and mouse only");

            var mixed = new List<ControllerInfo>
            {
                Controller(ControlsSettings.PlayStation, 1, ControllerLink.Usb, "DualSense"),
                Controller(ControlsSettings.Xbox, 2, ControllerLink.Unknown, "Xbox controller"),
                Controller(ControlsSettings.PlayStation, 0, ControllerLink.Bluetooth, "DualSense"),
                Controller(ControlsSettings.Steam, 0, ControllerLink.Wireless, "Steam Controller"),
            };
            list = DeviceList.Build(mixed, new DeviceKey(ControlsSettings.PlayStation, 1));
            Check(list.Count == 5, "keyboard plus four controllers");
            Check(list[1].Label == "Xbox controller", "Xbox first, no link named");
            Check(list[2].Label == "DualSense 1 (Bluetooth)" && list[3].Label == "DualSense 2 (USB)", "two controllers of one model are numbered by slot");
            Check(list[4].Label == "Steam Controller (wireless)", "wireless link");
            Check(!list[0].Active && !list[1].Active && !list[2].Active && list[3].Active && !list[4].Active, "the active controller is marked");
            Check(list[3].Text == "DualSense 2 (USB)  (active)" && list[2].Text == list[2].Label, "the active entry is marked in the drop-down");
            Check(DeviceList.Build(mixed, DeviceList.Keyboard)[0].Active, "the keyboard can be the active device");

            list = DeviceList.Build(mixed, null);
            var editing = new DeviceKey(ControlsSettings.PlayStation, 1);
            Check(DeviceList.Resolve(list, editing) == 3, "the edited controller keeps its entry");
            var remaining = DeviceList.Build(new[] { Controller(ControlsSettings.PlayStation, 0, ControllerLink.Usb, "DualShock 4") }, null);
            Check(DeviceList.Resolve(remaining, editing) == 1, "another controller of the family takes over the edit");
            Check(DeviceList.Resolve(DeviceList.Build(new ControllerInfo[0], null), editing) == 0, "no controller left: back to the keyboard");
            Check(DeviceList.SameEntries(list, DeviceList.Build(mixed, new DeviceKey(ControlsSettings.Steam, 0))), "activity does not change the entries");
            Check(!DeviceList.SameEntries(list, remaining), "plugging changes the entries");

            var family = DeviceList.Build(new[]
            {
                Controller(ControlsSettings.PlayStation, 0, ControllerLink.Usb, "DualShock 4"),
                Controller(ControlsSettings.PlayStation, 1, ControllerLink.Usb, "DualSense Edge"),
            }, null);
            Check(family[1].Label == "DualShock 4 (USB)" && family[2].Label == "DualSense Edge (USB)", "models of one family keep their names");

            var many = new List<ControllerInfo>();
            for (int i = 0; i < 16; i++) many.Add(Controller(ControlsSettings.SwitchPro, i, ControllerLink.Usb, "Switch Pro Controller"));
            var capped = DeviceList.Build(many, null);
            Check(capped.Count == DeviceList.MaxControllers + 1 && capped[DeviceList.MaxControllers].Label == "Switch Pro Controller 12 (USB)", "a long list is cut at its capacity");

            int scheme, psModel;
            string name;
            Check(Products.Find(0x054c, 0x0df2, out scheme, out name, out psModel) && scheme == ControlsSettings.PlayStation && name == "DualSense Edge" && psModel == Products.DualSenseEdge, "DualSense Edge");
            Check(Products.Find(0x054c, 0x09cc, out scheme, out name, out psModel) && name == "DualShock 4", "DualShock 4 (2016)");
            Check(Products.Find(0x28de, 0x1302, out scheme, out name, out psModel) && scheme == ControlsSettings.Steam && name == "Steam Controller", "Steam Controller");
            Check(!Products.Find(0x054c, 0x0268, out scheme, out name, out psModel) && !Products.Find(0x28de, 0x1205, out scheme, out name, out psModel), "the DualShock 3 and the Steam Deck are not claimed");
            Check(Products.LinkOf(ControlsSettings.Steam, 0x1304, "") == ControllerLink.Wireless && Products.LinkOf(ControlsSettings.Steam, 0x1303, "") == ControllerLink.Bluetooth &&
                  Products.LinkOf(ControlsSettings.PlayStation, 0x0ce6, @"\\?\hid#{00001124-0000-1000-8000-00805f9b34fb}_vid&0002054c_pid&0ce6#7") == ControllerLink.Bluetooth, "links");
            Check(HidDevices.InterfaceNumber(@"\\?\hid#vid_28de&pid_1304&mi_03#7&1") == 3 && HidDevices.InterfaceNumber(@"\\?\hid#vid_054c&pid_0ce6#7") == -1, "puck interface numbers");
        }

        static void Reports()
        {
            Check(PadReports.Crc32((byte)'1', Encoding.ASCII.GetBytes("23456789"), 8) == 0xCBF43926u, "report CRC is CRC-32");
            var bt = DualShock4Bluetooth();
            Check(PadReports.BluetoothCrcValid(bt, bt.Length), "a sealed Bluetooth report passes the CRC");
            bt[10] ^= 1;
            Check(!PadReports.BluetoothCrcValid(bt, bt.Length), "a flipped bit fails the CRC");

            var r = DualShock4Usb();
            var neutral = Decoded(0, false, r, r.Length);
            Check(neutral != null && neutral.Buttons == 0 && Math.Abs(neutral.Axes[0]) < 0.01f && neutral.Axes[4] == 0f, "neutral DualShock 4");
            r[1] = 255; r[2] = 0; r[3] = 0; r[4] = 255;
            var sticks = Decoded(0, false, r, r.Length);
            Check(Math.Abs(sticks.Axes[0] - 1f) < 1e-3f && Math.Abs(sticks.Axes[1] - 1f) < 1e-3f && Math.Abs(sticks.Axes[2] + 1f) < 1e-3f && Math.Abs(sticks.Axes[3] + 1f) < 1e-3f, "sticks reach +-1, Y up positive");
            r = DualShock4Usb();
            r[5] = 0x01 | 0x20 | 0x80; r[6] = 0x01 | 0x02 | 0x10 | 0x20 | 0x40 | 0x80; r[7] = 0xFF;
            int expected = Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.DpadRight) | Bit(ControlsSettings.South) | Bit(ControlsSettings.North) |
                           Bit(ControlsSettings.LeftShoulder) | Bit(ControlsSettings.RightShoulder) | Bit(ControlsSettings.Back) | Bit(ControlsSettings.Start) |
                           Bit(ControlsSettings.LeftStick) | Bit(ControlsSettings.RightStick) | Bit(ControlsSettings.Home) | Bit(ControlsSettings.Capture);
            Check(Decoded(0, false, r, r.Length).Buttons == expected, "DualShock 4 buttons map to the unified bits");
            r = DualShock4Usb();
            r[6] = 0x04;
            Check(Math.Abs(Decoded(0, false, r, r.Length).Axes[4] - 1f) < 1e-3f, "a digital-only L2 reads as fully pressed");
            r[0] = 2;
            Check(Decoded(0, false, r, r.Length) == null, "an unknown report id is rejected");

            bt = DualShock4Bluetooth();
            bt[3] = 255; bt[7] = 0x28;
            SealInput(bt);
            var moved = Decoded(0, true, bt, bt.Length);
            Check(moved != null && Math.Abs(moved.Axes[0] - 1f) < 1e-3f && (moved.Buttons & Bit(ControlsSettings.South)) != 0, "Bluetooth payload starts at byte 3");
            bt[20] ^= 0x10;
            Check(Decoded(0, true, bt, bt.Length) == null, "a corrupt Bluetooth report is dropped");
            var simple = new byte[547];
            simple[0] = 1;
            for (int i = 1; i <= 4; i++) simple[i] = 128;
            simple[1] = 0; simple[5] = 0x21; simple[6] = 0x10; simple[8] = 200;
            var shortReport = Decoded(0, true, simple, simple.Length);
            Check(shortReport != null && Math.Abs(shortReport.Axes[0] + 1f) < 0.01f && shortReport.Buttons ==
                  (Bit(ControlsSettings.South) | Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.DpadRight) | Bit(ControlsSettings.Back)) &&
                  Math.Abs(shortReport.Axes[4] - 200f / 255f) < 1e-3f, "the short Bluetooth report decodes");

            r = DualSenseUsb();
            r[5] = 255; r[6] = 40; r[8] = 0x80 | 0x02; r[9] = 0x04 | 0x08 | 0x10; r[10] = 0x01 | 0x02 | 0x04 | 0x40 | 0x80;
            var dual = Decoded(1, false, r, r.Length);
            Check(dual.Buttons == (Bit(ControlsSettings.North) | Bit(ControlsSettings.DpadRight) | Bit(ControlsSettings.Back) | Bit(ControlsSettings.Home) |
                                   Bit(ControlsSettings.Capture) | Bit(ControlsSettings.Chat)), "DualSense buttons: mute is Chat, paddles are an Edge feature");
            Check(Math.Abs(dual.Axes[4] - 1f) < 1e-3f && Math.Abs(dual.Axes[5] - 40f / 255f) < 1e-3f, "DualSense triggers");
            Check((Decoded(2, false, r, r.Length).Buttons & (Bit(ControlsSettings.GripLeft) | Bit(ControlsSettings.GripRight))) ==
                  (Bit(ControlsSettings.GripLeft) | Bit(ControlsSettings.GripRight)), "DualSense Edge paddles are GL and GR");
            var dualBt = new byte[78];
            dualBt[0] = 0x31; dualBt[1] = 1;
            for (int i = 2; i <= 5; i++) dualBt[i] = 128;
            dualBt[2] = 255; dualBt[9] = 0x20;
            SealInput(dualBt);
            var dualBluetooth = Decoded(1, true, dualBt, dualBt.Length);
            Check(dualBluetooth != null && Math.Abs(dualBluetooth.Axes[0] - 1f) < 1e-3f && (dualBluetooth.Buttons & Bit(ControlsSettings.South)) != 0, "DualSense Bluetooth payload starts at byte 2");

            var steam = new byte[46];
            steam[0] = 0x45;
            Put32(steam, 2, 0x00000001u | 0x00000040u | 0x00004000u | 0x00080000u | 0x00002000u | 0x00010000u | 0x00000010u | 0x00020000u | 0x00000100u | 0x00400000u);
            Put16(steam, 6, 16384); Put16(steam, 8, -300); Put16(steam, 10, 32767); Put16(steam, 12, -32768); Put16(steam, 14, -16384); Put16(steam, 16, 16384);
            var steamSample = new PadSample();
            Check(PadReports.DecodeSteam(steam, steam.Length, steamSample), "Steam state report decodes");
            Check(steamSample.Buttons == (Bit(ControlsSettings.South) | Bit(ControlsSettings.Start) | Bit(ControlsSettings.Back) | Bit(ControlsSettings.LeftShoulder) |
                                          Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.Home) | Bit(ControlsSettings.Capture) | Bit(ControlsSettings.GripLeft) |
                                          Bit(ControlsSettings.GripRight2) | Bit(ControlsSettings.RightPad)), "Steam buttons map to the unified bits");
            Check(Math.Abs(steamSample.Axes[4] - 0.5f) < 1e-3f && steamSample.Axes[5] == 0f && Math.Abs(steamSample.Axes[0] - 1f) < 1e-3f &&
                  Math.Abs(steamSample.Axes[1] + 1f) < 1e-3f && Math.Abs(steamSample.Axes[2] + 0.5f) < 1e-3f && Math.Abs(steamSample.Axes[3] - 0.5f) < 1e-3f, "Steam sticks and triggers");
            Check(!PadReports.DecodeSteam(new byte[20], 20, steamSample) && !PadReports.DecodeSteam(new byte[46] , 46, steamSample), "short and foreign reports are not state");
            var settingsReport = PadReports.SteamSettingsReport();
            Check(settingsReport.Length == 64 && settingsReport[0] == 1 && settingsReport[1] == 0x87 && settingsReport[2] == 9 && settingsReport[6] == 71 &&
                  settingsReport[9] == 48 && settingsReport[10] == 0x18, "Steam settings report");

            var before = new PadSample { Connected = true };
            var now = new PadSample { Connected = true, Buttons = Bit(ControlsSettings.RightPad) };
            InputSource source;
            Check(PadReports.Detect(before, now, out source) && source.Equals(new InputSource(SourceKind.Button, ControlsSettings.RightPad)), "the trackpad click can be captured");
        }
    }
}
