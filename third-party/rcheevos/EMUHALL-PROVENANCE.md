# rcheevos (vendored subset)

rcheevos is the official RetroAchievements client library, written by the RetroAchievements team. EmuHall did not
write it. EmuHall's playback worker compiles it in to identify games, evaluate achievements and talk to
RetroAchievements, as RetroArch and other emulators do.

- Upstream: https://github.com/RetroAchievements/rcheevos
- Release: v12.5.0, commit `1433173220a7eaede6a9ed7a18e94117be1821e0`
- Archive SHA-256: `da635153d2dab228f3a3ef672bae56a7a6e5f57f1aacd420133521c5d3768cec`
- Copied: 2026-10-24. The client (`rc_client`), runtime, server API, hashing and libretro memory-map helpers, with the
  headers they include (found with `gcc -MM`). Not copied: RAIntegration (developer toolkit), the external-client
  bridge, encrypted-ROM (3DS) hashing, tests and the validator.
- Built with `RC_CLIENT_SUPPORTS_HASH` and `RC_HASH_NO_ENCRYPTED` by `crates/emuhall-core-host/build.rs`.
- `SHA256SUMS` lists every vendored file.

## License

MIT (`LICENSE`), copyright RetroAchievements.org.

## EmuHall additions (not upstream files)

`emuhall-generated/sources.txt`: the compiled file list.

## EmuHall modifications to upstream files

None.
