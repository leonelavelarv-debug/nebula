using System;
using System.Collections.Generic;

namespace Nebula.Launcher
{
    internal enum ControllerLink { Unknown, Usb, Bluetooth, Wireless }

    /// <summary>
    /// One attached controller. Path is the HID device path (null for XInput); Slot is the XInput
    /// user index or the HID device's number in enumeration order.
    /// </summary>
    internal sealed class ControllerInfo
    {
        public int Scheme, Slot;
        public ControllerLink Link;
        public string Model, Path;
        public int Vendor, Product, PlayStationModel;
    }

    internal struct DeviceKey : IEquatable<DeviceKey>
    {
        public int Scheme, Slot;
        public DeviceKey(int scheme, int slot) { Scheme = scheme; Slot = slot; }
        public bool Equals(DeviceKey other) { return Scheme == other.Scheme && Slot == other.Slot; }
        public override bool Equals(object other) { return other is DeviceKey && Equals((DeviceKey)other); }
        public override int GetHashCode() { return (Scheme << 8) | (Slot & 0xFF); }
    }

    internal sealed class DeviceEntry
    {
        public DeviceKey Key;
        public bool Active;
        public string Label;
        /// <summary>The label as the drop-down shows it.</summary>
        public string Text { get { return Active ? Label + "  (active)" : Label; } }
    }

    /// <summary>
    /// The devices the Controls tab offers: keyboard and mouse plus every attached controller by
    /// model name (runtime controller_roster.h).
    /// </summary>
    internal static class DeviceList
    {
        public const int KeyboardSlot = 255, MaxControllers = 12;

        public static DeviceKey Keyboard { get { return new DeviceKey(ControlsSettings.Keyboard, KeyboardSlot); } }

        private static string LinkName(ControllerLink link)
        {
            switch (link)
            {
                case ControllerLink.Usb: return "USB";
                case ControllerLink.Bluetooth: return "Bluetooth";
                case ControllerLink.Wireless: return "wireless";
            }
            return null;
        }

        private static string Label(string model, int ordinal, string link)
        {
            string label = model;
            if (ordinal != 0) label += " " + ordinal;
            if (link != null) label += " (" + link + ")";
            return label;
        }

        /// <summary>
        /// Keyboard and mouse first, then the controllers by scheme and slot (more than MaxControllers
        /// are dropped). Controllers of one model are numbered; the link is named.
        /// </summary>
        public static List<DeviceEntry> Build(IList<ControllerInfo> connected, DeviceKey? active)
        {
            var list = new List<DeviceEntry>();
            list.Add(new DeviceEntry { Key = Keyboard, Label = "Keyboard and mouse", Active = active.HasValue && active.Value.Equals(Keyboard) });
            var sorted = new List<ControllerInfo>();
            foreach (var info in connected) if (info.Scheme != ControlsSettings.Keyboard) sorted.Add(info);
            sorted.Sort(delegate(ControllerInfo a, ControllerInfo b)
            {
                return a.Scheme != b.Scheme ? a.Scheme.CompareTo(b.Scheme) : a.Slot.CompareTo(b.Slot);
            });
            if (sorted.Count > MaxControllers) sorted.RemoveRange(MaxControllers, sorted.Count - MaxControllers);
            for (int i = 0; i < sorted.Count; i++)
            {
                int sameModel = 0, ordinal = 0;
                for (int j = 0; j < sorted.Count; j++)
                {
                    if (sorted[j].Model != sorted[i].Model) continue;
                    sameModel++;
                    if (j <= i) ordinal = sameModel;
                }
                var key = new DeviceKey(sorted[i].Scheme, sorted[i].Slot);
                list.Add(new DeviceEntry
                {
                    Key = key,
                    Label = Label(sorted[i].Model, sameModel > 1 ? ordinal : 0, LinkName(sorted[i].Link)),
                    Active = active.HasValue && active.Value.Equals(key),
                });
            }
            return list;
        }

        /// <summary>
        /// The entry to edit after the list changed: the previous one if it is still there, else another
        /// controller of its scheme (same bindings), else the keyboard.
        /// </summary>
        public static int Resolve(IList<DeviceEntry> list, DeviceKey previous)
        {
            for (int i = 0; i < list.Count; i++) if (list[i].Key.Equals(previous)) return i;
            for (int i = 0; i < list.Count; i++) if (list[i].Key.Scheme == previous.Scheme) return i;
            return 0;
        }

        /// <summary>The same entries in the same order and with the same labels (activity aside).</summary>
        public static bool SameEntries(IList<DeviceEntry> a, IList<DeviceEntry> b)
        {
            if (a.Count != b.Count) return false;
            for (int i = 0; i < a.Count; i++) if (!a[i].Key.Equals(b[i].Key) || a[i].Label != b[i].Label) return false;
            return true;
        }
    }

    /// <summary>The supported HID controllers by vendor and product id (runtime controller_hid.cpp).</summary>
    internal static class Products
    {
        public const int NintendoVendor = 0x057e, SonyVendor = 0x054c, ValveVendor = 0x28de;
        public const int DualShock4 = 0, DualSense = 1, DualSenseEdge = 2;

        public static bool IsPuck(int product) { return product == 0x1304 || product == 0x1305; }

        /// <summary>Scheme, display name and (PlayStation) model 0 DualShock 4, 1 DualSense, 2 DualSense Edge.</summary>
        public static bool Find(int vendor, int product, out int scheme, out string name, out int playStationModel)
        {
            scheme = ControlsSettings.Keyboard;
            name = null;
            playStationModel = 0;
            if (vendor == NintendoVendor)
            {
                if (product == 0x2009) { scheme = ControlsSettings.SwitchPro; name = "Switch Pro Controller"; return true; }
                if (product == 0x2069) { scheme = ControlsSettings.SwitchPro2; name = "Switch 2 Pro Controller"; return true; }
                return false;
            }
            if (vendor == SonyVendor)
            {
                scheme = ControlsSettings.PlayStation;
                if (product == 0x05c4 || product == 0x09cc) { name = "DualShock 4"; return true; }
                if (product == 0x0ce6) { name = "DualSense"; playStationModel = DualSense; return true; }
                if (product == 0x0df2) { name = "DualSense Edge"; playStationModel = DualSenseEdge; return true; }
                return false;
            }
            if (vendor == ValveVendor && (product == 0x1302 || product == 0x1303 || IsPuck(product)))
            {
                scheme = ControlsSettings.Steam;
                name = "Steam Controller";
                return true;
            }
            return false;
        }

        public static ControllerLink LinkOf(int scheme, int product, string path)
        {
            if (scheme == ControlsSettings.Steam)
            {
                if (IsPuck(product)) return ControllerLink.Wireless;
                return product == 0x1303 ? ControllerLink.Bluetooth : ControllerLink.Usb;
            }
            return HidDevices.IsBluetoothPath(path) ? ControllerLink.Bluetooth : ControllerLink.Usb;
        }
    }

    /// <summary>The controllers attached now: XInput pads, the supported HID controllers and generic HID game controllers.</summary>
    internal static class ControllerProbe
    {
        /// <summary>Stands in for the attached controllers (tests).</summary>
        public static Func<List<ControllerInfo>> Source = null;

        public static List<ControllerInfo> Enumerate()
        {
            if (Source != null) return Source();
            var list = new List<ControllerInfo>();
            for (int slot = 0; slot < 4; slot++)
            {
                if (HidDevices.XInputConnected(slot))
                    list.Add(new ControllerInfo { Scheme = ControlsSettings.Xbox, Slot = slot, Model = "Xbox controller" });
            }
            int hidSlot = 0;
            foreach (var device in HidDevices.Enumerate())
            {
                int scheme, model;
                string name;
                if (!Products.Find(device.Vendor, device.Product, out scheme, out name, out model))
                {
                    if (device.ProductName == null || GenericHid.ClaimedElsewhere(device.Vendor, device.Path)) continue;
                    scheme = ControlsSettings.Generic;
                    name = device.ProductName;
                }
                if (Products.IsPuck(device.Product) && (device.Interface < 2 || device.Interface > 5)) continue;
                list.Add(new ControllerInfo
                {
                    Scheme = scheme, Slot = hidSlot++, Model = name, Path = device.Path, Vendor = device.Vendor,
                    Product = device.Product, PlayStationModel = model,
                    Link = Products.LinkOf(scheme, device.Product, device.Path),
                });
            }
            return list;
        }
    }
}
