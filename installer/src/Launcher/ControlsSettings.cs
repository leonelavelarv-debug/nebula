using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace Nebula.Launcher
{
    internal enum SourceKind { None = 0, Key = 1, Mouse = 2, Button = 3, AxisPositive = 4, AxisNegative = 5 }

    internal struct InputSource
    {
        public SourceKind Kind;
        public int Code;
        public InputSource(SourceKind kind, int code) { Kind = kind; Code = code; }
        public bool IsEmpty { get { return Kind == SourceKind.None; } }
        public override bool Equals(object other) { return other is InputSource && ((InputSource)other).Kind == Kind && ((InputSource)other).Code == Code; }
        public override int GetHashCode() { return ((int)Kind << 8) | Code; }
    }

    internal sealed class SchemeTuning
    {
        public float StickDeadzone, PointerDeadzone, PointerSpeed = 1.6f;
        public bool InvertPointerY, GyroPointer;
        public float GyroSensitivity = 1.0f;
        public float PointerHideSeconds, PointerHideRadius = 0.12f;
        public int Motion; // 0 off, 1 Wii Remote, 2 Nunchuk
        public int ShakeSensitivity = 3;
        public bool Vibration = true;
        public int Rumble = 50; // vibration strength percent, 10..100 in steps of 10
        public SchemeTuning Clone() { return (SchemeTuning)MemberwiseClone(); }
    }

    /// <summary>
    /// The CONTROLS keys of runtime_settings.ini (input_bindings.cpp): the input
    /// source, one bind.&lt;scheme&gt;.&lt;target&gt; line per Wii input and the
    /// tune.&lt;scheme&gt;.* lines. Names, defaults and rules match the runtime.
    /// </summary>
    internal sealed class ControlsSettings
    {
        public const int SchemeCount = 6, TargetCount = 25, Slots = 4;
        public const int JumpBack = 24;
        // runtime_settings.ini controls_version (input_bindings.h kControlsVersion).
        public const int ControlsVersion = 5;
        // Version 3 still-pointer defaults (pads), replaced in version 4.
        public const float Version3PointerHideSeconds = 4.0f, Version3PointerHideRadius = 0.03f;
        // Version 4 still-pointer time and vibration strength, replaced in version 5.
        public const float Version4PointerHideSeconds = 2.0f;
        public const int Version4Rumble = 100;
        public const int ModeAuto = 0, ModeKeyboardMouse = 1, ModeController = 2, ModeRealWiimote = 3;
        public const int Keyboard = 0, Xbox = 1, SwitchPro = 2, SwitchPro2 = 3, PlayStation = 4, Steam = 5;
        public static readonly string[] SchemeIds = { "keyboard", "xbox", "switch_pro", "switch_pro2", "playstation", "steam" };
        public static readonly string[] SchemeLabels = { "Keyboard and mouse", "Xbox controller", "Switch Pro Controller", "Switch 2 Pro Controller", "PlayStation controller", "Steam Controller" };
        public static readonly string[] TargetIds = {
            "a", "b", "one", "two", "plus", "minus", "home", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
            "stick_up", "stick_down", "stick_left", "stick_right", "c", "z", "shake", "nunchuk_shake",
            "pointer_up", "pointer_down", "pointer_left", "pointer_right", "pointer_recenter", "jump_back" };
        public static readonly string[] TargetLabels = {
            "A", "B", "1", "2", "Plus", "Minus", "Home", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
            "Nunchuk stick up", "Nunchuk stick down", "Nunchuk stick left", "Nunchuk stick right", "C", "Z",
            "Shake (Wii Remote)", "Shake (Nunchuk)", "Pointer up", "Pointer down", "Pointer left", "Pointer right",
            "Pointer recenter", "A in play, B in menus" };
        public static readonly string[] MotionNames = { "off", "remote", "nunchuk" };
        public static readonly string[] MotionLabels = { "Off", "Wii Remote shake", "Nunchuk shake" };
        // tune.<scheme>.rumble steps (rumble.h).
        public static readonly int[] RumbleSteps = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
        public static readonly string[] RumbleLabels = { "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%" };

        /// <summary>The nearest step within 10..100, ties down (rumble.h snap_strength).</summary>
        public static int SnapRumble(float percent)
        {
            int best = RumbleSteps[0];
            foreach (int step in RumbleSteps)
                if (Math.Abs(percent - step) < Math.Abs(percent - best)) best = step;
            return best;
        }

        // Unified pad bits (input_bindings.h pad::).
        public const int DpadUp = 0, DpadDown = 1, DpadLeft = 2, DpadRight = 3, Start = 4, Back = 5, LeftStick = 6,
            RightStick = 7, LeftShoulder = 8, RightShoulder = 9, Home = 10, Capture = 11, South = 12, East = 13,
            West = 14, North = 15, GripLeft = 16, GripRight = 17, Chat = 18, GripLeft2 = 19, GripRight2 = 20,
            LeftPad = 21, RightPad = 22, ButtonCount = 23;
        public const int LeftX = 0, LeftY = 1, RightX = 2, RightY = 3, LeftTrigger = 4, RightTrigger = 5;

        private static readonly string[] XboxButtons = { "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Menu", "View", "LS", "RS",
            "LB", "RB", "Guide", "Share", "A", "B", "X", "Y", "GL", "GR", "C", "GL2", "GR2", "LPad", "RPad" };
        private static readonly string[] SwitchButtons = { "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Plus", "Minus", "LS", "RS",
            "L", "R", "Home", "Capture", "B", "A", "Y", "X", "GL", "GR", "C", "GL2", "GR2", "LPad", "RPad" };
        private static readonly string[] PlayStationButtons = { "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Options", "Share", "L3", "R3",
            "L1", "R1", "PS", "Touchpad", "Cross", "Circle", "Square", "Triangle", "LeftPaddle", "RightPaddle", "Mute", "GL2", "GR2", "LPad", "RPad" };
        private static readonly string[] SteamButtons = { "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Menu", "View", "LS", "RS",
            "LB", "RB", "Steam", "QAM", "A", "B", "X", "Y", "L4", "R4", "C", "L5", "R5", "LPad", "RPad" };
        private static readonly string[] StickAxes = { "LX", "LY", "RX", "RY" };
        private static readonly string[] MouseNames = { "MouseLeft", "MouseRight", "MouseMiddle", "Mouse4", "Mouse5" };
        private static readonly KeyValuePair<int, string>[] NamedKeys = {
            K(0x20, "Space"), K(0x0D, "Enter"), K(0x09, "Tab"), K(0x08, "Backspace"), K(0x1B, "Escape"),
            K(0xA0, "LShift"), K(0xA1, "RShift"), K(0x10, "Shift"), K(0xA2, "LCtrl"), K(0xA3, "RCtrl"), K(0x11, "Ctrl"),
            K(0xA4, "LAlt"), K(0xA5, "RAlt"), K(0x12, "Alt"), K(0x26, "Up"), K(0x28, "Down"), K(0x25, "Left"), K(0x27, "Right"),
            K(0x2D, "Insert"), K(0x2E, "Delete"), K(0x24, "Home"), K(0x23, "End"), K(0x21, "PageUp"), K(0x22, "PageDown"),
            K(0x14, "CapsLock"), K(0xBD, "Minus"), K(0xBB, "Equals"), K(0xDB, "LBracket"), K(0xDD, "RBracket"),
            K(0xDC, "Backslash"), K(0xBA, "Semicolon"), K(0xDE, "Quote"), K(0xBC, "Comma"), K(0xBE, "Period"),
            K(0xBF, "Slash"), K(0xC0, "Grave"), K(0x6B, "NumAdd"), K(0x6D, "NumSub"), K(0x6A, "NumMul"),
            K(0x6F, "NumDiv"), K(0x6E, "NumDecimal") };
        private static KeyValuePair<int, string> K(int vk, string name) { return new KeyValuePair<int, string>(vk, name); }

        public int InputMode; // ModeAuto, ModeKeyboardMouse, ModeController, ModeRealWiimote (kept, not offered)
        public readonly InputSource[][][] Bindings = new InputSource[SchemeCount][][];
        public readonly SchemeTuning[] Tuning = new SchemeTuning[SchemeCount];

        public static string SettingsPath { get { return AudioSettings.SettingsPath; } }

        public ControlsSettings()
        {
            for (int s = 0; s < SchemeCount; s++) { Bindings[s] = DefaultTable(s); Tuning[s] = DefaultTuning(s); }
        }

        public static bool IsAnalog(int target) { return (target >= 11 && target <= 14) || (target >= 19 && target <= 22); }
        public static bool HasMotion(int scheme) { return scheme == SwitchPro || scheme == SwitchPro2 || scheme == PlayStation || scheme == Steam; }
        private static bool IsGyroPad(int scheme) { return scheme == PlayStation || scheme == Steam; }

        private static string[] ButtonNames(int scheme)
        {
            switch (scheme)
            {
                case PlayStation: return PlayStationButtons;
                case Steam: return SteamButtons;
                case Xbox: return XboxButtons;
                default: return SwitchButtons;
            }
        }

        private static string TriggerName(int scheme, bool left)
        {
            switch (scheme)
            {
                case Xbox:
                case Steam: return left ? "LT" : "RT";
                case PlayStation: return left ? "L2" : "R2";
                default: return left ? "ZL" : "ZR";
            }
        }

        /// <summary>The unified button bits a scheme's controllers have (input_bindings.h scheme_button_mask).</summary>
        public static int ButtonMask(int scheme)
        {
            const int baseBits = (1 << Home) - 1, homeCapture = (1 << Home) | (1 << Capture), face = 0xF000;
            const int grips = (1 << GripLeft) | (1 << GripRight);
            switch (scheme)
            {
                case Xbox: return baseBits | face;
                case SwitchPro: return baseBits | homeCapture | face;
                case SwitchPro2: return baseBits | homeCapture | face | grips | (1 << Chat);
                case PlayStation: return baseBits | homeCapture | face | grips | (1 << Chat);
                case Steam: return baseBits | homeCapture | face | grips | (1 << GripLeft2) | (1 << GripRight2) | (1 << LeftPad) | (1 << RightPad);
            }
            return 0;
        }

        private static InputSource Key(int vk) { return new InputSource(SourceKind.Key, vk); }
        private static InputSource Mouse(int b) { return new InputSource(SourceKind.Mouse, b); }
        private static InputSource Button(int b) { return new InputSource(SourceKind.Button, b); }
        private static InputSource Pos(int a) { return new InputSource(SourceKind.AxisPositive, a); }
        private static InputSource Neg(int a) { return new InputSource(SourceKind.AxisNegative, a); }

        /// <summary>The defaults used before controls version 2, kept for migration.</summary>
        public static InputSource[][] LegacyTable(int scheme)
        {
            if (IsGyroPad(scheme)) return GyroPadTable();
            var t = new InputSource[TargetCount][];
            for (int i = 0; i < TargetCount; i++) t[i] = new InputSource[0];
            if (scheme == Keyboard)
            {
                t[0] = new[] { Key(0x20), Key(0x0D), Mouse(0) };
                t[1] = new[] { Key(0xA0), Key(0xA1), Mouse(1) };
                t[2] = new[] { Key('1'), Key(0x61) };
                t[3] = new[] { Key('2'), Key(0x62) };
                t[4] = new[] { Key(0xBB), Key('P') };
                t[5] = new[] { Key(0xBD), Key('M') };
                t[6] = new[] { Key('H') };
                t[7] = new[] { Key(0x26) }; t[8] = new[] { Key(0x28) }; t[9] = new[] { Key(0x25) }; t[10] = new[] { Key(0x27) };
                t[11] = new[] { Key('W') }; t[12] = new[] { Key('S') }; t[13] = new[] { Key('A') }; t[14] = new[] { Key('D') };
                t[15] = new[] { Key('C') };
                t[16] = new[] { Key('Z'), Key(0xA2), Key(0xA3) };
                t[17] = new[] { Key('Q'), Key('E') };
                t[19] = new[] { Key('I') }; t[20] = new[] { Key('K') }; t[21] = new[] { Key('J') }; t[22] = new[] { Key('L') };
                t[23] = new[] { Key('R') };
                return t;
            }
            t[7] = new[] { Button(DpadUp) }; t[8] = new[] { Button(DpadDown) }; t[9] = new[] { Button(DpadLeft) }; t[10] = new[] { Button(DpadRight) };
            t[4] = new[] { Button(Start) }; t[5] = new[] { Button(Back) };
            t[11] = new[] { Pos(LeftY) }; t[12] = new[] { Neg(LeftY) }; t[13] = new[] { Neg(LeftX) }; t[14] = new[] { Pos(LeftX) };
            t[15] = new[] { Button(LeftStick) };
            t[16] = new[] { Button(LeftShoulder), Pos(LeftTrigger) };
            t[19] = new[] { Pos(RightY) }; t[20] = new[] { Neg(RightY) }; t[21] = new[] { Neg(RightX) }; t[22] = new[] { Pos(RightX) };
            t[23] = new[] { Button(RightStick) };
            t[17] = new[] { Button(West), Button(North), Button(RightShoulder) };
            if (scheme == Xbox)
            {
                t[0] = new[] { Button(South) };
                t[1] = new[] { Button(East), Pos(RightTrigger) };
            }
            else
            {
                t[0] = new[] { Button(East) };
                t[1] = new[] { Button(South), Pos(RightTrigger) };
                t[6] = new[] { Button(Home) };
            }
            return t;
        }

        /// <summary>The defaults of controls version 2, kept for migration.</summary>
        public static InputSource[][] Version2Table(int scheme)
        {
            var t = LegacyTable(scheme);
            if (scheme == Keyboard || IsGyroPad(scheme)) return t;
            bool xbox = scheme == Xbox;
            t[0] = new[] { Button(xbox ? South : East) };
            t[JumpBack] = new[] { Button(xbox ? East : South) };
            t[1] = new[] { Pos(RightTrigger) };
            t[16] = new[] { Pos(LeftTrigger) };
            t[15] = new[] { Button(LeftShoulder), Button(LeftStick) };
            if (xbox)
            {
                t[17] = new[] { Button(West), Button(North), Button(RightShoulder) };
                t[23] = new[] { Button(RightStick) };
            }
            else
            {
                t[17] = new[] { Button(West), Button(North) };
                t[23] = new[] { Button(RightShoulder) };
                t[7] = new[] { Button(DpadUp), Pos(RightY) };
                t[8] = new[] { Button(DpadDown), Neg(RightY) };
                t[9] = new[] { Button(DpadLeft), Neg(RightX) };
                t[10] = new[] { Button(DpadRight), Pos(RightX) };
            }
            return t;
        }

        /// <summary>The defaults of controls version 3, kept for migration.</summary>
        public static InputSource[][] Version3Table(int scheme)
        {
            var t = Version2Table(scheme);
            if (scheme == Keyboard || IsGyroPad(scheme)) return t;
            t[1] = new[] { Button(LeftShoulder) };
            t[16] = new[] { Pos(LeftTrigger), Pos(RightTrigger) };
            t[15] = new[] { Button(LeftStick) };
            return t;
        }

        /// <summary>
        /// Super Mario Galaxy on Switch with the player's choices: A or B jump
        /// (B backs out of menus), L/LB Star Bits, ZL/LT crouch, X/Y/ZR (Xbox
        /// X/Y/RB/RT) or shake spin, left-stick click camera, R / right-stick
        /// click recenters (held: pinned at the centre), gyro points, right
        /// stick turns the camera (pointer when the gyro is off; Xbox: pointer).
        /// </summary>
        public static InputSource[][] DefaultTable(int scheme)
        {
            var t = Version3Table(scheme);
            if (scheme == Keyboard || IsGyroPad(scheme)) return t;
            t[16] = new[] { Pos(LeftTrigger) };
            t[17] = scheme == Xbox
                ? new[] { Button(West), Button(North), Button(RightShoulder), Pos(RightTrigger) }
                : new[] { Button(West), Button(North), Pos(RightTrigger) };
            return t;
        }

        /// <summary>
        /// PlayStation and Steam controllers: the Switch gyro layout with the Xbox face positions
        /// (South confirms and jumps, East backs out of menus).
        /// </summary>
        private static InputSource[][] GyroPadTable()
        {
            var t = new InputSource[TargetCount][];
            for (int i = 0; i < TargetCount; i++) t[i] = new InputSource[0];
            t[0] = new[] { Button(South) };
            t[JumpBack] = new[] { Button(East) };
            t[1] = new[] { Button(LeftShoulder) };
            t[4] = new[] { Button(Start) };
            t[5] = new[] { Button(Back) };
            t[6] = new[] { Button(Home) };
            t[7] = new[] { Button(DpadUp), Pos(RightY) };
            t[8] = new[] { Button(DpadDown), Neg(RightY) };
            t[9] = new[] { Button(DpadLeft), Neg(RightX) };
            t[10] = new[] { Button(DpadRight), Pos(RightX) };
            t[11] = new[] { Pos(LeftY) }; t[12] = new[] { Neg(LeftY) }; t[13] = new[] { Neg(LeftX) }; t[14] = new[] { Pos(LeftX) };
            t[15] = new[] { Button(LeftStick) };
            t[16] = new[] { Pos(LeftTrigger) };
            t[17] = new[] { Button(West), Button(North), Pos(RightTrigger) };
            t[19] = new[] { Pos(RightY) }; t[20] = new[] { Neg(RightY) }; t[21] = new[] { Neg(RightX) }; t[22] = new[] { Pos(RightX) };
            t[23] = new[] { Button(RightShoulder) };
            return t;
        }

        public static SchemeTuning LegacyTuning(int scheme)
        {
            var t = new SchemeTuning();
            if (scheme == Xbox)
            {
                t.StickDeadzone = 7849.0f / 32767.0f;
                t.PointerDeadzone = 8689.0f / 32767.0f;
                t.PointerSpeed = 2.4f;
                t.PointerHideSeconds = 4.0f;
            }
            else if (HasMotion(scheme))
            {
                t.StickDeadzone = 0.12f;
                t.PointerDeadzone = 0.12f;
                t.PointerSpeed = 2.4f;
                t.Motion = 1;
                t.PointerHideSeconds = 4.0f;
            }
            return t;
        }

        public static SchemeTuning DefaultTuning(int scheme)
        {
            var t = LegacyTuning(scheme);
            if (HasMotion(scheme)) t.GyroPointer = true;
            if (scheme != Keyboard)
            {
                t.PointerHideSeconds = 4.0f;
                t.PointerHideRadius = 0.12f;
            }
            return t;
        }

        private static bool SameBindings(InputSource[] a, InputSource[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (!a[i].Equals(b[i])) return false;
            return true;
        }

        /// <summary>Moves bindings and tuning that still equal the legacy defaults to the current defaults (runtime migrate_legacy_controls).</summary>
        private void MigrateLegacy(int version)
        {
            for (int s = 0; s < SchemeCount; s++)
            {
                var legacy = LegacyTable(s);
                var version2 = Version2Table(s);
                var version3 = Version3Table(s);
                var current = DefaultTable(s);
                for (int t = 0; t < TargetCount; t++)
                {
                    bool old = SameBindings(Bindings[s][t], legacy[t]) || SameBindings(Bindings[s][t], version2[t]) ||
                               SameBindings(Bindings[s][t], version3[t]);
                    if (old && !SameBindings(Bindings[s][t], current[t])) Bindings[s][t] = current[t];
                }
                if (version < 2 && Tuning[s].GyroPointer == LegacyTuning(s).GyroPointer) Tuning[s].GyroPointer = DefaultTuning(s).GyroPointer;
                if (version < ControlsVersion && s != Keyboard)
                {
                    var defaults = DefaultTuning(s);
                    bool oldSeconds = (version < 4 && Tuning[s].PointerHideSeconds == Version3PointerHideSeconds) ||
                                      Tuning[s].PointerHideSeconds == Version4PointerHideSeconds;
                    if (oldSeconds) Tuning[s].PointerHideSeconds = defaults.PointerHideSeconds;
                    if (version < 4 && Tuning[s].PointerHideRadius == Version3PointerHideRadius) Tuning[s].PointerHideRadius = defaults.PointerHideRadius;
                    if (Tuning[s].Rumble == Version4Rumble) Tuning[s].Rumble = defaults.Rumble;
                }
            }
            if (version < 2 && InputMode == ModeKeyboardMouse) InputMode = ModeAuto;
        }

        // ---- names ---------------------------------------------------------------
        public static string FormatSource(int scheme, InputSource source)
        {
            switch (source.Kind)
            {
                case SourceKind.Key: return FormatKey(source.Code);
                case SourceKind.Mouse: return source.Code < MouseNames.Length ? MouseNames[source.Code] : "None";
                case SourceKind.Button:
                    return source.Code < ButtonCount ? ButtonNames(scheme)[source.Code] : "None";
                case SourceKind.AxisPositive:
                case SourceKind.AxisNegative:
                    if (source.Code == LeftTrigger) return TriggerName(scheme, true);
                    if (source.Code == RightTrigger) return TriggerName(scheme, false);
                    if (source.Code >= StickAxes.Length) return "None";
                    return StickAxes[source.Code] + (source.Kind == SourceKind.AxisPositive ? "+" : "-");
            }
            return "None";
        }

        private static string FormatKey(int vk)
        {
            if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return ((char)vk).ToString();
            if (vk >= 0x60 && vk <= 0x69) return "Num" + (vk - 0x60);
            if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x70 + 1);
            foreach (var key in NamedKeys) if (key.Key == vk) return key.Value;
            return "VK" + vk.ToString("X2", CultureInfo.InvariantCulture);
        }

        public static bool TryParseSource(int scheme, string text, out InputSource source)
        {
            source = new InputSource();
            text = text.Trim();
            if (text.Length == 0) return false;
            var ic = StringComparison.OrdinalIgnoreCase;
            if (scheme == Keyboard)
            {
                for (int i = 0; i < MouseNames.Length; i++)
                    if (string.Equals(text, MouseNames[i], ic)) { source = Mouse(i); return true; }
                if (text.Length == 1)
                {
                    char c = char.ToUpperInvariant(text[0]);
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) { source = Key(c); return true; }
                }
                foreach (var key in NamedKeys) if (string.Equals(text, key.Value, ic)) { source = Key(key.Key); return true; }
                int n;
                if (text.Length == 4 && text.StartsWith("Num", ic) && int.TryParse(text.Substring(3), out n)) { source = Key(0x60 + n); return true; }
                if (text.Length >= 2 && text.Length <= 3 && (text[0] == 'F' || text[0] == 'f') &&
                    int.TryParse(text.Substring(1), NumberStyles.None, CultureInfo.InvariantCulture, out n) && n >= 1 && n <= 24)
                { source = Key(0x70 + n - 1); return true; }
                if (text.Length == 4 && text.StartsWith("VK", ic) &&
                    int.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out n) && n > 0)
                { source = Key(n); return true; }
                return false;
            }
            var names = ButtonNames(scheme);
            for (int i = 0; i < names.Length; i++) if (string.Equals(text, names[i], ic)) { source = Button(i); return true; }
            if (scheme == PlayStation && string.Equals(text, "Create", ic)) { source = Button(Back); return true; }
            if (string.Equals(text, TriggerName(scheme, true), ic)) { source = Pos(LeftTrigger); return true; }
            if (string.Equals(text, TriggerName(scheme, false), ic)) { source = Pos(RightTrigger); return true; }
            if (text.Length == 3 && (text[2] == '+' || text[2] == '-'))
                for (int i = 0; i < StickAxes.Length; i++)
                    if (string.Equals(text.Substring(0, 2), StickAxes[i], ic)) { source = text[2] == '+' ? Pos(i) : Neg(i); return true; }
            return false;
        }

        public static string FormatBindings(int scheme, InputSource[] bindings)
        {
            var parts = new List<string>();
            foreach (var b in bindings) if (!b.IsEmpty) parts.Add(FormatSource(scheme, b));
            return parts.Count == 0 ? "None" : string.Join(", ", parts);
        }

        public static bool Allowed(int scheme, int target, InputSource s)
        {
            switch (s.Kind)
            {
                case SourceKind.Key: return scheme == Keyboard && s.Code > 0 && s.Code < 256;
                case SourceKind.Mouse: return scheme == Keyboard && s.Code < MouseNames.Length && !IsAnalog(target);
                case SourceKind.Button:
                    return s.Code >= 0 && s.Code < ButtonCount && (ButtonMask(scheme) & (1 << s.Code)) != 0;
                case SourceKind.AxisPositive: return scheme != Keyboard && s.Code <= RightTrigger;
                case SourceKind.AxisNegative: return scheme != Keyboard && s.Code <= RightY;
            }
            return false;
        }

        public static InputSource[] AddBinding(InputSource[] bindings, InputSource source)
        {
            var list = new List<InputSource>(bindings);
            if (list.Contains(source)) return bindings;
            if (list.Count >= Slots) list.RemoveAt(list.Count - 1);
            list.Add(source);
            return list.ToArray();
        }

        // ---- ini -------------------------------------------------------------------
        public static ControlsSettings Load()
        {
            var settings = new ControlsSettings();
            string[] lines;
            try { lines = File.Exists(SettingsPath) ? File.ReadAllLines(SettingsPath) : new string[0]; }
            catch (IOException) { return settings; }
            catch (UnauthorizedAccessException) { return settings; }
            int version = 0;
            foreach (string line in lines)
            {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
                if (key == "controls_version") { int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out version); continue; }
                settings.Apply(key, value);
            }
            if (lines.Length != 0 && version < ControlsVersion) settings.MigrateLegacy(version);
            return settings;
        }

        private void Apply(string key, string value)
        {
            if (key == "input_mode")
            {
                string v = value.ToLowerInvariant();
                if (v == "auto") InputMode = ModeAuto;
                else if (v == "controller" || v == "xinput" || v == "gamepad" || v == "switch" || v == "switch_pro") InputMode = ModeController;
                else if (v.Contains("wiimote") || v.Contains("wii_remote") || v.Contains("wii-remote")) InputMode = ModeRealWiimote;
                else if (v.StartsWith("keyboard") || v == "kbm" || v == "mouse") InputMode = ModeKeyboardMouse;
                return;
            }
            bool bind = key.StartsWith("bind."), tune = key.StartsWith("tune.");
            if (!bind && !tune) return;
            string rest = key.Substring(5);
            int dot = rest.IndexOf('.');
            if (dot < 0) return;
            int scheme = Array.IndexOf(SchemeIds, rest.Substring(0, dot));
            string name = rest.Substring(dot + 1);
            if (scheme < 0) return;
            if (bind)
            {
                int target = Array.IndexOf(TargetIds, name);
                if (target < 0) return;
                var list = new InputSource[0];
                if (!string.Equals(value, "None", StringComparison.OrdinalIgnoreCase) && value.Length != 0)
                {
                    foreach (string token in value.Split(','))
                    {
                        if (token.Trim().Length == 0) continue;
                        InputSource source;
                        if (!TryParseSource(scheme, token, out source)) return;
                        if (Allowed(scheme, target, source)) list = AddBinding(list, source);
                    }
                }
                Bindings[scheme][target] = list;
                return;
            }
            var t = Tuning[scheme];
            float f;
            bool parsed = float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out f) && !float.IsNaN(f) && !float.IsInfinity(f);
            switch (name)
            {
                case "stick_deadzone": if (parsed) t.StickDeadzone = Math.Max(0f, Math.Min(0.9f, f)); break;
                case "pointer_deadzone": if (parsed) t.PointerDeadzone = Math.Max(0f, Math.Min(0.9f, f)); break;
                case "pointer_speed": if (parsed) t.PointerSpeed = Math.Max(0.1f, Math.Min(10f, f)); break;
                case "invert_pointer_y": t.InvertPointerY = value == "1" || value.ToLowerInvariant() == "true"; break;
                case "gyro_pointer": t.GyroPointer = value == "1" || value.ToLowerInvariant() == "true"; break;
                case "gyro_sensitivity": if (parsed) t.GyroSensitivity = Math.Max(0.2f, Math.Min(5f, f)); break;
                case "pointer_hide_seconds": if (parsed) t.PointerHideSeconds = Math.Max(0f, Math.Min(60f, f)); break;
                case "pointer_hide_radius": if (parsed) t.PointerHideRadius = Math.Max(0f, Math.Min(0.5f, f)); break;
                case "shake_sensitivity": if (parsed) t.ShakeSensitivity = Math.Max(1, Math.Min(5, (int)Math.Round(f))); break;
                case "motion": { int m = Array.IndexOf(MotionNames, value.ToLowerInvariant()); if (m >= 0) t.Motion = m; break; }
                case "vibration":
                    {
                        string v = value.Trim().ToLowerInvariant();
                        if (v == "1" || v == "true" || v == "on") t.Vibration = true;
                        else if (v == "0" || v == "false" || v == "off") t.Vibration = false;
                        break;
                    }
                // 0 was "off" before controls version 5.
                case "rumble": if (parsed) { if (f <= 0f) t.Vibration = false; else t.Rumble = SnapRumble(f); } break;
            }
        }

        private static string F(float value) { return value.ToString("R", CultureInfo.InvariantCulture); }

        public List<KeyValuePair<string, string>> Lines()
        {
            var lines = new List<KeyValuePair<string, string>>();
            if (InputMode != ModeRealWiimote)
                lines.Add(new KeyValuePair<string, string>("input_mode",
                    InputMode == ModeController ? "controller" : InputMode == ModeKeyboardMouse ? "keyboard_mouse" : "auto"));
            lines.Add(new KeyValuePair<string, string>("controls_version", ControlsVersion.ToString(CultureInfo.InvariantCulture)));
            for (int s = 0; s < SchemeCount; s++)
            {
                string p = SchemeIds[s] + ".";
                for (int t = 0; t < TargetCount; t++)
                    lines.Add(new KeyValuePair<string, string>("bind." + p + TargetIds[t], FormatBindings(s, Bindings[s][t])));
                var tuning = Tuning[s];
                if (s != Keyboard)
                {
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "stick_deadzone", F(tuning.StickDeadzone)));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "pointer_deadzone", F(tuning.PointerDeadzone)));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "pointer_hide_seconds", F(tuning.PointerHideSeconds)));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "pointer_hide_radius", F(tuning.PointerHideRadius)));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "vibration", tuning.Vibration ? "1" : "0"));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "rumble", SnapRumble(tuning.Rumble).ToString(CultureInfo.InvariantCulture)));
                }
                lines.Add(new KeyValuePair<string, string>("tune." + p + "pointer_speed", F(tuning.PointerSpeed)));
                lines.Add(new KeyValuePair<string, string>("tune." + p + "invert_pointer_y", tuning.InvertPointerY ? "1" : "0"));
                if (HasMotion(s))
                {
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "motion", MotionNames[tuning.Motion]));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "shake_sensitivity", tuning.ShakeSensitivity.ToString(CultureInfo.InvariantCulture)));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "gyro_pointer", tuning.GyroPointer ? "1" : "0"));
                    lines.Add(new KeyValuePair<string, string>("tune." + p + "gyro_sensitivity", F(tuning.GyroSensitivity)));
                }
            }
            return lines;
        }

        /// <summary>Replaces this model's keys and keeps every other line.</summary>
        public void Save()
        {
            var lines = File.Exists(SettingsPath) ? new List<string>(File.ReadAllLines(SettingsPath)) : new List<string>();
            foreach (var pair in Lines())
            {
                string line = pair.Key + "=" + pair.Value;
                bool found = false;
                for (int i = 0; i < lines.Count && !found; i++)
                {
                    int eq = lines[i].IndexOf('=');
                    if (eq > 0 && lines[i].Substring(0, eq).Trim() == pair.Key) { lines[i] = line; found = true; }
                }
                if (!found) lines.Add(line);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));
            string temp = SettingsPath + ".tmp";
            File.WriteAllText(temp, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
            if (File.Exists(SettingsPath)) File.Replace(temp, SettingsPath, null);
            else File.Move(temp, SettingsPath);
        }
    }
}
