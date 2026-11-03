# Contributing to the EmuHall Player

Thanks for helping. This repository is the playback engine of EmuHall: small, native and tested against synthetic engines. These rules keep it that way.

## Ground rules

- **No game data, ever.** Do not attach, commit or link to games, BIOS files, firmware, console keys or save files from commercial games, in code, tests, issues or pull requests. Tests use the original synthetic engines in `tests/libretro-fake-cores`.
- **Every behaviour change comes with a test.** Most changes belong in `crates/emuhall-core-host/tests/conformance.rs` or in the unit tests of `crates/emuhall-player-runtime`.
- **Report what you measured.** "Tested on Windows 11 with an Xbox pad" is useful. "Should work everywhere" is not.
- **Respect upstream.** Engine bugs belong to the engine's project. Code in `third-party/` is vendored unchanged with its provenance recorded; do not edit it here.

## Before you open a pull request

```sh
cargo fmt --all --check
cargo clippy --workspace -- -D warnings
cargo test --workspace
```

All three must pass. Keep pull requests focused: one fix or feature, with a description of what changed, why, and how you tested it.

## Reporting a bug

Open an issue with:

- your OS version, CPU and GPU, and the player version or commit;
- the engine and its version;
- what you did, what you expected and what happened;
- the player's log (stderr). Remove file paths you consider private.

A synthetic reproduction is ideal. Never attach game files.

## Licence

By contributing, you agree that your contribution is licensed under the [MIT licence](LICENSE) of this repository. The EmuHall name and logo are not part of that licence; see [TRADEMARKS.md](TRADEMARKS.md).
