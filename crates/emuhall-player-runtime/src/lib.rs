//! The parts of the runtime the player (emuhall-core-host) runs: controller profiles, save-state identity, the rewind
//! timeline, cheats, video helpers and the netplay relay. MIT-licensed with the player, because the player shares its
//! process with GPL engines; it must never depend on launcher-only crates.
pub mod cheats;
pub mod input;
pub mod relay;
pub mod snapshots;
pub mod timeline;
pub mod video;
