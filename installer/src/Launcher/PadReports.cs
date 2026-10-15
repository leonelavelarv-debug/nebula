using System;

namespace Nebula.Launcher
{
    /// <summary>One controller's buttons (unified bits, ControlsSettings) and axes (LX, LY, RX, RY up positive; LT, RT 0..1).</summary>
    internal sealed class PadSample
    {
        public bool Connected;
        public int Buttons;
        public readonly float[] Axes = new float[6];

        public bool HasInput
        {
            get
            {
                if (Buttons != 0) return true;
                for (int i = 0; i < 4; i++) if (Math.Abs(Axes[i]) > 0.5f) return true;
                return Axes[4] > 0.4f || Axes[5] > 0.4f;
            }
        }
    }

    /// <summary>One value usage as a generic controller describes it (runtime generic_hid.h ValueRange).</summary>
    internal struct GenericRange
    {
        public bool Present;
        public int Min, Max, Bits;
    }

    /// <summary>The axes (GenericHid axis order), hat and buttons of a generic controller's input reports.</summary>
    internal sealed class GenericLayout
    {
        public readonly GenericRange[] Axes = new GenericRange[GenericHid.AxisCount];
        public GenericRange Hat;
        public int Buttons;
    }

    /// <summary>The raw values last read from a generic controller; bit n-1 of Buttons is button n.</summary>
    internal sealed class GenericRaw
    {
        public readonly uint[] Axes = new uint[GenericHid.AxisCount];
        public uint Hat, Buttons;
    }

    /// <summary>
    /// Generic HID game controllers: Joystick or Game Pad collections no other path reads, mapped
    /// onto the unified pad as the runtime's generic_hid.cpp maps them.
    /// </summary>
    internal static class GenericHid
    {
        public const int PageGenericDesktop = 0x01, PageSimulation = 0x02, PageButton = 0x09;
        public const int UsageHat = 0x39, MaxButtons = 32, MappedButtons = 19, AxisCount = 8;
        // X, Y, Z, Rx, Ry, Rz (Generic Desktop), Accelerator, Brake (Simulation Controls).
        public const int X = 0, Y = 1, Z = 2, Rx = 3, Ry = 4, Rz = 5, Accelerator = 6, Brake = 7;
        private static readonly int[] Usages = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0xC4, 0xC5 };
        private static readonly int[] ButtonBits = {
            ControlsSettings.South, ControlsSettings.East, ControlsSettings.West, ControlsSettings.North,
            ControlsSettings.LeftShoulder, ControlsSettings.RightShoulder, ControlsSettings.GripLeft, ControlsSettings.GripRight,
            ControlsSettings.Back, ControlsSettings.Start, ControlsSettings.LeftStick, ControlsSettings.RightStick,
            ControlsSettings.Home, ControlsSettings.Capture, ControlsSettings.Chat, ControlsSettings.GripLeft2,
            ControlsSettings.GripRight2, ControlsSettings.LeftPad, ControlsSettings.RightPad };

        public static bool IsGameController(int usagePage, int usage) { return usagePage == PageGenericDesktop && (usage == 0x04 || usage == 0x05); }

        /// <summary>Microsoft (XInput), Nintendo, Sony and Valve controllers and XInput interfaces ("IG_") are read elsewhere.</summary>
        public static bool ClaimedElsewhere(int vendor, string path)
        {
            if (vendor == 0x045e || vendor == 0x057e || vendor == 0x054c || vendor == 0x28de) return true;
            return path != null && path.IndexOf("ig_", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>The unified bit of 1-based button `number`, -1 when it has none.</summary>
        public static int ButtonBit(int number) { return number >= 1 && number <= MappedButtons ? ButtonBits[number - 1] : -1; }

        public static int AxisOf(int page, int usage)
        {
            for (int i = 0; i < AxisCount; i++)
                if (Usages[i] == usage && page == (i >= Accelerator ? PageSimulation : PageGenericDesktop)) return i;
            return -1;
        }

        public static ushort AxisPage(int axis) { return (ushort)(axis >= Accelerator ? PageSimulation : PageGenericDesktop); }
        public static ushort AxisUsage(int axis) { return (ushort)Usages[axis]; }

        /// <summary>A field as a signed logical value (sign-extended when the range is signed).</summary>
        public static int LogicalValue(GenericRange range, uint raw)
        {
            if (range.Min < 0 && range.Bits > 0 && range.Bits < 32)
            {
                uint sign = 1u << (range.Bits - 1);
                raw &= (sign << 1) - 1u;
                return (int)(raw ^ sign) - (int)sign;
            }
            return (int)raw;
        }

        private static float Centred(GenericRange range, uint raw)
        {
            if (!range.Present || range.Max <= range.Min) return 0f;
            double span = (double)range.Max - range.Min, value = (double)LogicalValue(range, raw) - range.Min;
            return Math.Max(-1f, Math.Min(1f, (float)(value / span * 2.0 - 1.0)));
        }

        private static float Pulled(GenericRange range, uint raw)
        {
            if (!range.Present || range.Max <= range.Min) return 0f;
            double span = (double)range.Max - range.Min, value = (double)LogicalValue(range, raw) - range.Min;
            return Math.Max(0f, Math.Min(1f, (float)(value / span)));
        }

        /// <summary>
        /// Sticks -1..1 (up positive), triggers 0..1, buttons and the hat as the d-pad. The right stick is
        /// Z/Rz when the device has both, else Rx/Ry; triggers come from the pair it leaves free, then
        /// Brake/Accelerator.
        /// </summary>
        public static void Map(GenericLayout layout, GenericRaw raw, PadSample s)
        {
            int b = 0;
            for (int number = 1; number <= MappedButtons; number++)
                if ((raw.Buttons & (1u << (number - 1))) != 0) b |= 1 << ButtonBit(number);
            bool zPair = layout.Axes[Z].Present && layout.Axes[Rz].Present;
            int rightX = zPair ? Z : Rx, rightY = zPair ? Rz : Ry, leftTrigger = zPair ? Rx : Z, rightTrigger = zPair ? Ry : Rz;
            s.Axes[0] = Centred(layout.Axes[X], raw.Axes[X]);
            s.Axes[1] = -Centred(layout.Axes[Y], raw.Axes[Y]);
            s.Axes[2] = Centred(layout.Axes[rightX], raw.Axes[rightX]);
            s.Axes[3] = -Centred(layout.Axes[rightY], raw.Axes[rightY]);
            s.Axes[4] = layout.Axes[leftTrigger].Present ? Pulled(layout.Axes[leftTrigger], raw.Axes[leftTrigger]) : Pulled(layout.Axes[Brake], raw.Axes[Brake]);
            s.Axes[5] = layout.Axes[rightTrigger].Present ? Pulled(layout.Axes[rightTrigger], raw.Axes[rightTrigger]) : Pulled(layout.Axes[Accelerator], raw.Axes[Accelerator]);
            if (layout.Hat.Present && layout.Hat.Max > layout.Hat.Min)
            {
                long at = (long)LogicalValue(layout.Hat, raw.Hat) - layout.Hat.Min, count = (long)layout.Hat.Max - layout.Hat.Min + 1;
                if (at >= 0 && at < count && (count == 8 || count == 4))
                {
                    int direction = (int)(count == 4 ? at * 2 : at);
                    int up = 1 << ControlsSettings.DpadUp, down = 1 << ControlsSettings.DpadDown;
                    int left = 1 << ControlsSettings.DpadLeft, right = 1 << ControlsSettings.DpadRight;
                    int[] hat = { up, up | right, right, down | right, down, down | left, left, up | left };
                    b |= hat[direction];
                }
            }
            s.Buttons = b;
            s.Connected = true;
        }
    }

    /// <summary>
    /// Input report decoding for binding capture: buttons, sticks and triggers only. Layouts as the
    /// runtime's switch_controller.cpp, playstation_controller.cpp and steam_controller.cpp.
    /// </summary>
    internal static class PadReports
    {
        // ---- shared -------------------------------------------------------------
        private static int Bit(int index) { return 1 << index; }
        private static int S16(byte[] d, int i) { return (short)(d[i] | (d[i + 1] << 8)); }
        private static uint U32(byte[] d, int i) { return (uint)(d[i] | (d[i + 1] << 8) | (d[i + 2] << 16) | (d[i + 3] << 24)); }

        /// <summary>First input that became active since `baseline` (runtime detect_pad_capture).</summary>
        public static bool Detect(PadSample baseline, PadSample now, out InputSource source)
        {
            source = new InputSource();
            if (!now.Connected) return false;
            int pressed = now.Buttons & ~(baseline.Connected ? baseline.Buttons : 0);
            for (int i = 0; i < ControlsSettings.ButtonCount; i++)
                if ((pressed & (1 << i)) != 0) { source = new InputSource(SourceKind.Button, i); return true; }
            for (int i = 0; i < 6; i++)
            {
                float before = baseline.Connected ? baseline.Axes[i] : 0f, value = now.Axes[i];
                if (i >= 4)
                {
                    if (value >= 0.5f && before < 0.5f) { source = new InputSource(SourceKind.AxisPositive, i); return true; }
                    continue;
                }
                if (value >= 0.6f && before < 0.6f) { source = new InputSource(SourceKind.AxisPositive, i); return true; }
                if (value <= -0.6f && before > -0.6f) { source = new InputSource(SourceKind.AxisNegative, i); return true; }
            }
            return false;
        }

        // ---- Switch ----------------------------------------------------------------
        private static int U12Low(byte[] d, int i) { return d[i] | ((d[i + 1] & 0x0F) << 8); }
        private static int U12High(byte[] d, int i) { return (d[i] >> 4) | (d[i + 1] << 4); }
        private static float Stick(int raw) { return Math.Max(-1f, Math.Min(1f, (raw - 2048) / 1200f)); }

        /// <summary>Switch Pro Controller (0x30, 0x21, 0x3F) and Switch 2 Pro Controller (0x05) reports.</summary>
        public static bool DecodeSwitch(byte[] r, int size, PadSample s)
        {
            int b = 0;
            if (size >= 13 && (r[0] == 0x30 || r[0] == 0x21))
            {
                byte right = r[3], shared = r[4], left = r[5];
                if ((right & 0x01) != 0) b |= Bit(ControlsSettings.West);
                if ((right & 0x02) != 0) b |= Bit(ControlsSettings.North);
                if ((right & 0x04) != 0) b |= Bit(ControlsSettings.South);
                if ((right & 0x08) != 0) b |= Bit(ControlsSettings.East);
                if ((right & 0x40) != 0) b |= Bit(ControlsSettings.RightShoulder);
                if ((shared & 0x01) != 0) b |= Bit(ControlsSettings.Back);
                if ((shared & 0x02) != 0) b |= Bit(ControlsSettings.Start);
                if ((shared & 0x04) != 0) b |= Bit(ControlsSettings.RightStick);
                if ((shared & 0x08) != 0) b |= Bit(ControlsSettings.LeftStick);
                if ((shared & 0x10) != 0) b |= Bit(ControlsSettings.Home);
                if ((shared & 0x20) != 0) b |= Bit(ControlsSettings.Capture);
                if ((left & 0x01) != 0) b |= Bit(ControlsSettings.DpadDown);
                if ((left & 0x02) != 0) b |= Bit(ControlsSettings.DpadUp);
                if ((left & 0x04) != 0) b |= Bit(ControlsSettings.DpadRight);
                if ((left & 0x08) != 0) b |= Bit(ControlsSettings.DpadLeft);
                if ((left & 0x40) != 0) b |= Bit(ControlsSettings.LeftShoulder);
                s.Axes[0] = Stick(U12Low(r, 6)); s.Axes[1] = Stick(U12High(r, 7));
                s.Axes[2] = Stick(U12Low(r, 9)); s.Axes[3] = Stick(U12High(r, 10));
                s.Axes[4] = (left & 0x80) != 0 ? 1f : 0f; s.Axes[5] = (right & 0x80) != 0 ? 1f : 0f;
            }
            else if (size >= 12 && r[0] == 0x3F)
            {
                byte b0 = r[1], b1 = r[2];
                int[] map0 = { ControlsSettings.South, ControlsSettings.East, ControlsSettings.West, ControlsSettings.North,
                    ControlsSettings.LeftShoulder, ControlsSettings.RightShoulder };
                for (int i = 0; i < 6; i++) if ((b0 & (1 << i)) != 0) b |= Bit(map0[i]);
                int[] map1 = { ControlsSettings.Back, ControlsSettings.Start, ControlsSettings.LeftStick, ControlsSettings.RightStick,
                    ControlsSettings.Home, ControlsSettings.Capture };
                for (int i = 0; i < 6; i++) if ((b1 & (1 << i)) != 0) b |= Bit(map1[i]);
                int[] hat = { 0x1, 0x9, 0x8, 0xA, 0x2, 0x6, 0x4, 0x5 }; // up, up-right, right, ... in dpad bits
                if (r[3] < 8) b |= hat[r[3]];
                for (int i = 0; i < 4; i++)
                {
                    float v = Math.Max(-1f, Math.Min(1f, ((r[4 + i * 2] | (r[5 + i * 2] << 8)) - 0x8000) / 20000f));
                    s.Axes[i] = (i % 2) == 1 ? -v : v;
                }
                s.Axes[4] = (b0 & 0x40) != 0 ? 1f : 0f; s.Axes[5] = (b0 & 0x80) != 0 ? 1f : 0f;
            }
            else if (size >= 64 && r[0] == 0x05)
            {
                byte b5 = r[5], b6 = r[6], b7 = r[7], b8 = r[8];
                if ((b5 & 0x01) != 0) b |= Bit(ControlsSettings.West);
                if ((b5 & 0x02) != 0) b |= Bit(ControlsSettings.North);
                if ((b5 & 0x04) != 0) b |= Bit(ControlsSettings.South);
                if ((b5 & 0x08) != 0) b |= Bit(ControlsSettings.East);
                if ((b5 & 0x40) != 0) b |= Bit(ControlsSettings.RightShoulder);
                if ((b6 & 0x01) != 0) b |= Bit(ControlsSettings.Back);
                if ((b6 & 0x02) != 0) b |= Bit(ControlsSettings.Start);
                if ((b6 & 0x04) != 0) b |= Bit(ControlsSettings.RightStick);
                if ((b6 & 0x08) != 0) b |= Bit(ControlsSettings.LeftStick);
                if ((b6 & 0x10) != 0) b |= Bit(ControlsSettings.Home);
                if ((b6 & 0x20) != 0) b |= Bit(ControlsSettings.Capture);
                if ((b6 & 0x40) != 0) b |= Bit(ControlsSettings.Chat);
                if ((b7 & 0x01) != 0) b |= Bit(ControlsSettings.DpadDown);
                if ((b7 & 0x02) != 0) b |= Bit(ControlsSettings.DpadUp);
                if ((b7 & 0x04) != 0) b |= Bit(ControlsSettings.DpadRight);
                if ((b7 & 0x08) != 0) b |= Bit(ControlsSettings.DpadLeft);
                if ((b7 & 0x40) != 0) b |= Bit(ControlsSettings.LeftShoulder);
                if ((b8 & 0x01) != 0) b |= Bit(ControlsSettings.GripRight);
                if ((b8 & 0x02) != 0) b |= Bit(ControlsSettings.GripLeft);
                s.Axes[0] = Stick(U12Low(r, 11)); s.Axes[1] = Stick(U12High(r, 12));
                s.Axes[2] = Stick(U12Low(r, 14)); s.Axes[3] = Stick(U12High(r, 15));
                s.Axes[4] = (b7 & 0x80) != 0 ? 1f : 0f; s.Axes[5] = (b5 & 0x80) != 0 ? 1f : 0f;
            }
            else return false;
            s.Buttons = b;
            s.Connected = true;
            return true;
        }

        // ---- PlayStation -----------------------------------------------------------
        /// <summary>CRC-32 of the seed byte followed by data[0..length): the Bluetooth report check (input seed 0xA1, output 0xA2).</summary>
        public static uint Crc32(byte seed, byte[] data, int length)
        {
            uint crc = 0xFFFFFFFFu;
            crc = Step(crc, seed);
            for (int i = 0; i < length; i++) crc = Step(crc, data[i]);
            return ~crc;
        }

        private static uint Step(uint crc, byte value)
        {
            crc ^= value;
            for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            return crc;
        }

        public static bool BluetoothCrcValid(byte[] r, int size)
        {
            return size >= 78 && Crc32(0xA1, r, 74) == U32(r, 74);
        }

        private static readonly int[] HatBits =
        {
            Bit(ControlsSettings.DpadUp),
            Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.DpadRight),
            Bit(ControlsSettings.DpadRight),
            Bit(ControlsSettings.DpadDown) | Bit(ControlsSettings.DpadRight),
            Bit(ControlsSettings.DpadDown),
            Bit(ControlsSettings.DpadDown) | Bit(ControlsSettings.DpadLeft),
            Bit(ControlsSettings.DpadLeft),
            Bit(ControlsSettings.DpadUp) | Bit(ControlsSettings.DpadLeft),
        };

        private static float PlayStationStick(byte raw, bool upPositive)
        {
            float offset = raw - 128f;
            float value = offset >= 0f ? offset / 127f : offset / 128f;
            return upPositive ? -value : value;
        }

        private static float PlayStationTrigger(byte analog, bool digital) { return analog == 0 && digital ? 1f : analog / 255f; }

        /// <summary>
        /// DualShock 4 (model 0), DualSense (1) and DualSense Edge (2) over USB or Bluetooth. Bluetooth sends
        /// the short report 0x01 until a calibration feature report is read, then 0x11..0x19 / 0x31.
        /// </summary>
        public static bool DecodePlayStation(int model, bool bluetooth, byte[] r, int size, PadSample s)
        {
            if (size < 1) return false;
            bool dualsense = model != Products.DualShock4;
            int p;
            bool full;
            if (!bluetooth)
            {
                if (r[0] != 0x01 || size < (dualsense ? 32 : 25)) return false;
                p = 1; full = true;
            }
            else if (r[0] == 0x01)
            {
                if (size < 10) return false;
                p = 1; full = false;
            }
            else
            {
                bool id = dualsense ? r[0] == 0x31 : (r[0] >= 0x11 && r[0] <= 0x19);
                if (!id || size < 78 || !BluetoothCrcValid(r, size)) return false;
                if (!dualsense && (r[1] & 0x80) == 0) return false;
                p = dualsense ? 2 : 3; full = true;
            }
            int buttons, triggerBits;
            byte leftTrigger, rightTrigger;
            if (full && dualsense)
            {
                buttons = p + 7; triggerBits = r[p + 8];
                leftTrigger = r[p + 4]; rightTrigger = r[p + 5];
            }
            else
            {
                buttons = p + 4; triggerBits = r[p + 5];
                leftTrigger = r[p + 7]; rightTrigger = r[p + 8];
            }
            byte b0 = r[buttons], b1 = r[buttons + 1], b2 = r[buttons + 2];
            int b = 0;
            if ((b0 & 0x0F) < 8) b |= HatBits[b0 & 0x0F];
            if ((b0 & 0x10) != 0) b |= Bit(ControlsSettings.West);
            if ((b0 & 0x20) != 0) b |= Bit(ControlsSettings.South);
            if ((b0 & 0x40) != 0) b |= Bit(ControlsSettings.East);
            if ((b0 & 0x80) != 0) b |= Bit(ControlsSettings.North);
            if ((b1 & 0x01) != 0) b |= Bit(ControlsSettings.LeftShoulder);
            if ((b1 & 0x02) != 0) b |= Bit(ControlsSettings.RightShoulder);
            if ((b1 & 0x10) != 0) b |= Bit(ControlsSettings.Back);
            if ((b1 & 0x20) != 0) b |= Bit(ControlsSettings.Start);
            if ((b1 & 0x40) != 0) b |= Bit(ControlsSettings.LeftStick);
            if ((b1 & 0x80) != 0) b |= Bit(ControlsSettings.RightStick);
            if ((b2 & 0x01) != 0) b |= Bit(ControlsSettings.Home);
            if ((b2 & 0x02) != 0) b |= Bit(ControlsSettings.Capture);
            if (full && dualsense)
            {
                if ((b2 & 0x04) != 0) b |= Bit(ControlsSettings.Chat);
                if (model == Products.DualSenseEdge)
                {
                    if ((b2 & 0x40) != 0) b |= Bit(ControlsSettings.GripLeft);
                    if ((b2 & 0x80) != 0) b |= Bit(ControlsSettings.GripRight);
                }
            }
            s.Axes[0] = PlayStationStick(r[p], false); s.Axes[1] = PlayStationStick(r[p + 1], true);
            s.Axes[2] = PlayStationStick(r[p + 2], false); s.Axes[3] = PlayStationStick(r[p + 3], true);
            s.Axes[4] = PlayStationTrigger(leftTrigger, (triggerBits & 0x04) != 0);
            s.Axes[5] = PlayStationTrigger(rightTrigger, (triggerBits & 0x08) != 0);
            s.Buttons = b;
            s.Connected = true;
            return true;
        }

        // ---- Steam Controller ---------------------------------------------------------
        /// <summary>The 2026 Steam Controller's state reports 0x42 (USB), 0x45 (Bluetooth LE) and 0x47.</summary>
        public static bool DecodeSteam(byte[] r, int size, PadSample s)
        {
            if (size < 46 || (r[0] != 0x42 && r[0] != 0x45 && r[0] != 0x47)) return false;
            uint word = U32(r, 2);
            int b = 0;
            if ((word & 0x00000001u) != 0) b |= Bit(ControlsSettings.South);
            if ((word & 0x00000002u) != 0) b |= Bit(ControlsSettings.East);
            if ((word & 0x00000004u) != 0) b |= Bit(ControlsSettings.West);
            if ((word & 0x00000008u) != 0) b |= Bit(ControlsSettings.North);
            if ((word & 0x00000010u) != 0) b |= Bit(ControlsSettings.Capture);
            if ((word & 0x00000020u) != 0) b |= Bit(ControlsSettings.RightStick);
            if ((word & 0x00000040u) != 0) b |= Bit(ControlsSettings.Start);
            if ((word & 0x00000080u) != 0) b |= Bit(ControlsSettings.GripRight);
            if ((word & 0x00000100u) != 0) b |= Bit(ControlsSettings.GripRight2);
            if ((word & 0x00000200u) != 0) b |= Bit(ControlsSettings.RightShoulder);
            if ((word & 0x00000400u) != 0) b |= Bit(ControlsSettings.DpadDown);
            if ((word & 0x00000800u) != 0) b |= Bit(ControlsSettings.DpadRight);
            if ((word & 0x00001000u) != 0) b |= Bit(ControlsSettings.DpadLeft);
            if ((word & 0x00002000u) != 0) b |= Bit(ControlsSettings.DpadUp);
            if ((word & 0x00004000u) != 0) b |= Bit(ControlsSettings.Back);
            if ((word & 0x00008000u) != 0) b |= Bit(ControlsSettings.LeftStick);
            if ((word & 0x00010000u) != 0) b |= Bit(ControlsSettings.Home);
            if ((word & 0x00020000u) != 0) b |= Bit(ControlsSettings.GripLeft);
            if ((word & 0x00040000u) != 0) b |= Bit(ControlsSettings.GripLeft2);
            if ((word & 0x00080000u) != 0) b |= Bit(ControlsSettings.LeftShoulder);
            if ((word & 0x00400000u) != 0) b |= Bit(ControlsSettings.RightPad);
            if ((word & 0x04000000u) != 0) b |= Bit(ControlsSettings.LeftPad);
            s.Axes[0] = Unit(S16(r, 10)); s.Axes[1] = Unit(S16(r, 12));
            s.Axes[2] = Unit(S16(r, 14)); s.Axes[3] = Unit(S16(r, 16));
            s.Axes[4] = Math.Max(Unit(S16(r, 6)), 0f); s.Axes[5] = Math.Max(Unit(S16(r, 8)), 0f);
            s.Buttons = b;
            s.Connected = true;
            return true;
        }

        private static float Unit(int value) { return Math.Max(-1f, Math.Min(1f, value / 32767f)); }

        /// <summary>Feature report 1 that turns off the keyboard/mouse emulation and starts the sensors (steam_controller.cpp build_settings_report).</summary>
        public static byte[] SteamSettingsReport()
        {
            var report = new byte[64];
            report[0] = 1; report[1] = 0x87; report[2] = 9;
            report[3] = 9; report[4] = 0; report[5] = 0;       // lizard mode off
            report[6] = 71; report[7] = 0; report[8] = 0;      // Steam watchdog off
            report[9] = 48; report[10] = 0x18; report[11] = 0; // raw accelerometer and gyro
            return report;
        }
    }
}
