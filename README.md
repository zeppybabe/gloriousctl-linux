# Gloriousctl (Pixart Edition)
**A native Linux command-line configuration tool for the Glorious Model I 2 Wireless (and other Pixart-based Glorious mice).**

Linux gaming is growing rapidly, and hardware support shouldn't be left behind. This project is a reverse-engineered Linux driver for newer Glorious mice that use Pixart microcontrollers. 

**Note:** This is a *baseline* implementation. I completely revamped the protocol logic from older Sinowealth-based Glorious mice to support the new Pixart 192-byte and 256-byte multi-packet architectures. It handles DPI, Polling Rate, Lift-Off Distance, Debounce, and RGB Lighting. The codebase is entirely open for community contribution to map out remaining features (like Keybinds/Macros) or to build a GUI!

## Supported Devices
* **Glorious Model I 2 Wireless** (VID `0x093a` PID `0x821d`)
* *(Add other Pixart-based Glorious mice here if tested)*

## ⚠️ Architecture Note: Write-Only peri.
During reverse-engineering, it was discovered that the Glorious Model I 2 Wireless firmware acts as a **Write-Only** device to save memory. It does not allow the host PC to read its current configuration. 

To solve this, `gloriousctl` uses a **Local Linux Cache**. 
The first time you run the tool, it generates safe factory defaults and saves them to `~/.gloriousctl_state.bin`. Any time you change a setting, it modifies this local cache and pushes the full payload to the mouse. 

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
