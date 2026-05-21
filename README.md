# Gloriousctl (Pixart Edition)
**A native Linux command-line configuration tool for the Glorious Model I 2 Wireless (and other Pixart-based Glorious mice).**

Linux gaming is growing rapidly, and hardware support shouldn't be left behind. This project is a reverse-engineered Linux driver for newer Glorious mice that use Pixart microcontrollers. 

**Note:** This is a *baseline* implementation. I completely revamped the protocol logic from older Sinowealth-based Glorious mice to support the new Pixart 192-byte and 256-byte multi-packet architectures. It handles DPI, Polling Rate, Lift-Off Distance, Debounce, and RGB Lighting. The codebase is entirely open for community contribution to map out remaining features (like Keybinds/Macros) or to build a GUI!

## Supported Devices
* **Glorious Model I 2 Wireless** (VID `0x093a` PID `0x821d`)
* *(Add other Pixart-based Glorious mice here if tested)*

## ⚠️ Architecture Note: The "Write-Only" Quirk
During reverse-engineering, it was discovered that the Glorious Model I 2 Wireless firmware acts as a **Write-Only** device to save memory. It does not allow the host PC to read its current configuration. 

To solve this, `gloriousctl` uses a **Local Linux Cache**. 
The first time you run the tool, it generates safe factory defaults and saves them to `~/.gloriousctl_state.bin`. Any time you change a setting, it modifies this local cache and pushes the full payload to the mouse. 

## Prerequisites
To compile this tool, you will need `gcc`, `make`, and the `hidapi` development libraries.
On Debian/Ubuntu-based systems:
```bash
sudo apt update
sudo apt install build-essential libhidapi-dev
