using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace Nebula.Launcher
{
    /// <summary>
    /// Controller input for the Controls tab: XInput polling, and HID reports read on a thread per
    /// device. It tells which controller the player is using and supplies the pad state for binding
    /// capture. Reading is passive; Prepare sends what a controller needs before it streams (the
    /// Switch Pro Controller's full reports, the Steam Controller's settings). The Pro Controller 2
    /// only reports once something has sent it the vendor init command, e.g. the game.
    /// </summary>
    internal sealed class ControllerReader : IDisposable
    {
        private sealed class HidPad
        {
            public ControllerInfo Info;
            public SafeFileHandle Handle;
            public int InputLength, OutputLength;
            public bool Bluetooth;
            public readonly PadSample State = new PadSample();
            // Generic controllers: the HID parser's data, the value layout and the last values.
            public IntPtr Preparsed;
            public readonly GenericLayout Layout = new GenericLayout();
            public readonly GenericRaw Raw = new GenericRaw();
            public readonly ushort[] Usages = new ushort[GenericHid.MaxButtons];
        }

        private readonly List<HidPad> pads = new List<HidPad>();
        private readonly float[] axes = new float[6];
        private volatile bool stopping;
        private long active = -1;
        private int[] xinputSlots = new int[0];

        public ControllerReader(IEnumerable<ControllerInfo> controllers)
        {
            foreach (var controller in controllers)
                if (controller.Path != null) TryOpen(controller);
            SetXInput(controllers);
        }

        /// <summary>The XInput pads to watch; empty slots are slow to poll.</summary>
        public void SetXInput(IEnumerable<ControllerInfo> controllers)
        {
            xinputSlots = controllers.Where(c => c.Scheme == ControlsSettings.Xbox).Select(c => c.Slot).ToArray();
        }

        private static long KeyOf(DeviceKey key) { return ((long)key.Scheme << 8) | (uint)key.Slot; }

        /// <summary>The controller that most recently sent input.</summary>
        public DeviceKey? Active
        {
            get
            {
                long value = Interlocked.Read(ref active);
                return value < 0 ? (DeviceKey?)null : new DeviceKey((int)(value >> 8), (int)(value & 0xFF));
            }
        }

        /// <summary>Notes XInput pads that are being used.</summary>
        public void PollXInput()
        {
            foreach (int slot in xinputSlots)
            {
                var sample = ReadXInput(slot);
                if (sample.Connected && sample.HasInput) Interlocked.Exchange(ref active, KeyOf(new DeviceKey(ControlsSettings.Xbox, slot)));
            }
        }

        private PadSample ReadXInput(int slot)
        {
            var sample = new PadSample();
            int buttons;
            if (!HidDevices.XInputRead(slot, out buttons, axes)) return sample;
            sample.Connected = true;
            sample.Buttons = buttons;
            Array.Copy(axes, sample.Axes, 6);
            return sample;
        }

        public PadSample Read(DeviceKey key)
        {
            if (key.Scheme == ControlsSettings.Xbox) return ReadXInput(key.Slot);
            var sample = new PadSample();
            lock (pads)
            {
                foreach (var pad in pads)
                {
                    if (pad.Info.Scheme != key.Scheme || pad.Info.Slot != key.Slot || !pad.State.Connected) continue;
                    sample.Connected = true;
                    sample.Buttons = pad.State.Buttons;
                    Array.Copy(pad.State.Axes, sample.Axes, 6);
                    break;
                }
            }
            return sample;
        }

        /// <summary>Starts a controller streaming, before binding capture reads it.</summary>
        public void Prepare(DeviceKey key)
        {
            HidPad target = null;
            lock (pads)
            {
                foreach (var pad in pads) if (pad.Info.Scheme == key.Scheme && pad.Info.Slot == key.Slot) target = pad;
            }
            if (target == null) return;
            if (target.Info.Scheme == ControlsSettings.SwitchPro)
            {
                if (!target.Bluetooth) { Write(target, new byte[] { 0x80, 0x02 }); Write(target, new byte[] { 0x80, 0x04 }); }
                // Subcommand 0x03: full report mode 0x30 (neutral rumble).
                Write(target, new byte[] { 0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x03, 0x30 });
            }
            else if (target.Info.Scheme == ControlsSettings.Steam)
            {
                HidDevices.SetFeature(target.Handle, PadReports.SteamSettingsReport());
            }
        }

        private void TryOpen(ControllerInfo info)
        {
            var handle = HidDevices.Open(info.Path);
            if (handle == null) return;
            var pad = new HidPad { Info = info, Handle = handle, Bluetooth = info.Link == ControllerLink.Bluetooth };
            HidDevices.ReportLengths(handle, out pad.InputLength, out pad.OutputLength);
            if (info.Scheme == ControlsSettings.Generic) pad.Preparsed = HidDevices.OpenGeneric(handle, pad.Layout);
            lock (pads) pads.Add(pad);
            new Thread(delegate() { ReadLoop(pad); }) { IsBackground = true }.Start();
        }

        private static void Write(HidPad pad, byte[] data)
        {
            var buffer = new byte[pad.OutputLength];
            Array.Copy(data, buffer, Math.Min(data.Length, buffer.Length));
            int written;
            HidDevices.WriteFile(pad.Handle, buffer, buffer.Length, out written, IntPtr.Zero);
            Thread.Sleep(20);
        }

        private bool Decode(HidPad pad, byte[] buffer, int size, PadSample sample)
        {
            switch (pad.Info.Scheme)
            {
                case ControlsSettings.PlayStation: return PadReports.DecodePlayStation(pad.Info.PlayStationModel, pad.Bluetooth, buffer, size, sample);
                case ControlsSettings.Steam: return PadReports.DecodeSteam(buffer, size, sample);
                case ControlsSettings.Generic:
                    if (!HidDevices.ReadGeneric(pad.Preparsed, pad.Layout, buffer, size, pad.Raw, pad.Usages)) return false;
                    GenericHid.Map(pad.Layout, pad.Raw, sample);
                    return true;
                default: return PadReports.DecodeSwitch(buffer, size, sample);
            }
        }

        private void ReadLoop(HidPad pad)
        {
            var buffer = new byte[pad.InputLength];
            while (!stopping)
            {
                int read;
                if (!HidDevices.ReadFile(pad.Handle, buffer, buffer.Length, out read, IntPtr.Zero)) break;
                var sample = new PadSample();
                if (!Decode(pad, buffer, read, sample)) continue;
                lock (pads)
                {
                    pad.State.Connected = true;
                    pad.State.Buttons = sample.Buttons;
                    Array.Copy(sample.Axes, pad.State.Axes, 6);
                }
                if (sample.HasInput) Interlocked.Exchange(ref active, KeyOf(new DeviceKey(pad.Info.Scheme, pad.Info.Slot)));
            }
            // The read thread owns the parser data: Decode never runs after this.
            HidDevices.FreeGeneric(pad.Preparsed);
            pad.Preparsed = IntPtr.Zero;
        }

        public void Dispose()
        {
            stopping = true;
            lock (pads)
            {
                foreach (var pad in pads)
                {
                    HidDevices.CancelIoEx(pad.Handle, IntPtr.Zero);
                    pad.Handle.Dispose();
                }
                pads.Clear();
            }
        }
    }
}
