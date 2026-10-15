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
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, IntPtr enumerator, IntPtr parent, int flags);
        [DllImport("setupapi.dll")] private static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr info, ref Guid guid, int index, ref SpDeviceInterfaceData data);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] private static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SpDeviceInterfaceData data, IntPtr detail, int size, out int required, IntPtr info);
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

        /// <summary>HID devices of the vendors Nebula reads (Nintendo, Sony, Valve), attributes read without opening for I/O.</summary>
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
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (!SetupDiGetDeviceInterfaceDetailW(set, ref data, detail, required, out required, IntPtr.Zero)) continue;
                        path = Marshal.PtrToStringUni(new IntPtr(detail.ToInt64() + 4));
                    }
                    finally { Marshal.FreeHGlobal(detail); }
                    if (path == null || (path.IndexOf("057e", StringComparison.OrdinalIgnoreCase) < 0 &&
                        path.IndexOf("054c", StringComparison.OrdinalIgnoreCase) < 0 &&
                        path.IndexOf("28de", StringComparison.OrdinalIgnoreCase) < 0)) continue;
                    using (var probe = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
                    {
                        if (probe.IsInvalid) continue;
                        var attributes = new HiddAttributes { Size = Marshal.SizeOf(typeof(HiddAttributes)) };
                        if (!HidD_GetAttributes(probe, ref attributes)) continue;
                        found.Add(new HidInfo { Path = path, Vendor = attributes.Vendor, Product = attributes.Product, Interface = InterfaceNumber(path) });
                    }
                }
            }
            finally { SetupDiDestroyDeviceInfoList(set); }
            return found;
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
    }
}
