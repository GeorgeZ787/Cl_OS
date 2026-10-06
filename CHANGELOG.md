# Changelog

## [v26.1.03] - 
**[For more information](./docs/updates/v26103.md)**
### Added
- A new `ver` interface

### Fixed
- `ver` interface can not show the correct version number
### Probelems

## [v26.1.02] - 2026-09-09
**[For more information](./docs/updates/v26102.md)**
### Added
- Screen scrolling (`Up`/`Down` keys)
- Clock process `time.bin`
- Shift mapping, `CAPS`, `TAB` keys
- Multitasking (`run` / `ps` / `kill`)

### Fixed
- `ps` command failing to recognize processes
- Loop output process until it freezes
- Task scheduler failing to return, causing multiple errors

### Problems
- The clock can not shutdown normally
- A bug with version number occur on command `ver`