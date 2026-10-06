# Chlorine_OS (Cl_OS)
> A 32-bit x86 operating system running on QEMU

## Functions
- Command Line Interface (It supports many commands such as `ver` `ls` `cd` `mk` and many more)
- Supports the FAT32 file system
- Simple text editor (arrow keys to move, `Enter` to start a new line , and `F2` key to save)
- Snake game
- Colorful terminal
- 1024x768x24 VBE graphical desktop with drawn app icons, mouse pointer, and keyboard navigation
## Ways to Operate
### Prerequisites
- QEMU
- Cross-compiler toolchain (`i686-elf-gcc`, `nasm`, `ld`)
### Compilation
```bash
make
make run
```
The system starts in VGA text mode. Enter `gui` to switch to the 1024x768x24
VBE framebuffer desktop. Open Files and Editor from the desktop to work with
both windows at once. Drag a title bar to move a window and drag any corner to
resize it. All five desktop apps open as movable, resizable windows and can be
used together:
- **Files** browses the current FAT32 directory; open an entry to edit it.
- **Editor** keeps an independent buffer per window; press F2 to save, or enter
  a filename and press Enter to create a file.
- **Snake** uses the arrow keys to move and Enter to restart after game over.
- **Terminal** accepts shell commands; interactive, mode-switching, and
  VGA-display-specific commands stay available from the VGA CLI.
- **Toolbox** offers a calculator (type an expression and press Enter) and a
  clock/calendar (press 2).

Use a window's close button or ESC to close it. Use the desktop's `EXIT TO CLI`
button to switch back to VGA text mode, then enter `shut` to power off.
`make run` automatically creates a 32 MB FAT32 `hdd.img` if it does not exist.
An existing hard disk image is preserved. This launches QEMU with VNC enabled.
To view the system interface:
1. Download and install **TigerVNC** (or any VNC client)
2. Open TigerVNC and connect to:
   ```
   localhost:5901
   ```
3. Click "Connect" — you should see the Cl_OS boot screen
## AI-Assisted Development Note
AI programming assistance tools were employed throughout the development process of this project.
## About
Cl_OS is a personal project. Feedback and discussions are welcome.
