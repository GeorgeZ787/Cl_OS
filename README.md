# Chlorine_OS (Cl_OS)
> A 32-bit x86 operating system running on QEMU
## Functions
- Command Line Interface (It supports many commands such as `ver` `ls` `cd` `mk` and many more)
- Supports the FAT32 file system
- Simple text editor (arrow keys to move, `Enter` to start a new line , and `F2` key to save)
- Snake game
- Colorful terminal
- Simple GUI desktop (with mouse driver)
## Ways to Operate
### Prerequisites
- QEMU
- Cross-compiler toolchain (`i686-elf-gcc`, `nasm`, `ld`)
### Compilation
```bash
make
make run
```
This will launch QEMU with VNC enabled. To view the system interface:
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
