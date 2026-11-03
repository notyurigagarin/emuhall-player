//! The player and its runtime are MIT-licensed and published on their own (emuhall-player), because the player shares
//! its process with GPL engines. They must never depend on the proprietary launcher crates.
const LAUNCHER_CRATES: [&str; 4] = [
    "emuhall-app",
    "emuhall-library",
    "emuhall-runtime",
    "emuhall-desktop",
];

#[test]
fn player_crates_do_not_depend_on_launcher_crates() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    for manifest in [
        root.join("Cargo.toml"),
        root.join("../emuhall-player-runtime/Cargo.toml"),
    ] {
        let text = std::fs::read_to_string(&manifest).unwrap();
        for line in text.lines() {
            let name = line.split('=').next().unwrap_or("").trim();
            assert!(
                !LAUNCHER_CRATES.contains(&name),
                "{} depends on the launcher crate {name}",
                manifest.display()
            );
        }
        assert!(
            text.contains("license = \"MIT\""),
            "{} must stay MIT",
            manifest.display()
        );
    }
}
