# yunzii-bridge

Userspace HID bridge for USB keyboards whose vendor interfaces break the
kernel's `usbhid` probing. Born for the wired mode of the
**ROYUAN YUNZII B75 PRO** (`3151:4015`), but reusable for any keyboard
with the same disease.

- keyboard stays fully usable in wired mode (typing, `Fn+Space`, boot and
  login screen included)
- no more 50–100 second stalls and disconnect loops at enumeration
- small C daemon (libusb + uhid), systemd unit, one kernel cmdline flag

---

## Table of contents

1. [The problem](#the-problem)
2. [Why it happens](#why-it-happens)
3. [The fix](#the-fix)
4. [Requirements](#requirements)
5. [Build and install](#build-and-install)
6. [Kernel cmdline quirk](#kernel-cmdline-quirk)
7. [Usage](#usage)
8. [Verify](#verify)
9. [RGB backlight](#rgb-backlight)
10. [Known limitations](#known-limitations)
11. [How it works (in detail)](#how-it-works-in-detail)
12. [Troubleshooting](#troubleshooting)
13. [Future work](#future-work)
14. [License](#license)

---

## The problem

The YUNZII B75 Pro exposes three HID interfaces in **wired** mode:

| Interface | Purpose | Report descriptor | Endpoint |
|---|---|---|---|
| 0 | keyboard (boot interface) | 59 bytes, **served** | `0x81` IN (8 B) |
| 1 | vendor (RGB) | 177 bytes, **STALLed** | `0x82` IN (16 B) |
| 2 | vendor (RGB) | 20 bytes, **STALLed** | `0x83` IN (8 B) |

The 2.4 GHz dongle of the same keyboard (`3151:4011`) exposes the *same*
three interfaces and serves **all** of them correctly. The wired firmware
is the broken part.

Observed symptoms on Linux (kernel 7.2.x, Fedora 44):

1. **Enumeration stalls.** At every plug `usbhid` probes interfaces 1 and 2
   and each probe hangs ~26 s:

   ```
   usbhid 5-1:1.1: can't add hid device: -110
   usbhid 5-1:1.2: can't add hid device: -110
   ```

   (`-110` = `ETIMEDOUT`.) The keyboard is unusable for ~52 s after every
   plug, including at boot: the login screen often appears before the
   keyboard starts working.

2. **Disconnect loops.** Often the second probe fails with `-71`
   (`EPROTO`) and the device is dropped from the bus, re-enumerates, and
   the whole stall starts over:

   ```
   usbhid 5-1:1.2: can't add hid device: -71
   usb 5-1: USB disconnect, device number 3
   usb 5-1: new full-speed USB device number 4 using xhci_hcd
   ```

   In the worst observed case the keyboard was unusable for ~100 s.

3. **`Fn+Space` deadlock.** Pressing `Fn+Space` (backlight on/off) makes
   the firmware hang: it stops sending key reports until the cable is
   re-plugged — and since the keyboard has an internal battery, even a
   re-plug does not always recover it; the physical power switch does.

The same `Fn+Space` works fine on Windows: the OS masks the broken
interfaces, while Linux insists on probing them. **The real fix is a
firmware update from the vendor.** This daemon is the workaround.

## Why it happens

`usbhid` probes every HID interface of every device. The probe reads the
interface's report descriptor with a control request that the wired
firmware never answers — the two vendor interfaces STALL or NAK it.

In the kernel, `hid_add_device()` reads the report descriptor
(`hdev->ll_driver->parse()`) **after** checking `hid_ignore()`. So the
only way to stop the stall is to make `hid_ignore()` return true for the
device: the `HID_QUIRK_IGNORE` quirk.

Once the device is ignored by `usbhid`, nothing claims the keyboard
interface, so this daemon claims it from userspace and re-exposes it to
the kernel as a `uhid` device.

The `Fn+Space` deadlock is a different effect of the same root cause: the
firmware tries to notify the host on the vendor interrupt endpoints
(`0x82`/`0x83`) and waits forever when nobody reads them. The daemon
keeps those endpoints drained, so the firmware never waits.

## The fix

Two pieces:

1. **Kernel**: `usbhid.quirks=0x3151:0x4015:0x4` — `HID_QUIRK_IGNORE`.
   `usbhid` refuses the device before reading any descriptor: enumeration
   is instant and the device never drops off the bus.

2. **This daemon**: claims the three interfaces via libusb and re-exposes
   the keyboard to the kernel as a `uhid` device. The kernel keeps doing
   all the HID parsing (keys, layout); the daemon just moves bytes and
   drains the two vendor endpoints.

## Requirements

- Linux
- `libusb-1.0` (Fedora: `sudo dnf install libusb1-devel`)
- C compiler, `make`
- systemd (for the service unit; the daemon runs standalone too)

## Build and install

```sh
make
sudo make install                          # -> /usr/local/sbin/yunzii-bridge
sudo install -m644 yunzii-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now yunzii-bridge
```

The daemon uses libusb hotplug: it runs permanently and attaches to the
keyboard whenever it appears. It ignores the 2.4 GHz dongle (`3151:4011`),
which works natively.

## Kernel cmdline quirk

Fedora/RHEL (BLS):

```sh
sudo grubby --update-kernel=ALL --args="usbhid.quirks=0x3151:0x4015:0x4"
```

Revert:

```sh
sudo grubby --update-kernel=ALL --remove-args="usbhid.quirks=0x3151:0x4015:0x4"
```

The daemon works without the quirk too, but at every plug it may wait up
to ~20 s for `usbhid` to finish failing on the vendor interfaces.

> Tip: keep your distro's rescue kernel entry **without** the quirk. The
> rescue environment does not start this daemon, so the keyboard still
> works there natively (with the stall, but it works).

## Usage

```
yunzii-bridge [VID_HEX PID_HEX]      # defaults: 3151 4015
```

The arguments let you point it at another keyboard with the same problem.

## Verify

```sh
lsusb -t                                   # interfaces show no driver
journalctl -b -k | grep -E 'usbhid.*-110'  # nothing
journalctl -u yunzii-bridge                # attach log, see below
```

A healthy attach log:

```
yunzii-bridge: report descriptor read: 59 bytes
yunzii-bridge: uhid device created (yunzii-bridge/5/2)
yunzii-bridge: device 5-2 attached
yunzii-bridge: uhid device started (yunzii-bridge/5/2)
```

Then type, press `Fn+Space`, type again.

## RGB backlight

The backlight is controlled by the keyboard firmware itself via `Fn`
combinations — no host software involved, so they work through the bridge
and even at the GRUB menu. Common bindings on the B75 Pro:

| Combo | Action |
|---|---|
| `Fn + Space` | backlight on/off |
| `Fn + Insert` | change effect |
| `Fn + End` | change color |
| `Fn + ↑ / ↓` | brightness |
| `Fn + ← / →` | effect speed |
| `Fn + PgUp` | effect direction |

Exact bindings vary between firmware revisions. **The only dangerous one
is the on/off combo (`Fn+Space`): never press it at the GRUB menu or in
the firmware setup**, where this daemon is not running. From the login
screen onward it is safe.

Arbitrary per-key colors (like the vendor's Windows software) require
speaking the vendor RGB protocol, which is not yet reverse-engineered —
see [Future work](#future-work).

## Known limitations

- **LED sync**: on the B75 Pro the LED output report (`SET_REPORT`) is
  STALLed, so the Caps/Num/Scroll indicators on the keyboard do not follow
  the system state. Keys work; physical LED indicators don't sync.
- **~0.5 s pause after `Fn+Space`**: the firmware re-initialises the
  backlight and briefly stops reporting. It recovers by itself — device
  behaviour, not a bridge bug.
- **GRUB is unprotected**: before Linux boots there is no bridge. If
  `Fn+Space` is pressed there, the firmware hangs and the keyboard stays
  dead through the login screen. Recovery: flip the keyboard's physical
  power switch off and on (unplugging the cable is not enough, the battery
  keeps the MCU alive).
- **Typing depends on the daemon** in wired mode: if it crashes,
  `Restart=` brings it back within seconds; the dongle remains a native
  fallback.

## How it works (in detail)

1. **Hotplug.** The daemon registers a libusb hotplug callback for the
   configured VID/PID (`LIBUSB_HOTPLUG_ENUMERATE` catches keyboards that
   are already connected at daemon start).

2. **Claim.** Interfaces 0, 1 and 2 are claimed with
   `usb_set_auto_detach_kernel_driver`, with a bounded retry loop: at
   enumeration `usbhid` may still be probing (and, without the quirk,
   holds the interfaces for ~50 s before giving up).

3. **Descriptor.** The daemon reads the real report descriptor of
   interface 0 *first* (the device serves it while fresh) and hands it to
   `uhid`, so the kernel parses the reports with the real layout. If the
   descriptor read fails, it tries `SET_PROTOCOL(boot)` and falls back to
   the canonical 63-byte boot-keyboard descriptor.

4. **uhid.** The keyboard is re-created as a virtual device via
   `/dev/uhid` (`UHID_CREATE2`). Input reports from endpoint `0x81` are
   forwarded with `UHID_INPUT2`; the kernel's HID layer maps keys, layout
   and LEDs as usual. `UHID_OUTPUT` reports (LEDs) are translated back
   into `SET_REPORT` control requests on interface 0 — and disabled with
   one log line if the device STALLs them.

5. **Draining.** Endpoints `0x82` and `0x83` are polled continuously and
   the data discarded. This is what keeps the firmware from hanging on
   `Fn+Space`.

6. **Error handling.** Each endpoint worker uses polling transfers with a
   250 ms timeout so shutdown is clean; STALLs are cleared (`CLEAR_HALT`)
   and retried; device removal ends the workers; the uhid reader treats
   the kernel's `UHID_DESTROY` as a stop signal.

7. **Quirk collision (a real gotcha).** The virtual uhid device is
   registered with VID/PID `0x1209:<target PID>`, **not** the physical
   device's IDs: `usbhid.quirks` matches *all* HID devices by
   vendor/product, so a virtual device carrying `3151:4015` would be
   ignored by the same quirk that protects the physical one
   (`Cannot register HID device: error -19`). `0x1209` is the pid.codes
   vendor ID, reserved for userland devices.

## Troubleshooting

**Keyboard dead at the login screen after using `Fn+Space` in GRUB.**
Flip the keyboard's physical power switch off/on. The deadlock is in the
firmware and survives cable re-plugging because of the battery.

**No `uhid device started` in the log, and the kernel says
`Cannot register HID device: error -19`.** The virtual device's VID/PID
collide with a quirk; update to the current version (it uses `0x1209`).

**`claim interface N pending (LIBUSB_ERROR_BUSY), retrying`.** Normal
without the kernel quirk: `usbhid` is still failing on the vendor
interfaces. It resolves by itself; enabling the quirk makes it instant.

**`LED output not supported (...)`.** Expected on the B75 Pro: the
device STALLs the LED output report. Harmless.

## Future work

- **RGB protocol reverse engineering**: the vendor interfaces have no OUT
  endpoints, so the vendor software sends RGB commands as control
  transfers (likely `SET_REPORT`) on EP0. A USB capture of the vendor's
  software (e.g. USBPcap/Wireshark on Windows) would let us replay them
  from Linux and drive arbitrary colors. OpenRGB does not support
  `3151:4015` yet (issue #4214).
- **Upstream kernel patch**: a per-interface ignore mechanism in
  `usbhid` would make the daemon unnecessary; the firmware update from
  ROYUAN/YUNZII remains the true fix.

## License

MIT — see [LICENSE](LICENSE).
