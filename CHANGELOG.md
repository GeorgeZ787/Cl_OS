# Changelog
## Unreleased
### Fixed
- Kept startup and the CLI in VGA text mode; switched to the VBE framebuffer only when `gui` is run.
- Made all desktop icons visible and clickable at 1024x768.
- Added an explicit desktop exit-to-CLI button that restores VGA text mode; ESC no longer leaves the desktop.
- Reapplied VGA text mode and reset its page and cursor when returning from VBE, restoring the standard 80-column CLI.
- Added movable, four-corner-resizable windows for all five desktop apps, with up to five simultaneous windows.
- Added graphical file browsing and independent editor buffers with F2 save support.
- Added an in-window Snake game, interactive graphical terminal, and calculator/clock Toolbox.
- Extended the boot loader to load kernels of up to 124 sectors without crossing a DMA boundary.
- Preserved mouse button state across app transitions so hovering cannot launch apps.
- Accepted VBE 2.0 pitch and color masks when VBE 3.0 linear fields are unavailable.
- Drained the keyboard initialization response before enabling PS/2 mouse input.
- Initialized the kernel BSS before use, including terminal and graphics state.
- Bounded IDE status waits so missing or failed disks return errors instead of hanging.
- Corrected FAT32 deleted-entry checks for signed directory-name bytes.
- Created the build directory automatically when compiling from a clean checkout.
- Created the FAT32 hard disk image automatically when running QEMU if it is missing.

## [v26.1.02] - 2026-09-09
### Added
- Screen scrolling (`Up`/`Down` keys)
- Clock process `time.bin`
- Shift mapping, `CAPS`, `TAB` keys
- Multitasking (`run` / `ps` / `kill`)

### Fixed
- `ps` command failing to recognize processes
- Loop output process until it freezes
- Task scheduler failing to return, causing multiple errors