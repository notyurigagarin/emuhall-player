use std::{env, path::PathBuf};
fn main() {
    let include = "../../third-party/libretro";
    let rcheevos = "../../third-party/rcheevos";
    let sdl3 = "../../third-party/sdl3";
    let mut build = cc::Build::new();
    build
        .file("native/host.c")
        .file("native/citra_content.c")
        .file("native/desktop.c")
        .file("native/achievements.c")
        .file("native/gamepad.c")
        .file("native/glhost.c")
        .file("native/d3dhost.c")
        .file("native/timestretch.c")
        .file("native/vkhost.c")
        .file("native/shaders.c")
        .include(include)
        .include(format!("{sdl3}/include"))
        .include("../../third-party/vulkan-headers/include")
        .include(format!("{rcheevos}/include"))
        .include(format!("{rcheevos}/src"))
        .define("RC_CLIENT_SUPPORTS_HASH", None)
        .define("RC_HASH_NO_ENCRYPTED", None)
        .flag_if_supported("-std=c11");
    let compiler = build.get_compiler();
    build.compile("emuhall_host");
    // The official RetroAchievements client, vendored unchanged (third-party/rcheevos/EMUHALL-PROVENANCE.md).
    // Upstream code is built without our warning flags.
    let mut client = cc::Build::new();
    client
        .include(include)
        .include(format!("{rcheevos}/include"))
        .define("RC_CLIENT_SUPPORTS_HASH", None)
        .define("RC_HASH_NO_ENCRYPTED", None)
        .define("NDEBUG", None)
        .warnings(false);
    let sources =
        std::fs::read_to_string(format!("{rcheevos}/emuhall-generated/sources.txt")).unwrap();
    for source in sources.split_whitespace() {
        client.file(format!("{rcheevos}/{source}"));
    }
    client.compile("rcheevos");
    println!("cargo:rerun-if-changed=native/achievements.c");
    println!("cargo:rerun-if-changed=native/achievements.h");
    println!("cargo:rerun-if-changed=native/ra_system.h");
    println!("cargo:rerun-if-changed=native/desktop.c");
    println!("cargo:rerun-if-changed=native/desktop.h");
    println!("cargo:rerun-if-changed=native/runtime.h");
    println!("cargo:rerun-if-changed=native/gamepad.c");
    println!("cargo:rerun-if-changed=native/gamepad.h");
    println!("cargo:rerun-if-changed=native/glhost.c");
    println!("cargo:rerun-if-changed=native/glhost.h");
    println!("cargo:rerun-if-changed=native/d3dhost.c");
    println!("cargo:rerun-if-changed=native/d3dhost.h");
    println!("cargo:rerun-if-changed=native/timestretch.c");
    println!("cargo:rerun-if-changed=native/timestretch.h");
    println!("cargo:rerun-if-changed=native/audio_pacing.h");
    println!("cargo:rerun-if-changed=native/picture_layout.h");
    println!("cargo:rerun-if-changed=native/vkhost.c");
    println!("cargo:rerun-if-changed=native/vkhost.h");
    println!("cargo:rerun-if-changed=native/shaders.c");
    println!("cargo:rerun-if-changed=native/shaders.h");
    println!("cargo:rerun-if-changed={rcheevos}/emuhall-generated/sources.txt");
    let target = env::var("CARGO_CFG_TARGET_OS").unwrap();
    if target == "windows" {
        for lib in ["user32", "gdi32", "winmm", "dwmapi", "winhttp", "opengl32"] {
            println!("cargo:rustc-link-lib={lib}");
        }
    } else if target == "linux" {
        println!("cargo:rustc-link-lib=dl");
        println!("cargo:rustc-link-lib=pthread");
        // The bundled libSDL3.so.0 sits beside the player (portable folder, AppImage) or in the package's
        // library folder (/usr/lib/EmuHall for .deb and .rpm); dlopen searches both through this rpath.
        println!("cargo:rustc-link-arg-bins=-Wl,--disable-new-dtags,-rpath,$ORIGIN:$ORIGIN/../lib/EmuHall");
    }
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    // Original EmuHall branding on the worker's Windows executable (including firewall prompts).
    if target == "windows" && compiler.is_like_gnu() {
        let icon = std::fs::canonicalize("../../apps/desktop/src-tauri/icons/icon.ico").unwrap();
        let rc = out.join("EmuHall.Player.rc");
        let object = out.join("EmuHall.Player.resources.o");
        let text = format!(
            r#"1 ICON "{}"
1 VERSIONINFO
FILEVERSION {numbers}
PRODUCTVERSION {numbers}
FILEOS 0x40004
FILETYPE 1
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "EmuHall\0"
   VALUE "FileDescription", "EmuHall Player\0"
   VALUE "ProductName", "EmuHall\0"
   VALUE "FileVersion", "{version}\0"
   VALUE "OriginalFilename", "emuhall-core-host.exe\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"#,
            icon.to_string_lossy().replace('\\', "/"),
            // The workspace version, so the player's file properties match the release.
            numbers = env::var("CARGO_PKG_VERSION").unwrap().replace('.', ",") + ",0",
            version = env::var("CARGO_PKG_VERSION").unwrap(),
        );
        std::fs::write(&rc, text).unwrap();
        let windres = compiler.path().parent().unwrap().join("windres.exe");
        assert!(std::process::Command::new(windres)
            .args(["-i"])
            .arg(rc)
            .arg("-o")
            .arg(&object)
            .status()
            .expect("compile EmuHall resource")
            .success());
        println!("cargo:rustc-link-arg={}", object.display());
        println!("cargo:rerun-if-changed=../../apps/desktop/src-tauri/icons/icon.ico");
    }
    // The player loads SDL3.dll (controllers) from its own folder; put it beside the built player.
    if target == "windows" {
        if let Some(profile_dir) = out.ancestors().nth(3) {
            let _ = std::fs::copy(format!("{sdl3}/bin/SDL3.dll"), profile_dir.join("SDL3.dll"));
        }
        println!("cargo:rerun-if-changed={sdl3}/bin/SDL3.dll");
    }
    for (name, define) in [
        ("valid", "EH_VALID"),
        ("temporal", "EH_TEMPORAL"),
        ("temporal_fail", "EH_TEMPORAL_FAIL"),
        ("properties", "EH_PROPERTIES"),
        ("bad_api", "EH_BAD_API"),
        ("load_fail", "EH_LOAD_FAIL"),
        ("crash", "EH_CRASH"),
        ("hang", "EH_HANG"),
        ("bad_geometry", "EH_BAD_GEOMETRY"),
        ("dir_probe", "EH_DIR_PROBE"),
        ("gl", "EH_GL"),
        ("nds", "EH_NDS"),
        ("dynamic_save", "EH_DYNAMIC_SAVE"),
        ("layout_geometry", "EH_LAYOUT_GEOMETRY"),
        ("variable_states", "EH_VARIABLE_STATES"),
        ("late_cheats", "EH_LATE_CHEATS"),
        ("dualshock", "EH_DUALSHOCK"),
    ] {
        let dest = out.join(format!(
            "{name}.{}",
            if target == "windows" {
                "dll"
            } else if target == "macos" {
                "dylib"
            } else {
                "so"
            }
        ));
        let mut cmd = compiler.to_command();
        if compiler.is_like_msvc() {
            cmd.args(["/LD", "../../tests/libretro-fake-cores/fake.c"])
                .arg(format!("/I{include}"))
                .arg(format!("/D{define}"))
                .arg("/link")
                .arg(format!("/OUT:{}", dest.display()));
        } else {
            cmd.args([
                "-shared",
                "-fPIC",
                "-std=c11",
                "../../tests/libretro-fake-cores/fake.c",
                "-I",
                include,
            ])
            .arg(format!("-D{define}"))
            .arg("-o")
            .arg(&dest);
        }
        assert!(cmd.status().expect("compile fixture core").success());
        println!(
            "cargo:rustc-env=EH_CORE_{}={}",
            name.to_uppercase(),
            dest.display()
        );
    }
    println!("cargo:rerun-if-changed=native/host.c");
    println!("cargo:rerun-if-changed=native/citra_content.c");
    let audio_test = out.join(if target == "windows" {
        "audio-pacing.exe"
    } else {
        "audio-pacing"
    });
    let mut audio_command = compiler.to_command();
    if compiler.is_like_msvc() {
        audio_command
            .arg("../../tests/libretro-fake-cores/audio-pacing.c")
            .arg("native/timestretch.c")
            .arg(format!("/Fo{}\\", out.display()))
            .arg(format!("/Fe{}", audio_test.display()));
    } else {
        audio_command
            .args([
                "-std=c11",
                "../../tests/libretro-fake-cores/audio-pacing.c",
                "native/timestretch.c",
                "-o",
            ])
            .arg(&audio_test);
        audio_command.arg("-lm");
    }
    assert!(audio_command
        .status()
        .expect("compile synthetic audio pacing check")
        .success());
    println!(
        "cargo:rustc-env=EH_AUDIO_PACING_TEST={}",
        audio_test.display()
    );
    println!("cargo:rerun-if-changed=../../tests/libretro-fake-cores/audio-pacing.c");
    let ra_test = out.join(if target == "windows" {
        "n64-achievements.exe"
    } else {
        "n64-achievements"
    });
    let mut ra_command = compiler.to_command();
    if compiler.is_like_msvc() {
        ra_command
            .args([
                "../../tests/libretro-fake-cores/n64-achievements.c",
                "/I../../third-party/libretro",
                "/I../../third-party/rcheevos/include",
                "/I../../third-party/rcheevos/src",
            ])
            .arg(format!("/Fo{}\\", out.display()))
            .arg(format!("/Fe{}", ra_test.display()))
            .arg(out.join("rcheevos.lib"));
    } else {
        ra_command
            .args([
                "-std=c11",
                "../../tests/libretro-fake-cores/n64-achievements.c",
                "-I../../third-party/libretro",
                "-I../../third-party/rcheevos/include",
                "-I../../third-party/rcheevos/src",
                "-o",
            ])
            .arg(&ra_test)
            .arg(out.join("librcheevos.a"))
            .arg("-lm");
    }
    assert!(ra_command
        .status()
        .expect("compile N64 achievement memory check")
        .success());
    println!(
        "cargo:rustc-env=EH_N64_ACHIEVEMENTS_TEST={}",
        ra_test.display()
    );
    println!("cargo:rerun-if-changed=../../tests/libretro-fake-cores/n64-achievements.c");
    if target == "windows" {
        let presentation_test = out.join("d3d-presentation.exe");
        let mut command = compiler.to_command();
        if compiler.is_like_msvc() {
            command
                .args([
                    "/I../../third-party/libretro",
                    "../../tests/libretro-fake-cores/d3d-presentation.c",
                    "native/d3dhost.c",
                ])
                .arg(format!("/Fo{}\\", out.display()))
                .arg(format!("/Fe{}", presentation_test.display()))
                .args(["/link", "user32.lib", "gdi32.lib"]);
        } else {
            command
                .args([
                    "-std=c11",
                    "-I../../third-party/libretro",
                    "../../tests/libretro-fake-cores/d3d-presentation.c",
                    "native/d3dhost.c",
                    "-o",
                ])
                .arg(&presentation_test)
                .args(["-luser32", "-lgdi32", "-lm"]);
        }
        assert!(command
            .status()
            .expect("compile synthetic presentation check")
            .success());
        println!(
            "cargo:rustc-env=EH_D3D_PRESENTATION_TEST={}",
            presentation_test.display()
        );
    }
    println!("cargo:rerun-if-changed=../../tests/libretro-fake-cores/d3d-presentation.c");
    println!("cargo:rerun-if-changed=../../tests/libretro-fake-cores/fake.c");
    println!("cargo:rerun-if-changed=../../third-party/libretro/libretro.h");
}
