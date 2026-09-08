# gloriousctl

Configure Glorious mice on Linux from the command line or a small GTK window.

Two families are supported by two separate code paths:

| Family | Mice | Protocol | Status |
|---|---|---|---|
| SinoWealth (VID 258a) | Model O / O-, Model D, Dream Machines DM5 | 520-byte feature report, readable | ported from upstream, unchanged |
| Pixart (VID 093a) | Model I 2 Wireless / Wired (093a:821d) | 64-byte write-only fragments | new in this fork, mapped on real hardware |

<<<<<<< HEAD
## ⚠️ Architecture Note: Write-Only peri.
During reverse-engineering, it was discovered that the Glorious Model I 2 Wireless firmware acts as a **Write-Only** device to save memory. It does not allow the host PC to read its current configuration. 
=======
Anything else that enumerates as Glorious is detected and refused rather than written to.
See "Getting your mouse supported" below.
>>>>>>> 6c96134 (Pixart: hardware-mapped DPI/colour/polling, GTK GUI, installer, README)

## Install

<<<<<<< HEAD
## Prerequisites
To compile this tool, you will need `gcc`, `make`, and the `hidapi` development libraries.
On Debian/Ubuntu-based systems:
```bash
sudo apt update
sudo apt install build-essential libhidapi-dev

```
## Compilation and Global Installation

Clone the repository and compile the source code:
```bash
git clone https://github.com/zeppybabe/gloriousctl-linux.git
cd gloriousctl-linux
make
```
To make the command available globally on your Linux system, copy the compiled binary to your local bin dir:
```bash
sudo cp gloriousctl /usr/local/bin
```
## Commands and parameters

| Command | Description |
|---|---|
| `gloriousctl --info` | Displays the current cached configuration. |
| `gloriousctl --set-dpi` | Sets up to 6 DPI stages (comma separated).Example: `400`,`800`,`1600`,`3200` |
| `gloriousctl --set-dpi-color` | Sets colors for each DPI stage (Hex, comma-separated). Ex.: `FF0000`,`0000FF` |
| `gloriousctl --set-effect` | Sets the lighting mode.(Options: `off`, `rave`, `glorious`, `breathing`, etc.) |
| `gloriousctl --set-colors` | Sets the colors for the selected effect. |
| `gloriousctl --set-brightness` | Sets effect brightness (0 to 4). |
| `gloriousctl --set-speed` | Sets effect animation speed (0 to 3). |
| `gloriousctl --set-debounce-time` | Sets click debounce in ms (2-16, even numbers. Odds get rounded down to even). |
| `gloriousctl --help` | Displays the built-in help text. |

## Original repo basis

[enkore's repo](https://github.com/enkore/gloriousctl)
=======
```
git clone <this repo>
cd gloriousctl
./install.sh
```

`install.sh` builds the CLI, installs it to `/usr/local/bin`, installs a udev rule so no
`sudo` is needed, installs the GUI and a menu entry, and offers to install PyGObject/GTK3
if the GUI needs it. Replug the mouse once after the first install.

- `./install.sh --verify` checks permissions without changing anything.
- `./install.sh --purge` removes everything, including the udev rule and cached state.

Dependencies: `hidapi` (libhidapi-hidraw), a C compiler, `make`. GUI only: PyGObject with
GTK 3 (`python3-gi gir1.2-gtk-3.0` on Debian/Ubuntu, `python3-gobject` on Fedora,
`python-gobject` on Arch).

## Use

```
gloriousctl --info
gloriousctl --set-dpi 400,800,1600,3200 --set-dpi-color FF0000,00FF00,0000FF,FFFFFF
gloriousctl --set-stages 6 --set-dpi 400,800,1600,3200,6400,12800
gloriousctl --set-effect breathing --set-colors FF0000,0000FF --set-brightness 4
gloriousctl --set-polling-rate 500 --set-lod 2
gloriousctl-gui
```

`gloriousctl --help` lists every option. The GUI runs the same commands and shows each
one, with its output, in a log pane.

### Things to know about the Pixart mice

- **Write-only.** The mouse never reports its settings back. `--info` and the GUI show the
  local cache, i.e. what was last sent, not what the mouse holds. A transport
  acknowledgement is not proof the setting applied.
- **The cache is the source of truth for every send.** `~/.gloriousctl_state.bin` holds the
  full lighting and settings payloads. Every command edits the fields you named and then
  transmits the *whole* payload, so values from earlier commands persist until overwritten.
  `--reset-cache` puts it back to defaults (sends nothing); `install.sh` clears it too. The
  file is versioned and an incompatible one is ignored, never replayed.
- **Polling rate.** 125/250/500 verified. The 1000 Hz code reads as 500–1000 on browser-based
  rate meters, which are limited by the compositor; measure with `evhz` for a real figure.
- **Wake it first.** A sleeping wireless mouse drops fragments. Wiggle it, then send.
- **Cycle to see a DPI change.** The stage you are on keeps its old DPI until you press the
  DPI button away and back.
- **Every command resets the active stage** to what the cache holds (stage 1 by default).
- **Stages 2 and 4 cannot have a free blue channel.** Their blue byte is the same wire byte
  as the next stage's DPI low byte. DPI wins; `--info` shows the colour actually sent.
- `--set-debounce-time` is sent as documented and cannot be verified.
- The lighting "speed" byte is written where the datasheet puts it, but no speed change has
  been observed on hardware yet.

## Getting your mouse supported

The Pixart family is mapped from hardware observation, and every unsupported model needs
the same treatment. To help:

1. Run `gloriousctl --collect > my-mouse.txt`. It gathers kernel info, HID enumeration and
   the raw report descriptors of any Glorious device. It is read-only and local; nothing is
   transmitted anywhere.
2. Open an issue and attach `my-mouse.txt`, plus the mouse's exact name and whether it is
   wired or wireless.
3. Be ready to run a few `--raw-settings` / `--raw-lighting` test commands and say what the
   mouse did. That is how every byte of the Model I 2 map was found.

If you can capture Glorious CORE on Windows (Wireshark + USBPcap in a VM), attach the
capture too — it shortens the work enormously. `gloriousctl --dump-payload --set-...` prints
the exact bytes this tool would send, for diffing against a capture.

## How the Pixart map was established

Every constant in `gloriousctl.c` carries a comment saying where it came from and how
confident it is. In short, the community datasheet this fork started from was wrong in four
places, all found on a Model I 2 Wireless:

- DPI is the raw value, little-endian, not DPI/50.
- Every stage's DPI field is one byte earlier than documented; the documented 0x01 pad after
  each fragment header does not exist (it was the DPI low byte, which pinned stages at max).
- Stage 4's colour bytes overlap stage 5's DPI, and stage 2's overlap stage 3's.
- The polling-rate scale is linear (0x01 = 125 Hz … 0x04 = 1000 Hz), not the documented order.

Also found: back-to-back fragments get dropped by the wireless firmware while the dongle
still acknowledges them, so fragments are sent 120 ms apart.

## Files

- `gloriousctl.c` — the whole CLI, both protocols.
- `gloriousctl-gui.py` — GTK3 front end (PyGObject); builds and runs CLI commands.
- `install.sh`, `Makefile`, `gloriousctl.desktop`.

## Licence and attribution

Licensed under the European Union Public Licence v1.2 (see `LICENSE`).

The SinoWealth support, the command-line structure and the original tool are
[enkore/gloriousctl](https://github.com/enkore/gloriousctl), copyright 2020 Marian Beermann,
EUPL-1.2. The exact upstream commit the port was taken from was not recorded at the time;
if you know it, please open an issue so it can be listed here.

Pixart (Model I 2) support, the GUI and the installer were added in 2026 with the help of
the community around the original repository.
>>>>>>> 6c96134 (Pixart: hardware-mapped DPI/colour/polling, GTK GUI, installer, README)
