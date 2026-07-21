# __wLaunchELF_R3Z__

Based off of [wLE_ISR](https://github.com/israpps/wLaunchELF_ISR)

# Supported devices:
| Device | Description | Visibility |
|---|---|---|
| __mc:/__ | Memory Cards | Always |
| __usb:/__ | Fat/exFAT (BDM) USB | Always |
| __mmce:/__ | Multi Purpose Memory Card Emulator IE SD2PSX, PSxMemCard Gen2 or MemCard Pro 2. | Always |
| __mx4sio:/__ | SD card interface over memory card port. | Always |
| __hdd:/__ | APA formatted internal HDD | Hidden on deckard |
| __ata:/__ | BDM hard drive, exFAT for now till more are supported | Hiddon on deckard |
| __xfrom:/__ | PSX DESR-XXXX flash storage | Hidden on non-PSX |
| __dvr_hdd0:/__ | PSX DESR-XXXX digital video recorder hdd partition/side | Hidden on non-PSX |
| __cdfs:/__ | CD/DVD File System | Always |
| __udpfs:/__ | Network interface used with [PCM720s UDPFSD Server](https://github.com/pcm720/udpfsd) | Always |

Drivers are lazy loading for for maximum compatibility. MMCE and MX4SIO will incure an IOP reboot as the 2 are incompatible.

Dual HDD/ATA support is built in for future development.

## Features:
- RetroGem Game ID for [PIXEL FX RetroGem](https://www.pixelfx.co/hdmi-retro-gem)
- Launch PS1 VCDs with option for custom POPStarter path, falls back to defaults.
- Writes to history file for disc launches for mmce vmc change
- Applies deckard disc patches
- X/0 applied per region if no config file is found so O is only default for Japan
- Keyboard layout choices: QWERTY, DVORAK, AZERTY, QWERTZ, ABNT, ABC
- Language built: English, Spanish, Italian, French, German, Polish, Portuguese, Portuguese Brazilian, Hungarian
- All config options exposed in gui
- HDD/ATA drives hidden for deckard ps2 (SCPH-75K+)
- Xfrom/dvr_hdd0 hidden from non-PSX consoles
- Dual hdd support
- multi-usb support
- HDD Manager can inject APA partition headers from header files on `usb:`, `mmce0:`, `mmce1:`, or `udpfs:` for HDD-OSD icons/launch metadata
- All drivers besides core lazy loading for faster boot and support of everything
- Create and Extract PSU options so user knows what will happen
- Extra file extensions for Text Editor ShortCuts
- Timestamp manipulation feature to fix the date of any memory card folder containing any icon-based exploit _(\*tuna)_
- Launch KELFs (encrypted elfs)
- Support for PS3/PS4 Dualshocks thanks to Alex Parrado (DS34 build)

### Build shortcuts

- `make all-ds34-off` builds a dedicated ELF without DS34 support.
- `make all-ds34-on` builds a dedicated ELF with DS34 support.
- `make all-ds34-variants` builds both variants in one run.

### HDD APA header injection

The HDD Manager R1 menu includes `Inject Header` for PFS partitions.  
Source for files includes usb, mmce and udpfs:

`<source>/__Headers/<partition name>/`

Required files:
- `system.cnf`
- `icon.sys`
- `list.ico`

Optional file:
- `boot.kelf`  
Use KELFTool to create a kelf.

Example for partition `PP.ULE` on USB:

- `usb:/__Headers/PP.ULE/system.cnf`
- `usb:/__Headers/PP.ULE/icon.sys`
- `usb:/__Headers/PP.ULE/list.ico`


### Build shortcuts

- `make all-ds34-off` builds a dedicated ELF without DS34 support.
- `make all-ds34-on` builds a dedicated ELF with DS34 support.
- `make all-ds34-variants` builds both variants in one run.

# **original readme**
wLaunchELF, formerly known as uLaunchELF, also known as wLE or uLE (abbreviated), is an open source file manager and executable launcher for the Playstation 2 console based off of the original LaunchELF. It contains many different features, including a text editor, hard drive manager, as well as network support, and much more.
