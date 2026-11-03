use std::{
    fs,
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

#[cfg(windows)]
#[test]
fn d3d_presentation_resizes_and_honors_display_aspect() {
    let output = Command::new(env!("EH_D3D_PRESENTATION_TEST"))
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "status {:?}: {} {}",
        output.status.code(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn audio_pacing_distinguishes_healthy_refills_from_real_shortages() {
    let output = Command::new(env!("EH_AUDIO_PACING_TEST")).output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stdout)
    );
}

#[cfg(windows)]
#[test]
fn ps2_audio_output_waits_for_a_buffer_before_starting() {
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("synthetic.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original audio fixture").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "120",
            "window",
        ])
        .env("EMUHALL_SYSTEM", "ps2")
        .env("EMUHALL_AUDIO_DIAGNOSTICS", "1")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let log = String::from_utf8_lossy(&output.stderr);
    let buffered = log
        .lines()
        .find_map(|line| {
            line.strip_prefix("Audio: playback started with ")
                .and_then(|line| line.split_whitespace().next())
                .and_then(|n| n.parse::<f64>().ok())
        })
        .expect("Output should start after priming");
    assert!((96.0..=171.0).contains(&buffered), "{log}");
    let summary = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| serde_json::from_str::<serde_json::Value>(line).ok())
        .find(|value| value["frames"] == 120)
        .unwrap();
    assert_eq!(summary["audio_frames"], 120 * 800);
}

#[test]
fn console_system_storage_is_separate_from_game_save_storage() {
    let dir = tempfile::tempdir().unwrap();
    let shared = dir.path().join("shared-console");
    let profile = dir.path().join("game-profile");
    fs::create_dir_all(&shared).unwrap();
    fs::create_dir_all(&profile).unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            profile.join("native.srm").to_str().unwrap(),
            "2",
            "headless",
        ])
        .env("EMUHALL_CORE_SYSTEM_DIRECTORY", &shared)
        .env("EMUHALL_FIXTURE_SYSTEM_DIRECTORY", &shared)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stderr).contains("shared system directory is separate"));
    assert!(profile.join("native.srm").is_file());
    assert!(!shared.join("native.srm").exists());
}

#[test]
fn input_test_streams_four_ports_without_loading_a_core_and_exits_on_pipe_close() {
    use std::io::BufRead;
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .arg("--input-test")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let input = child.stdin.take().unwrap();
    let output = child.stdout.take().unwrap();
    let (sender, receiver) = std::sync::mpsc::channel();
    let reader = thread::spawn(move || {
        for line in std::io::BufReader::new(output).lines() {
            let Ok(line) = line else {
                break;
            };
            if let Ok(event) = serde_json::from_str::<serde_json::Value>(&line) {
                if event["event"] == "controller-input" {
                    let _ = sender.send(event);
                }
            }
        }
    });
    let sample = receiver.recv_timeout(Duration::from_secs(8));
    drop(input);
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        if started.elapsed() > Duration::from_secs(3) {
            let _ = child.kill();
            panic!("Input reader survived launcher pipe close");
        }
        thread::sleep(Duration::from_millis(10));
    };
    reader.join().unwrap();
    assert!(status.success());
    assert_eq!(sample.unwrap()["ports"].as_array().unwrap().len(), 4);
}

#[test]
fn launcher_gate_waits_for_settings_even_without_a_game_window() {
    use std::io::Write;
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("gated.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0original launch-gate diagnostic").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_TEMPORAL"),
            rom.to_str().unwrap(),
            save.to_str().unwrap(),
            "1",
            "headless",
        ])
        .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    thread::sleep(Duration::from_millis(250));
    assert!(child.try_wait().unwrap().is_none());
    assert!(!save.exists());
    let mut pipe = child.stdin.take().unwrap();
    pipe.write_all(b"set filter lcd\nstart\n").unwrap();
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(save.exists());
}

#[test]
fn older_states_require_opt_in_and_preserve_the_previous_in_game_save() {
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("old.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0original older-state diagnostic").unwrap();
    fs::write(&save, [0x66u8; 32]).unwrap();
    let mut state = vec![0u8; 36];
    state[..4].copy_from_slice(&42u32.to_ne_bytes());
    state[8..12].copy_from_slice(&42u32.to_ne_bytes());
    fs::write(dir.path().join("state1.state"), &state).unwrap();
    let launch = |older: bool| {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"));
        cmd.args([
            env!("EH_CORE_TEMPORAL"),
            rom.to_str().unwrap(),
            save.to_str().unwrap(),
            "1",
            "headless",
        ])
        .env("EMUHALL_RESUME_SLOT", "1");
        if older {
            cmd.env("EMUHALL_LEGACY_STATE_SLOT", "1");
        }
        cmd.output().unwrap()
    };
    assert!(!launch(false).status.success());
    assert_eq!(fs::read(&save).unwrap(), [0x66u8; 32]);
    let loaded = launch(true);
    assert!(
        loaded.status.success(),
        "{}",
        String::from_utf8_lossy(&loaded.stderr)
    );
    let current = fs::read(&save).unwrap();
    assert_eq!(u32::from_ne_bytes(current[4..8].try_into().unwrap()), 43);
    assert_eq!(
        fs::read(dir.path().join("native.srm.bak")).unwrap(),
        [0x66u8; 32]
    );
    assert_eq!(fs::read(dir.path().join("state1.state")).unwrap(), state);
}

#[test]
fn runahead_preserves_canonical_memory_audio_and_input_and_failed_restores_preserve_saves() {
    use std::io::Write;
    let execute = |core: &str, ahead: bool| {
        let dir = tempfile::tempdir().unwrap();
        let rom = dir.path().join("temporal.ehdemo");
        let save = dir.path().join("native.srm");
        fs::write(&rom, b"EMUHALL-DEMO\0original temporal diagnostic").unwrap();
        fs::write(&save, [0x6au8; 32]).unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args([
                core,
                rom.to_str().unwrap(),
                save.to_str().unwrap(),
                "120",
                "control",
            ])
            .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
            .env("EMUHALL_TEMPORAL_SUPPORTED", "1")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let mut pipe = child.stdin.take().unwrap();
        writeln!(
            pipe,
            "set runahead {}\nstart",
            if ahead { "1" } else { "off" }
        )
        .unwrap();
        pipe.flush().unwrap();
        let output = child.wait_with_output().unwrap();
        let bytes = fs::read(save).unwrap();
        (output, bytes)
    };
    let (baseline, normal) = execute(env!("EH_CORE_TEMPORAL"), false);
    let (ahead, predicted) = execute(env!("EH_CORE_TEMPORAL"), true);
    assert!(
        baseline.status.success(),
        "{}",
        String::from_utf8_lossy(&baseline.stderr)
    );
    assert!(
        ahead.status.success(),
        "{}",
        String::from_utf8_lossy(&ahead.stderr)
    );
    assert_eq!(normal, predicted);
    assert_eq!(u32::from_ne_bytes(predicted[4..8].try_into().unwrap()), 120);
    let summary = |output: &std::process::Output| -> serde_json::Value {
        String::from_utf8_lossy(&output.stdout)
            .lines()
            .filter_map(|line| serde_json::from_str::<serde_json::Value>(line).ok())
            .find(|v| v.get("frames").is_some())
            .unwrap()
    };
    let b = summary(&baseline);
    let a = summary(&ahead);
    for key in ["frames", "audio_frames", "input_polls", "frame_hash"] {
        assert_eq!(a[key], b[key], "{key}");
    }
    let (failed, save) = execute(env!("EH_CORE_TEMPORAL_FAIL"), true);
    assert!(!failed.status.success());
    assert_eq!(save, [0x6au8; 32]);
}

#[test]
fn rewind_restores_an_older_complete_snapshot_without_using_disk_slots() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("temporal.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original rewind diagnostic").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_TEMPORAL"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
        .env("EMUHALL_TEMPORAL_SUPPORTED", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut pipe = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let mut next =
        || -> serde_json::Value { serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap() };
    assert_eq!(next()["event"], "started");
    writeln!(pipe, "set rewind on").unwrap();
    assert_eq!(next()["event"], "properties");
    writeln!(pipe, "start").unwrap();
    // Wait for actual history, rather than assuming a busy machine runs enough frames in 120 ms.
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        writeln!(pipe, "properties").unwrap();
        let properties = next();
        if properties["runtime"]["rewindSteps"].as_u64().unwrap_or(0) >= 2 {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "Rewind history did not become available"
        );
        thread::sleep(Duration::from_millis(20));
    }
    writeln!(pipe, "menu").unwrap();
    assert_eq!(next()["event"], "menu");
    writeln!(pipe, "save-state 1").unwrap();
    assert_eq!(next()["event"], "state-saved");
    writeln!(pipe, "rewind").unwrap();
    assert_eq!(next()["event"], "rewound");
    writeln!(pipe, "save-state 2").unwrap();
    assert_eq!(next()["event"], "state-saved");
    let id = emuhall_player_runtime::snapshots::Identity::local(
        emuhall_player_runtime::snapshots::file_digest(&rom).unwrap(),
        emuhall_player_runtime::snapshots::file_digest(std::path::Path::new(env!(
            "EH_CORE_TEMPORAL"
        )))
        .unwrap(),
        emuhall_player_runtime::snapshots::digest(b""),
    );
    let first =
        emuhall_player_runtime::snapshots::load(&dir.path().join("state1.state"), &id).unwrap();
    let older =
        emuhall_player_runtime::snapshots::load(&dir.path().join("state2.state"), &id).unwrap();
    assert!(
        u32::from_ne_bytes(older[..4].try_into().unwrap())
            < u32::from_ne_bytes(first[..4].try_into().unwrap())
    );
    assert_eq!(&older[..4], &older[8..12]);
    writeln!(pipe, "quit").unwrap();
    assert!(child.wait().unwrap().success());
}

#[cfg(windows)]
#[test]
fn portable_sdl_surface_presents_with_dummy_devices() {
    let output = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .arg("--sdl-smoke")
        .env("SDL_VIDEODRIVER", "dummy")
        .env("SDL_AUDIODRIVER", "dummy")
        .env("SDL_RENDER_DRIVER", "software")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}
fn run(core: &str) -> (bool, String) {
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            core,
            rom.to_str().unwrap(),
            dir.path().join("save.srm").to_str().unwrap(),
            "60",
            "headless",
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let start = Instant::now();
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if start.elapsed() > Duration::from_secs(5) {
            child.kill().unwrap();
            child.wait().unwrap();
            return (false, "timeout".into());
        }
        thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().unwrap();
    (
        output.status.success(),
        String::from_utf8_lossy(&output.stdout).into_owned(),
    )
}
#[test]
fn load_time_layout_overrides_placeholder_geometry_maxima() {
    let (ok, text) = run(env!("EH_CORE_LAYOUT_GEOMETRY"));
    assert!(ok, "{text}");
    let report: serde_json::Value = serde_json::from_str(text.trim()).unwrap();
    assert_eq!(report["frames"], 60);
    assert_eq!(report["video_frames"], 60);
}
#[test]
fn provisional_save_memory_may_become_zero_without_failing_playback() {
    let (ok, text) = run(env!("EH_CORE_DYNAMIC_SAVE"));
    assert!(ok, "{text}");
    let summary: serde_json::Value =
        serde_json::from_str(text.trim().lines().last().unwrap()).unwrap();
    assert_eq!(summary["frames"], 60);
    assert_eq!(summary["save_bytes"], 0);
}
#[test]
fn properties_report_choices_disc_switches_and_cheats_reach_the_core() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("fixture.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_PROPERTIES"),
            rom.to_str().unwrap(),
            save.to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut pipe = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let mut next =
        || -> serde_json::Value { serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap() };
    assert_eq!(next()["event"], "started");
    writeln!(pipe, "properties").unwrap();
    let properties = next();
    assert_eq!(properties["discs"]["count"], 2);
    assert_eq!(properties["discs"]["index"], 0);
    assert_eq!(properties["cheatsSupported"], true);
    assert_eq!(properties["options"][0]["value"], "standard");
    assert_eq!(properties["options"][0]["options"][0][0], "alternate");
    writeln!(pipe, "change-disc 1").unwrap();
    let changed = next();
    assert_eq!(changed["discs"]["index"], 1);
    assert_eq!(changed["discs"]["ejected"], false);
    writeln!(pipe, "change-disc 2").unwrap();
    assert_eq!(next()["event"], "error");
    writeln!(pipe, "properties").unwrap();
    assert_eq!(next()["discs"]["index"], 1);
    writeln!(
        pipe,
        "cheat 0 1 1234 ABCD\nset volume 50\nset mute on\nquit"
    )
    .unwrap();
    drop(pipe);
    while next().get("event").is_some() {}
    assert!(child.wait().unwrap().success());
    assert_eq!(
        fs::read(save).unwrap()[2],
        1,
        "The synthetic core received the enabled cheat"
    );
}

#[test]
fn cheats_sent_before_the_first_frame_reach_engines_that_start_late() {
    // melonDS DS with a graphics-card renderer creates its console on the first frame and drops earlier cheats;
    // the launcher always sends cheats before "start".
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("fixture.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_LATE_CHEATS"),
            rom.to_str().unwrap(),
            save.to_str().unwrap(),
            "0",
            "control",
        ])
        .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut pipe = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let first: serde_json::Value = serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap();
    assert_eq!(first["event"], "started");
    writeln!(pipe, "cheat-reset\ncheat 0 1 1234 ABCD\nstart").unwrap();
    pipe.flush().unwrap();
    thread::sleep(Duration::from_millis(400));
    writeln!(pipe, "quit").unwrap();
    drop(pipe);
    for _ in lines.by_ref() {}
    assert!(child.wait().unwrap().success());
    assert_eq!(
        fs::read(save).unwrap()[2],
        1,
        "A cheat sent before the first frame was replayed once the engine had started"
    );
}

#[test]
fn engines_offering_a_dualshock_get_one_so_analog_sticks_work() {
    // PCSX ReARMed lists a digital pad first; choosing it would leave PS1 sticks dead.
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("fixture.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_DUALSHOCK"),
            rom.to_str().unwrap(),
            save.to_str().unwrap(),
            "0",
            "control",
        ])
        .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut pipe = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    lines.next().unwrap().unwrap();
    writeln!(pipe, "start").unwrap();
    pipe.flush().unwrap();
    thread::sleep(Duration::from_millis(300));
    writeln!(pipe, "quit").unwrap();
    drop(pipe);
    for _ in lines.by_ref() {}
    assert!(child.wait().unwrap().success());
    let sram = fs::read(save).unwrap();
    // RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1) is 0x205.
    assert_eq!((sram[3], sram[5]), (0x02, 0x05));
}

#[test]
fn engine_inspection_discovers_load_time_options_without_running_frames_or_saving() {
    let dir = tempfile::tempdir().unwrap();
    let save = dir.path().join("probe.srm");
    let result = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_NDS"),
            dir.path().join("probe.ehprobe").to_str().unwrap(),
            save.to_str().unwrap(),
            "0",
            "inspect",
        ])
        .output()
        .unwrap();
    assert!(result.status.success());
    let events: Vec<serde_json::Value> = String::from_utf8(result.stdout)
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert!(events.last().unwrap()["options"]
        .as_array()
        .unwrap()
        .iter()
        .any(|option| option["key"] == "melonds_screen_layout1"));
    assert!(!save.exists());
    assert!(events.iter().all(|event| event["event"] == "properties"));
}

#[test]
fn valid_core_calls_video_audio_and_input() {
    let (ok, text) = run(env!("EH_CORE_VALID"));
    assert!(ok, "{text}");
    let v: serde_json::Value = serde_json::from_str(&text).unwrap();
    assert_eq!(v["frames"], 60);
    assert_eq!(v["video_frames"], 60);
    assert_eq!(v["input_polls"], 60);
    assert_eq!(v["audio_frames"], 48000);
    assert_eq!(v["save_bytes"], 32);
}
#[test]
fn rejects_unsupported_api() {
    assert!(!run(env!("EH_CORE_BAD_API")).0);
}
#[test]
fn handles_failed_game_load() {
    assert!(!run(env!("EH_CORE_LOAD_FAIL")).0);
}
#[test]
fn malformed_geometry_is_rejected() {
    assert!(!run(env!("EH_CORE_BAD_GEOMETRY")).0);
}
#[test]
fn core_crash_is_confined_to_child() {
    assert!(!run(env!("EH_CORE_CRASH")).0);
    assert!(run(env!("EH_CORE_VALID")).0);
}
#[test]
fn stuck_worker_can_be_killed() {
    assert_eq!(run(env!("EH_CORE_HANG")).1, "timeout");
}
#[test]
fn engine_directories_are_the_managed_save_folder_not_the_working_directory() {
    let dir = tempfile::tempdir().unwrap();
    let profile = dir.path().join("profile");
    let elsewhere = dir.path().join("elsewhere");
    fs::create_dir_all(&profile).unwrap();
    fs::create_dir_all(&elsewhere).unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let status = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_DIR_PROBE"),
            rom.to_str().unwrap(),
            profile.join("native.srm").to_str().unwrap(),
            "2",
            "headless",
        ])
        .current_dir(&elsewhere)
        .output()
        .unwrap()
        .status;
    assert!(status.success());
    assert!(profile.join("probe.txt").is_file());
    assert!(!elsewhere.join("probe.txt").exists());
}
#[test]
fn launcher_commands_pause_states_screenshots_and_quit() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let mut next =
        || -> serde_json::Value { serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap() };
    let mut send = |command: &str| writeln!(stdin, "{command}").unwrap();
    assert_eq!(next()["event"], "started");
    send("load-state 2");
    assert_eq!(next()["message"], "Slot 2 is empty.");
    send("save-state 2");
    assert_eq!(next(), serde_json::json!({"event":"state-saved","slot":2}));
    let preview = fs::read(dir.path().join("state2.bmp")).unwrap();
    assert_eq!(&preview[..2], b"BM");
    assert_eq!(u32::from_le_bytes(preview[18..22].try_into().unwrap()), 160);
    assert_eq!(u32::from_le_bytes(preview[22..26].try_into().unwrap()), 144);
    send("load-state 2");
    assert_eq!(next(), serde_json::json!({"event":"state-loaded","slot":2}));
    send("menu");
    assert_eq!(next()["event"], "menu");
    assert!(dir.path().join("menu-frame.bmp").is_file());
    send("save-state 10");
    assert_eq!(next(), serde_json::json!({"event":"state-saved","slot":10}));
    let first_state = fs::read(dir.path().join("state10.state")).unwrap();
    assert_eq!(
        fs::read(dir.path().join("state10.bmp")).unwrap(),
        fs::read(dir.path().join("menu-frame.bmp")).unwrap()
    );
    send("save-state 10");
    assert_eq!(next()["event"], "state-saved");
    assert_eq!(
        fs::read(dir.path().join("state10.state.bak")).unwrap(),
        first_state
    );
    send("load-state 10");
    assert_eq!(
        next(),
        serde_json::json!({"event":"state-loaded","slot":10})
    );
    for command in [
        "save-state 0",
        "save-state 11",
        "load-state -1",
        "save-state invalid",
    ] {
        send(command);
        let error = next();
        assert_eq!(error["event"], "error");
        assert_eq!(error["message"], "Choose a save slot from 1 to 10.");
    }
    assert!(
        !dir.path().join("state1.state").exists(),
        "An invalid slot must never overwrite slot 1"
    );
    send("screenshot");
    assert_eq!(next()["event"], "screenshot");
    send("fast-forward");
    assert_eq!(
        next(),
        serde_json::json!({"event":"fast-forward","on":true})
    );
    send("reset");
    assert_eq!(next()["event"], "reset");
    send("resume");
    assert_eq!(next()["event"], "resumed");
    send("quit");
    let stats = next();
    assert!(stats.get("event").is_none() && stats["save_bytes"] == 32);
    assert!(child.wait().unwrap().success());
    assert_eq!(
        emuhall_player_runtime::snapshots::inspect(&dir.path().join("state2.state"))
            .unwrap()
            .size,
        4
    );
    let bmp = fs::read_dir(dir.path())
        .unwrap()
        .flatten()
        .find(|e| e.file_name().to_string_lossy().starts_with("screenshot-"))
        .unwrap();
    assert_eq!(&fs::read(bmp.path()).unwrap()[..2], b"BM");
}

#[test]
fn saving_and_closing_uses_a_separate_verified_resume_state_and_failed_saves_stay_open() {
    use std::io::{BufRead, BufReader, Write};
    for fail in [false, true] {
        let dir = tempfile::tempdir().unwrap();
        let rom = dir.path().join("original.ehdemo");
        fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
        for slot in 1..=10 {
            fs::write(
                dir.path().join(format!("state{slot}.state")),
                format!("manual slot {slot}"),
            )
            .unwrap();
        }
        if fail {
            fs::create_dir(dir.path().join("resume.state")).unwrap();
        }
        let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args([
                env!("EH_CORE_VALID"),
                rom.to_str().unwrap(),
                dir.path().join("native.srm").to_str().unwrap(),
                "0",
                "control",
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut pipe = child.stdin.take().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        let out = child.stdout.take().unwrap();
        thread::spawn(move || {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                let _ = tx.send(serde_json::from_str::<serde_json::Value>(&line).unwrap());
            }
        });
        let next = || {
            rx.recv_timeout(Duration::from_secs(10))
                .expect("worker response")
        };
        assert_eq!(next()["event"], "started");
        writeln!(pipe, "menu").unwrap();
        assert_eq!(next()["event"], "menu");
        writeln!(pipe, "save-close").unwrap();
        assert_eq!(next()["event"], if fail { "error" } else { "resume-saved" });
        if fail {
            assert!(
                child.try_wait().unwrap().is_none(),
                "Saving failed; the game must stay open"
            );
            writeln!(pipe, "resume").unwrap();
            assert_eq!(next()["event"], "resumed");
            writeln!(pipe, "quit").unwrap();
        }
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success());
                break;
            }
            if Instant::now() > deadline {
                let _ = child.kill();
                panic!("worker did not close");
            }
            thread::sleep(Duration::from_millis(20));
        }
        for slot in 1..=10 {
            assert_eq!(
                fs::read(dir.path().join(format!("state{slot}.state"))).unwrap(),
                format!("manual slot {slot}").as_bytes()
            );
        }
        if !fail {
            let header =
                emuhall_player_runtime::snapshots::inspect(&dir.path().join("resume.state"))
                    .unwrap();
            assert_eq!(header.size, 4);
            let restored = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
                .args([
                    env!("EH_CORE_VALID"),
                    rom.to_str().unwrap(),
                    dir.path().join("native.srm").to_str().unwrap(),
                    "2",
                    "headless",
                ])
                .env("EMUHALL_RESUME_CONTINUE", "1")
                .output()
                .unwrap();
            assert!(
                restored.status.success(),
                "{}",
                String::from_utf8_lossy(&restored.stderr)
            );
        }
    }
}

#[test]
fn n64_content_fingerprints_survive_random_file_names() {
    let dir = tempfile::tempdir().unwrap();
    let mut bytes = vec![0; 8192];
    bytes[..4].copy_from_slice(&[0x80, 0x37, 0x12, 0x40]);
    bytes[8..12].copy_from_slice(&0x80000400u32.to_be_bytes());
    let a = dir.path().join("Original fixture.z64");
    let b = dir.path().join("183496543187643318743.z64");
    fs::write(&a, &bytes).unwrap();
    fs::write(&b, &bytes).unwrap();
    let hash = |path: &std::path::Path| {
        let output = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args(["--ra-hash", "n64", path.to_str().unwrap()])
            .output()
            .unwrap();
        assert!(output.status.success());
        output.stdout
    };
    let fingerprint = hash(&a);
    assert_eq!(fingerprint.len(), 33);
    assert_eq!(fingerprint, hash(&b));
}
#[test]
fn files_written_beside_the_save_work_with_canonical_windows_paths() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    // On Windows this is a \\?\ path, where "/" is not accepted as a separator.
    let profile = fs::canonicalize(dir.path()).unwrap();
    let rom = profile.join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            profile.join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    lines.next().unwrap().unwrap();
    writeln!(stdin, "\u{feff}menu").unwrap();
    assert!(lines.next().unwrap().unwrap().contains("\"menu\""));
    writeln!(stdin, "save-state 4").unwrap();
    assert!(lines.next().unwrap().unwrap().contains("state-saved"));
    drop(stdin);
    assert!(child.wait().unwrap().success());
    assert!(profile.join("menu-frame.bmp").is_file());
    assert!(profile.join("state4.state").is_file());
}
#[test]
fn closing_the_launcher_pipe_ends_the_session_and_keeps_the_save() {
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .spawn()
        .unwrap();
    drop(child.stdin.take());
    assert!(child.wait().unwrap().success());
    assert_eq!(fs::read(dir.path().join("native.srm")).unwrap().len(), 32);
}
#[test]
fn native_save_survives_second_session_and_other_sizes_load_with_the_original_kept() {
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    let save = dir.path().join("native.srm");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    fs::write(&save, [42u8; 32]).unwrap();
    let invoke = || {
        Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args([
                env!("EH_CORE_VALID"),
                rom.to_str().unwrap(),
                save.to_str().unwrap(),
                "2",
                "headless",
            ])
            .output()
            .unwrap()
    };
    assert!(invoke().status.success());
    assert_eq!(fs::read(&save).unwrap(), [42; 32]);
    assert!(invoke().status.success());
    // Another engine padded the same save differently: the overlap loads and the original is kept as .bak.
    let mut padded = vec![7u8; 32];
    padded.extend([0u8; 96]);
    fs::write(&save, &padded).unwrap();
    assert!(invoke().status.success());
    assert_eq!(fs::read(&save).unwrap(), [7u8; 32]);
    assert_eq!(fs::read(save.with_extension("srm.bak")).unwrap(), padded);
    fs::write(&save, b"short").unwrap();
    assert!(invoke().status.success());
    assert_eq!(&fs::read(&save).unwrap()[..5], b"short");
}
#[test]
fn n64_achievements_read_words_and_expansion_ram_without_a_second_byte_swap() {
    assert!(Command::new(env!("EH_N64_ACHIEVEMENTS_TEST"))
        .status()
        .unwrap()
        .success());
}
#[test]
fn achievements_are_off_until_signed_in_and_unsupported_systems_say_so() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VALID"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let mut next =
        || -> serde_json::Value { serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap() };
    assert_eq!(next()["event"], "started");
    writeln!(stdin, "achievements").unwrap();
    assert_eq!(
        next(),
        serde_json::json!({"event":"ra-list","available":false})
    );
    writeln!(stdin, "ra-login emuhall-test 1 someone sometoken").unwrap();
    let status = next();
    assert_eq!(status["event"], "ra-status");
    assert!(status["message"]
        .as_str()
        .unwrap()
        .contains("does not support"));
    // Without a RetroAchievements session, hardcore restrictions do not apply.
    writeln!(stdin, "save-state 1").unwrap();
    assert_eq!(next()["event"], "state-saved");
    writeln!(stdin, "load-state 1").unwrap();
    assert_eq!(next()["event"], "state-loaded");
    drop(stdin);
    assert!(child.wait().unwrap().success());
}
#[test]
fn engines_drawing_with_the_graphics_card_reach_the_frame_buffer() {
    #[cfg(not(windows))]
    if std::env::var("EMUHALL_TEST_GL").as_deref() != Ok("1") {
        eprintln!("Hardware OpenGL test requires EMUHALL_TEST_GL=1 and a display");
        return;
    }
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_GL"),
            rom.to_str().unwrap(),
            dir.path().join("save.srm").to_str().unwrap(),
            "30",
            "headless",
        ])
        .output()
        .unwrap();
    let log = String::from_utf8_lossy(&output.stderr);
    if log.contains("is not available on this graphics card") || log.contains("no context") {
        eprintln!("skipped: this machine has no OpenGL 3.3 driver ({log})");
        return;
    }
    assert!(output.status.success(), "{log}");
    // The stats summary is the line with "frames"; an independent event (such as a controller announcement) may
    // also reach stdout first.
    let stdout = String::from_utf8_lossy(&output.stdout);
    let v: serde_json::Value = stdout
        .lines()
        .filter_map(|line| serde_json::from_str::<serde_json::Value>(line).ok())
        .find(|line| line.get("frames").is_some())
        .unwrap_or_else(|| panic!("No frame summary in: {stdout}"));
    // Up to three copies are in flight (frames are never waited for while the GPU keeps up): the last ones are still in
    // flight when the session ends, and each of the two layout changes discards the old-size copies.
    let delivered = v["video_frames"].as_u64().unwrap();
    assert!((21..=28).contains(&delivered), "{delivered} frames: {log}");
    assert_ne!(v["frame_hash"], 0);
}

#[cfg(windows)]
#[test]
fn native_game_picture_handles_space_pause_and_double_click_fullscreen() {
    native_picture_input_case(env!("EH_CORE_VALID"), None, (160, 120));
}

#[cfg(windows)]
#[test]
fn ds_touchscreen_double_click_preserves_play_without_toggling_fullscreen() {
    // Default stacked screens, side-by-side screens, and a smaller bottom screen with a gap.
    // Points are client coordinates after the actual native renderer has letterboxed the picture.
    native_picture_input_case(
        env!("EH_CORE_NDS"),
        Some(("", (160, 180), (20, 120))),
        (160, 60),
    );
    native_picture_input_case(
        env!("EH_CORE_NDS"),
        Some((
            "melonds_screen_layout1 = \"left-right\"\n",
            (240, 120),
            (20, 30),
        )),
        (80, 120),
    );
    native_picture_input_case(
        env!("EH_CORE_NDS"),
        Some((
            "melonds_screen_gap = \"32\"\nmelonds_secondary_screen_scale = \"50\"\n",
            (160, 204),
            (20, 120),
        )),
        (160, 72),
    );
}

#[cfg(windows)]
type DsPictureCase<'a> = (&'a str, (i32, i32), (i32, i32));

#[cfg(windows)]
fn native_picture_input_case(
    core: &str,
    ds: Option<DsPictureCase<'_>>,
    fullscreen_point: (i32, i32),
) {
    use std::{
        ffi::c_void,
        io::{BufRead, BufReader, Write},
        sync::mpsc,
    };
    type Hwnd = *mut c_void;
    #[repr(C)]
    struct Message {
        hwnd: Hwnd,
        message: u32,
        w: usize,
        l: isize,
        time: u32,
        x: i32,
        y: i32,
        private: u32,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn CreateWindowExW(
            ex: u32,
            class: *const u16,
            title: *const u16,
            style: u32,
            x: i32,
            y: i32,
            w: i32,
            h: i32,
            parent: Hwnd,
            menu: Hwnd,
            instance: Hwnd,
            param: Hwnd,
        ) -> Hwnd;
        fn DestroyWindow(hwnd: Hwnd) -> i32;
        fn MoveWindow(hwnd: Hwnd, x: i32, y: i32, w: i32, h: i32, repaint: i32) -> i32;
        fn EnumWindows(callback: unsafe extern "system" fn(Hwnd, isize) -> i32, arg: isize) -> i32;
        fn GetWindowThreadProcessId(hwnd: Hwnd, pid: *mut u32) -> u32;
        fn GetClassNameW(hwnd: Hwnd, name: *mut u16, length: i32) -> i32;
        fn SendMessageW(hwnd: Hwnd, message: u32, w: usize, l: isize) -> isize;
        fn PeekMessageW(message: *mut Message, hwnd: Hwnd, min: u32, max: u32, flags: u32) -> i32;
        fn TranslateMessage(message: *const Message) -> i32;
        fn DispatchMessageW(message: *const Message) -> isize;
    }
    fn pump() {
        unsafe {
            let mut message: Message = std::mem::zeroed();
            while PeekMessageW(&mut message, std::ptr::null_mut(), 0, 0, 1) != 0 {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    struct Parent(Hwnd);
    impl Drop for Parent {
        fn drop(&mut self) {
            unsafe {
                DestroyWindow(self.0);
            }
        }
    }
    struct Session(std::process::Child);
    impl Drop for Session {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    struct Search {
        pid: u32,
        found: Hwnd,
    }
    unsafe extern "system" fn find_worker(hwnd: Hwnd, arg: isize) -> i32 {
        let search = unsafe { &mut *(arg as *mut Search) };
        let mut pid = 0;
        unsafe {
            GetWindowThreadProcessId(hwnd, &mut pid);
        }
        let mut class = [0u16; 64];
        let len = unsafe { GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32) };
        if pid == search.pid && String::from_utf16_lossy(&class[..len as usize]) == "EmuHallPlay" {
            search.found = hwnd;
            0
        } else {
            1
        }
    }
    let class: Vec<u16> = "STATIC\0".encode_utf16().collect();
    // A non-activating, off-screen owner exercises the actual worker window without taking focus.
    let parent = Parent(unsafe {
        CreateWindowExW(
            0x08000080,
            class.as_ptr(),
            class.as_ptr(),
            0x80000000,
            -32000,
            -32000,
            320,
            240,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    });
    assert!(!parent.0.is_null());
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("demo.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0fixture").unwrap();
    if let Some((options, _, _)) = ds {
        fs::write(dir.path().join("core-options.cfg"), options).unwrap();
    }
    let mut session = Session(
        Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args([
                core,
                rom.to_str().unwrap(),
                dir.path().join("native.srm").to_str().unwrap(),
                "0",
                &format!("embed:{}", parent.0 as usize),
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    );
    let mut input = session.0.stdin.take().unwrap();
    let output = session.0.stdout.take().unwrap();
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        for line in BufReader::new(output).lines().map_while(Result::ok) {
            if let Ok(value) = serde_json::from_str::<serde_json::Value>(&line) {
                if tx.send(value).is_err() {
                    break;
                }
            }
        }
    });
    let next = || {
        let start = Instant::now();
        loop {
            pump();
            if let Ok(value) = rx.try_recv() {
                // SDL may announce devices before the game starts or while
                // commands are handled; that independent event is not a reply.
                if value["event"] == "controllers" {
                    continue;
                }
                break value;
            }
            assert!(
                // Generous: a busy machine (parallel builds or UI tests) can delay replies; success is unaffected.
                start.elapsed() < Duration::from_secs(15),
                "Worker did not respond"
            );
            thread::sleep(Duration::from_millis(5));
        }
    };
    assert_eq!(next()["event"], "started");
    let mut search = Search {
        pid: session.0.id(),
        found: std::ptr::null_mut(),
    };
    unsafe {
        EnumWindows(find_worker, &mut search as *mut Search as isize);
    }
    assert!(!search.found.is_null());
    thread::sleep(Duration::from_millis(100));
    if let Some((_, touch, bars)) = ds {
        for point in [touch, bars] {
            unsafe {
                SendMessageW(
                    search.found,
                    0x0203,
                    1,
                    ((point.1 as isize) << 16) | (point.0 as isize),
                );
            }
            // A pipe command is a barrier: any unintended fullscreen event would arrive before menu.
            writeln!(input, "menu").unwrap();
            assert_eq!(
                next()["event"],
                "menu",
                "Touchscreen and letterbox clicks must not toggle fullscreen"
            );
            writeln!(input, "resume").unwrap();
            assert_eq!(next()["event"], "resumed");
        }
    }
    unsafe {
        SendMessageW(
            search.found,
            0x0203, /* WM_LBUTTONDBLCLK */
            1,
            ((fullscreen_point.1 as isize) << 16) | (fullscreen_point.0 as isize),
        );
    }
    assert_eq!(next()["event"], "fullscreen-toggle");
    if let Some((config, touch, _)) = ds {
        // Simulate the launcher applying fullscreen, then double-click the touch
        // screen again. The late-option fixture follows real melonDS load timing.
        unsafe {
            MoveWindow(parent.0, 0, 0, 1280, 720, 1);
        }
        for _ in 0..30 {
            pump();
            thread::sleep(Duration::from_millis(10));
        }
        let point = if config.contains("left-right") {
            (960, 360)
        } else {
            (640, touch.1 * 3)
        };
        unsafe {
            SendMessageW(
                search.found,
                0x0203,
                1,
                ((point.1 as isize) << 16) | point.0,
            );
        }
        writeln!(input, "menu").unwrap();
        assert_eq!(
            next()["event"],
            "menu",
            "DS touch must not exit fullscreen after resizing"
        );
        writeln!(input, "resume").unwrap();
        assert_eq!(next()["event"], "resumed");
        unsafe {
            MoveWindow(parent.0, 0, 0, 320, 240, 1);
        }
        for _ in 0..20 {
            pump();
            thread::sleep(Duration::from_millis(10));
        }
    }
    unsafe {
        SendMessageW(
            search.found,
            0x0100, /* WM_KEYDOWN */
            0x20,   /* Space */
            1,
        );
    }
    assert_eq!(next()["event"], "menu");
    writeln!(input, "resume").unwrap();
    assert_eq!(next()["event"], "resumed");
    unsafe {
        SendMessageW(search.found, 0x0100, 0x20, 1 | (1 << 30));
    }
    assert!(
        rx.recv_timeout(Duration::from_millis(100)).is_err(),
        "A held key must not reopen the menu"
    );
    unsafe {
        SendMessageW(search.found, 0x0100, 0x1b /* Escape */, 1);
    }
    assert_eq!(next()["event"], "menu");
    writeln!(input, "quit").unwrap();
    assert!(next().get("event").is_none());
    let start = Instant::now();
    loop {
        pump();
        if let Some(status) = session.0.try_wait().unwrap() {
            assert!(status.success());
            break;
        }
        assert!(
            start.elapsed() < Duration::from_secs(15),
            "Worker did not close"
        );
        thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn variable_length_states_restore_using_the_stored_length() {
    use std::io::{BufRead, BufReader, Write};
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("original.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_VARIABLE_STATES"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    assert_eq!(
        serde_json::from_str::<serde_json::Value>(&lines.next().unwrap().unwrap()).unwrap()
            ["event"],
        "started"
    );
    for (command, event) in [
        ("menu", "menu"),
        ("save-state 1", "state-saved"),
        ("save-state 2", "state-saved"),
        ("load-state 1", "state-loaded"),
    ] {
        writeln!(stdin, "{command}").unwrap();
        stdin.flush().unwrap();
        let reply: serde_json::Value =
            serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap();
        assert_eq!(reply["event"], event, "{reply}");
    }
    assert_eq!(
        emuhall_player_runtime::snapshots::inspect(&dir.path().join("state1.state"))
            .unwrap()
            .size,
        4
    );
    assert_eq!(
        emuhall_player_runtime::snapshots::inspect(&dir.path().join("state2.state"))
            .unwrap()
            .size,
        8
    );
    writeln!(stdin, "quit").unwrap();
    stdin.flush().unwrap();
    assert!(child.wait().unwrap().success());
}

#[test]
fn native_fps_reports_core_frames_and_accepts_presentation_and_rumble_toggles() {
    use std::io::Write;
    let dir = tempfile::tempdir().unwrap();
    let rom = dir.path().join("original.ehdemo");
    fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
        .args([
            env!("EH_CORE_PROPERTIES"),
            rom.to_str().unwrap(),
            dir.path().join("native.srm").to_str().unwrap(),
            "0",
            "control",
        ])
        .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    {
        use std::io::{BufRead, BufReader};
        let mut reader = BufReader::new(child.stdout.take().unwrap());
        let mut first = String::new();
        reader.read_line(&mut first).unwrap();
        assert_eq!(
            serde_json::from_str::<serde_json::Value>(&first).unwrap()["event"],
            "started"
        );
        child.stdout = Some(reader.into_inner());
    }
    let mut pipe = child.stdin.take().unwrap();
    writeln!(
        pipe,
        "set fps on\nset rumble off\nset bezel console\nset filter warm\nproperties\nstart"
    )
    .unwrap();
    pipe.flush().unwrap();
    // Wait for the first once-a-second metrics report instead of sleeping a fixed time, so a busy machine
    // (parallel builds or UI tests) cannot fail the test.
    let (lines, received) = std::sync::mpsc::channel();
    let stdout = child.stdout.take().unwrap();
    let reader = thread::spawn(move || {
        use std::io::BufRead;
        for line in std::io::BufReader::new(stdout)
            .lines()
            .map_while(Result::ok)
        {
            let _ = lines.send(line);
        }
    });
    let deadline = std::time::Instant::now() + Duration::from_secs(6);
    let mut events: Vec<serde_json::Value> = Vec::new();
    let reported = |event: &serde_json::Value| {
        event["event"] == "metrics"
            && event["fps"]
                .as_f64()
                .is_some_and(|fps| fps.is_finite() && fps > 0.0)
    };
    while !events.iter().any(reported) {
        let left = deadline.saturating_duration_since(std::time::Instant::now());
        match received.recv_timeout(left) {
            Ok(line) => events.extend(serde_json::from_str(&line).ok()),
            Err(_) => break,
        }
    }
    writeln!(pipe, "set fps off\nset rumble on\nquit").unwrap();
    drop(pipe);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    events.extend(
        received
            .try_iter()
            .filter_map(|line| serde_json::from_str(&line).ok()),
    );
    assert!(events.iter().any(|event| event["event"] == "properties"));
    assert!(events.iter().any(|event| event["event"] == "metrics"
        && event["fps"]
            .as_f64()
            .is_some_and(|fps| fps.is_finite() && fps > 0.0)));
    assert!(!events.iter().any(|event| event["event"] == "error"));
}

#[test]
fn data_memory_cheats_reach_the_worker_with_platform_specific_byte_order() {
    use std::io::Write;
    for (system, kind, code) in [
        ("ps2", 11, "20000004 12345678"),
        ("n64", 10, "81000004 1234"),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let rom = dir.path().join("original.ehdemo");
        let save = dir.path().join("native.srm");
        fs::write(&rom, b"EMUHALL-DEMO\0original fixture").unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_emuhall-core-host"))
            .args([
                env!("EH_CORE_PROPERTIES"),
                rom.to_str().unwrap(),
                save.to_str().unwrap(),
                "4",
                "control",
            ])
            .env("EMUHALL_WAIT_FOR_LAUNCHER", "1")
            .env("EMUHALL_SYSTEM", system)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut pipe = child.stdin.take().unwrap();
        writeln!(pipe, "cheat-memory 0 {kind} {code}\nstart").unwrap();
        pipe.flush().unwrap();
        let output = child.wait_with_output().unwrap();
        drop(pipe);
        assert!(output.status.success());
        let bytes = fs::read(save).unwrap();
        if kind == 11 {
            assert_eq!(&bytes[4..8], &[0x78, 0x56, 0x34, 0x12]);
        } else if cfg!(target_endian = "little") {
            assert_eq!(&bytes[6..8], &[0x34, 0x12]);
        } else {
            assert_eq!(&bytes[4..6], &[0x12, 0x34]);
        }
    }
}
