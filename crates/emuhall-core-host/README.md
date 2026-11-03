# EmuHall player

The playback worker of EmuHall: it loads one Libretro engine (an emulator core) into its own process, runs the game,
and handles video (OpenGL, Direct3D 11/12, Vulkan or software), audio, controllers, save states, rewind, cheats,
RetroAchievements and netplay. The EmuHall launcher starts it and talks to it only through command-line arguments,
environment variables and pipes (commands on stdin, JSON events on stdout, the log on stderr).

## Licence

The player (`crates/emuhall-core-host`) and its runtime (`crates/emuhall-player-runtime`) are MIT-licensed, copyright
Braza IO Capital Holding. Engines are separate programs under their own licences (many are GPL) and are not part of
this repository. Third-party code in `third-party/` keeps its own licence; see each folder's licence and provenance
files (the Libretro headers carry their MIT licence in the header text).

The EmuHall name and icon (`apps/desktop/src-tauri/icons/icon.ico`, embedded in the executable) are trademarks of
Braza IO Capital Holding and are not licensed under MIT.

## Building

Rust (version pinned in `rust-toolchain.toml`, target `x86_64-pc-windows-gnu`) and a MinGW-w64 GCC toolchain.

    cargo build --release -p emuhall-core-host
    cargo test --workspace

The tests build original synthetic engines from `tests/libretro-fake-cores`; no game data is needed or included.

## Running

    emuhall-core-host <engine library> <game file> <save file> <frame limit, 0 = none> headless|window|control|embed:<window>

Save states, screenshots and `core-options.cfg` live in the save file's folder.

`EMUHALL_SYSTEM` names the console; `EMUHALL_RESUME_SLOT` and `EMUHALL_STATE_IDENTITY` resume a save state.
