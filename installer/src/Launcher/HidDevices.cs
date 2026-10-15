using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Nebula.Launcher
{
    internal sealed class HidInfo
    {
        public string Path;
        public int Vendor, Product, Interface;
        // Top-level collection; ProductName only for game controller collections.
        public int UsagePage, Usage;
        public string ProductName;
    }

    /// <summary>Win32 HID enumeration and access for the controller list and binding capture.</summary>
    internal static class HidDevices
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct XInputState
        {
            public uint Packet;
            public ushort Buttons;
            public byte LeftTrigger, RightTrigger;
            public short LX, LY, RX, RY;
        }
        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] private static extern int XInputGetState(int index, out XInputState state);

        [StructLayout(LayoutKind.Sequential)]
        private struct SpDeviceInterfaceData { public int Size; public Guid Class; public int Flags; public IntPtr Reserved; }
        [StructLayout(LayoutKind.Sequential)]
        private struct SpDevinfoData { public int Size; public Guid Class; public int DevInst; public IntPtr Reserved; }
        [StructLayout(LayoutKind.Sequential)]
        private struct HiddAttributes { public int Size; public ushort Vendor, Product, Version; }
        [StructLayout(LayoutKind.Sequential)]
        private struct HidpCaps
        {
            public ushort Usage, UsagePage, InputLength, OutputLength, FeatureLength;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
            public ushort A, B, C, D, E, F, G, H, I, J;
        }
        [DllImport("hid.dll")] private static extern void HidD_GetHidGuid(out Guid guid);
        [DllImport("hid.dll")] private static extern bool HidD_GetAttributes(SafeFileHandle device, ref HiddAttributes attributes);
        [DllImport("hid.dll")] private static extern bool HidD_GetPreparsedData(SafeFileHandle device, out IntPtr data);
        [DllImport("hid.dll")] private static extern bool HidD_FreePreparsedData(IntPtr data);
        [DllImport("hid.dll")] private static extern int HidP_GetCaps(IntPtr data, ref HidpCaps caps);
        [DllImport("hid.dll")] private static extern bool HidD_SetFeature(SafeFileHandle device, byte[] report, int length);
        [DllImport("hid.dll")] private static extern bool HidD_GetProductString(SafeFileHandle device, byte[] buffer, int length);
        [DllImport("hid.dll")] private static extern int HidP_GetValueCaps(int reportType, byte[] caps, ref ushort length, IntPtr data);
        [DllImport("hid.dll")] private static extern int HidP_GetButtonCaps(int reportType, byte[] caps, ref ushort length, IntPtr data);
        [DllImport("hid.dll")] private static extern int HidP_GetUsages(int reportType, ushort page, ushort collection, ushort[] usages, ref uint length, IntPtr data, byte[] report, uint reportLength);
        [DllImport("hid.dll")] private static extern int HidP_GetUsageValue(int reportType, ushort page, ushort collection, ushort usage, out uint value, IntPtr data, byte[] report, uint reportLength);
        private const int HidpInput = 0, HidpStatusSuccess = 0x00110000, HidpStatusBufferTooSmall = unchecked((int)0xC0110007);
        // HIDP_VALUE_CAPS and HIDP_BUTTON_CAPS are 72 bytes; fields are read at their offsets.
        private const int CapsBytes = 72;
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, IntPtr enumerator, IntPtr parent, int flags);
        [DllImport("setupapi.dll")] private static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr info, ref Guid guid, int index, ref SpDeviceInterfaceData data);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SpDeviceInterfaceData data, IntPtr detail, int size, out int required, IntPtr info);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SpDeviceInterfaceData data, IntPtr detail, int size, out int required, ref SpDevinfoData info);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref SpDevinfoData info, int property, IntPtr type, byte[] buffer, int size, IntPtr required);
        [DllImport("setupapi.dll")] private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool ReadFile(SafeFileHandle file, byte[] buffer, int size, out int read, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(SafeFileHandle file, byte[] buffer, int size, out int written, IntPtr overlapped);
        [DllImport("kernel32.dll")] public static extern bool CancelIoEx(SafeFileHandle file, IntPtr overlapped);

        /// <summary>One XInput pad: buttons in the XInput word and the six axes (sticks up positive, triggers 0..1).</summary>
        public static bool XInputRead(int slot, out int buttons, float[] axes)
        {
            buttons = 0;
            XInputState state;
            try { if (XInputGetState(slot, out state) != 0) return false; }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
            buttons = state.Buttons;
            axes[0] = Normalize(state.LX); axes[1] = Normalize(state.LY);
            axes[2] = Normalize(state.RX); axes[3] = Normalize(state.RY);
            axes[4] = state.LeftTrigger / 255f; axes[5] = state.RightTrigger / 255f;
            return true;
        }

        public static bool XInputConnected(int slot)
        {
            int buttons;
            return XInputRead(slot, out buttons, new float[6]);
        }

        private static float Normalize(short value) { return Math.Max(-1f, value / 32767f); }

        public static bool IsBluetoothPath(string path)
        {
            if (path == null) return false;
            return path.IndexOf("{00001124-0000-1000-8000-00805f9b34fb}", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   path.IndexOf("{00001812-0000-1000-8000-00805f9b34fb}", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>USB interface number from "&amp;mi_NN" in a device path (-1: not a composite device).</summary>
        public static int InterfaceNumber(string path)
        {
            int at = path.IndexOf("&mi_", StringComparison.OrdinalIgnoreCase);
            if (at < 0 || at + 6 > path.Length) return -1;
            int value;
            return int.TryParse(path.Substring(at + 4, 2), System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture, out value) ? value : -1;
        }

        /// <summary>HID devices with their attributes and top-level collection, read without opening for I/O.</summary>
        public static List<HidInfo> Enumerate()
        {
            var found = new List<HidInfo>();
            Guid hid;
            HidD_GetHidGuid(out hid);
            IntPtr set = SetupDiGetClassDevsW(ref hid, IntPtr.Zero, IntPtr.Zero, 0x12 /* PRESENT | DEVICEINTERFACE */);
            if (set == new IntPtr(-1)) return found;
            try
            {
                var data = new SpDeviceInterfaceData { Size = Marshal.SizeOf(typeof(SpDeviceInterfaceData)) };
                for (int index = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hid, index, ref data); index++)
                {
                    int required;
                    SetupDiGetDeviceInterfaceDetailW(set, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                    if (required <= 0) continue;
                    IntPtr detail = Marshal.AllocHGlobal(required);
                    string path;
                    var devinfo = new SpDevinfoData { Size = Marshal.SizeOf(typeof(SpDevinfoData)) };
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (!SetupDiGetDeviceInterfaceDetailW(set, ref data, detail, required, out required, ref devinfo)) continue;
                        path = Marshal.PtrToStringUni(new IntPtr(detail.ToInt64() + 4));
                    }
                    finally { Marshal.FreeHGlobal(detail); }
                    if (path == null) continue;
                    // Only the vendors with their own decoders and game collections are opened (runtime enumerate_hid_pads).
                    bool knownVendor = path.IndexOf("057e", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                       path.IndexOf("054c", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                       path.IndexOf("28de", StringComparison.OrdinalIgnoreCase) >= 0;
                    if (!knownVendor && !GameCollection(set, ref devinfo)) continue;
                    using (var probe = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
                    {
                        if (probe.IsInvalid) continue;
                        var attributes = new HiddAttributes { Size = Marshal.SizeOf(typeof(HiddAttributes)) };
                        if (!HidD_GetAttributes(probe, ref attributes)) continue;
                        var info = new HidInfo { Path = path, Vendor = attributes.Vendor, Product = attributes.Product, Interface = InterfaceNumber(path) };
                        IntPtr preparsed;
                        if (HidD_GetPreparsedData(probe, out preparsed))
                        {
                            var caps = new HidpCaps { Reserved = new ushort[17] };
                            if (HidP_GetCaps(preparsed, ref caps) == HidpStatusSuccess) { info.UsagePage = caps.UsagePage; info.Usage = caps.Usage; }
                            HidD_FreePreparsedData(preparsed);
                        }
                        if (GenericHid.IsGameController(info.UsagePage, info.Usage)) info.ProductName = ProductName(probe);
                        found.Add(info);
                    }
                }
            }
            finally { SetupDiDestroyDeviceInfoList(set); }
            return found;
        }

        /// <summary>The compatible ids name a joystick or game pad collection ("HID_DEVICE_SYSTEM_GAME", "HID_DEVICE_UP:0001_U:0005").</summary>
        private static bool GameCollection(IntPtr set, ref SpDevinfoData info)
        {
            var buffer = new byte[2048];
            if (!SetupDiGetDeviceRegistryPropertyW(set, ref info, 2 /* SPDRP_COMPATIBLEIDS */, IntPtr.Zero, buffer, buffer.Length - 4, IntPtr.Zero)) return false;
            foreach (string id in System.Text.Encoding.Unicode.GetString(buffer).Split('\0'))
            {
                if (id.IndexOf("HID_DEVICE_SYSTEM_GAME", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    id.IndexOf("HID_DEVICE_UP:0001_U:0004", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    id.IndexOf("HID_DEVICE_UP:0001_U:0005", StringComparison.OrdinalIgnoreCase) >= 0) return true;
            }
            return false;
        }

        /// <summary>The device opened for synchronous reads and writes; null if it cannot be opened.</summary>
        public static SafeFileHandle Open(string path)
        {
            var handle = CreateFileW(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (handle.IsInvalid) { handle.Dispose(); return null; }
            return handle;
        }

        public static void ReportLengths(SafeFileHandle handle, out int input, out int output)
        {
            input = 64;
            output = 64;
            IntPtr preparsed;
            if (!HidD_GetPreparsedData(handle, out preparsed)) return;
            var caps = new HidpCaps { Reserved = new ushort[17] };
            HidP_GetCaps(preparsed, ref caps);
            HidD_FreePreparsedData(preparsed);
            input = Math.Max(64, (int)caps.InputLength);
            output = Math.Max(2, (int)caps.OutputLength);
        }

        public static bool SetFeature(SafeFileHandle handle, byte[] report)
        {
            return HidD_SetFeature(handle, report, report.Length);
        }

        /// <summary>The HID product string in printable ASCII, spaces collapsed ("Game controller" when empty; runtime generic_model_name).</summary>
        private static string ProductName(SafeFileHandle handle)
        {
            var buffer = new byte[254];
            var name = new System.Text.StringBuilder();
            if (HidD_GetProductString(handle, buffer, buffer.Length))
            {
                foreach (char c in System.Text.Encoding.Unicode.GetString(buffer))
                {
                    if (c == '\0' || name.Length >= 39) break;
                    bool space = c == ' ' || c == '\t';
                    if (space && (name.Length == 0 || name[name.Length - 1] == ' ')) continue;
                    if (space || (c > 0x20 && c < 0x7f)) name.Append(space ? ' ' : c);
                }
            }
            string text = name.ToString().TrimEnd(' ');
            return text.Length != 0 ? text : "Game controller";
        }

        /// <summary>A generic controller's parser data (free with FreeGeneric) and the layout of its input values.</summary>
        public static IntPtr OpenGeneric(SafeFileHandle handle, GenericLayout layout)
        {
            IntPtr preparsed;
            if (!HidD_GetPreparsedData(handle, out preparsed)) return IntPtr.Zero;
            var caps = new HidpCaps { Reserved = new ushort[17] };
            if (HidP_GetCaps(preparsed, ref caps) != HidpStatusSuccess) return preparsed;
            ushort count = caps.C;  // NumberInputValueCaps
            var values = new byte[Math.Max(1, (int)count) * CapsBytes];
            if (count != 0 && HidP_GetValueCaps(HidpInput, values, ref count, preparsed) == HidpStatusSuccess)
            {
                for (int i = 0; i < count; i++)
                {
                    int at = i * CapsBytes;
                    int page = BitConverter.ToUInt16(values, at), bitSize = BitConverter.ToUInt16(values, at + 18);
                    bool isRange = values[at + 12] != 0;
                    int reportCount = BitConverter.ToUInt16(values, at + 20);
                    if (isRange || reportCount > 1) continue;
                    int usage = BitConverter.ToUInt16(values, at + 56);
                    var range = new GenericRange { Present = true, Min = BitConverter.ToInt32(values, at + 40), Max = BitConverter.ToInt32(values, at + 44), Bits = bitSize };
                    if (range.Max < range.Min && bitSize > 0 && bitSize < 32) { range.Min = 0; range.Max = (int)((1u << bitSize) - 1u); }
                    int axis = GenericHid.AxisOf(page, usage);
                    if (axis >= 0) layout.Axes[axis] = range;
                    else if (page == GenericHid.PageGenericDesktop && usage == GenericHid.UsageHat) layout.Hat = range;
                }
            }
            count = caps.B;  // NumberInputButtonCaps
            var buttons = new byte[Math.Max(1, (int)count) * CapsBytes];
            if (count != 0 && HidP_GetButtonCaps(HidpInput, buttons, ref count, preparsed) == HidpStatusSuccess)
            {
                for (int i = 0; i < count; i++)
                {
                    int at = i * CapsBytes;
                    if (BitConverter.ToUInt16(buttons, at) != GenericHid.PageButton) continue;
                    bool isRange = buttons[at + 12] != 0;
                    int last = BitConverter.ToUInt16(buttons, at + (isRange ? 58 : 56));
                    layout.Buttons = Math.Max(layout.Buttons, last);
                }
            }
            return preparsed;
        }

        public static void FreeGeneric(IntPtr preparsed)
        {
            if (preparsed != IntPtr.Zero) HidD_FreePreparsedData(preparsed);
        }

        /// <summary>One report through the HID parser: the values it carries replace those in `raw`. False when it carries none.</summary>
        public static bool ReadGeneric(IntPtr preparsed, GenericLayout layout, byte[] report, int size, GenericRaw raw, ushort[] usages)
        {
            if (preparsed == IntPtr.Zero || size <= 0) return false;
            bool any = false;
            uint held = (uint)usages.Length;
            int status = HidP_GetUsages(HidpInput, GenericHid.PageButton, 0, usages, ref held, preparsed, report, (uint)size);
            if (status == HidpStatusSuccess || status == HidpStatusBufferTooSmall)
            {
                uint mask = 0;
                for (int i = 0; i < Math.Min((int)held, usages.Length); i++)
                    if (usages[i] >= 1 && usages[i] <= GenericHid.MaxButtons) mask |= 1u << (usages[i] - 1);
                raw.Buttons = mask;
                any = true;
            }
            for (int axis = 0; axis < GenericHid.AxisCount; axis++)
            {
                if (!layout.Axes[axis].Present) continue;
                uint value;
                if (HidP_GetUsageValue(HidpInput, GenericHid.AxisPage(axis), 0, GenericHid.AxisUsage(axis), out value, preparsed, report, (uint)size) == HidpStatusSuccess)
                {
                    raw.Axes[axis] = value;
                    any = true;
                }
            }
            if (layout.Hat.Present)
            {
                uint value;
                if (HidP_GetUsageValue(HidpInput, GenericHid.PageGenericDesktop, 0, GenericHid.UsageHat, out value, preparsed, report, (uint)size) == HidpStatusSuccess)
                {
                    raw.Hat = value;
                    any = true;
                }
            }
            return any;
        }
    }
}
