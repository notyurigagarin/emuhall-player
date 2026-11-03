# SDL3 (controllers)

Simple DirectMedia Layer by Sam Lantinga and the SDL contributors. EmuHall's player loads it at run time to read
game controllers: Xbox, PlayStation 3/4/5, Nintendo Switch and many generic USB pads, by name and button layout.
EmuHall does not modify it.

- Upstream: https://github.com/libsdl-org/SDL, release `release-3.4.18` (2026-10-02)
- `bin/SDL3.dll` and `include/`: from the official `SDL3-devel-3.4.18-mingw.tar.gz` (x86_64-w64-mingw32), whose
  SHA-256 `049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf` matches GitHub's published digest.
- `SDL3-3.4.18-source.tar.gz`: the official source archive, SHA-256
  `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3` (matches GitHub's digest).
- Copied: 2026-10-25.

## License

zlib (`LICENSE.txt`): free to use and redistribute; the notice must stay with source copies.

## How EmuHall uses it

`crates/emuhall-core-host/native/gamepad.c` loads `SDL3.dll` from the player's folder (it falls back to XInput when
the file is missing) and reads every connected pad as player 1. `tools/build-dev.ps1` copies the DLL beside the
player; the Rust build copies it beside development builds.

## EmuHall modifications

None.

## Controller table in the launcher

`tools/extract-sdl-gamepad-mappings.mjs` reads `src/joystick/SDL_gamepad_db.h` from the pinned source archive and
writes `apps/desktop/src/sdl-gamepad-mappings.json`: for each Windows DirectInput vendor/product, which raw button is
A, B, X, Y, the shoulders, triggers, Back, Start and the stick clicks. The launcher reads pads through the browser,
which reports such pads with raw button numbers; the table gives them the same layout the player gets from SDL.
Hats and stick axes are not taken from the table. Regenerate after updating SDL.
