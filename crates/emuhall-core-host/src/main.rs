mod content_identity;
mod netplay;
mod runtime;
use std::ffi::{c_char, c_int, CString};
extern "C" {
    fn eh_citra_content(
        core: *const c_char,
        user: *const c_char,
        input: *const c_char,
        mode: c_int,
        id: *mut u64,
        title: *mut c_char,
        title_cap: usize,
        launch: *mut c_char,
        launch_cap: usize,
        folder: *mut c_char,
        folder_cap: usize,
    ) -> c_int;
    fn eh_pad_read(pad: *mut emuhall_player_runtime::input::Pad);
    fn eh_pad_read_port(port: u32, pad: *mut emuhall_player_runtime::input::Pad);
    fn eh_pad_list();
    fn eh_pad_gestures() -> u32;
    fn eh_desktop_smoke() -> c_int;
    fn eh_shader_smoke() -> c_int;
    fn eh_pad_init() -> c_int;
    fn eh_pad_probe();
    fn eh_pad_shutdown();
    fn eh_ra_hash(system: *const c_char, path: *const c_char, out: *mut c_char) -> c_int;
    fn eh_run(
        core: *const c_char,
        content: *const c_char,
        save: *const c_char,
        frames: u32,
        mode: c_int,
        parent: usize,
    ) -> c_int;
}
fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() == 4 && args[1] == "--identify-n64" {
        let result = (|| -> anyhow::Result<serde_json::Value> {
            let system = CString::new("n64")?;
            let path = CString::new(args[2].as_str())?;
            let mut hash = [0 as c_char; 33];
            anyhow::ensure!(
                unsafe { eh_ra_hash(system.as_ptr(), path.as_ptr(), hash.as_mut_ptr()) } != 0,
                "Cannot fingerprint this N64 file"
            );
            let hash = unsafe { std::ffi::CStr::from_ptr(hash.as_ptr()) }.to_str()?;
            let title = content_identity::n64_title(std::path::Path::new(&args[3]), hash)?;
            Ok(serde_json::json!({"title":title,"source":"mupen64plus-rom-catalog"}))
        })();
        match result {
            Ok(value) => println!("{value}"),
            Err(error) => {
                eprintln!("{error}");
                std::process::exit(1);
            }
        }
        return;
    }
    if args.len() == 6 && args[1] == "--3ds-content" {
        let result = (|| -> anyhow::Result<serde_json::Value> {
            let core = CString::new(args[2].as_str())?;
            let user = CString::new(args[3].as_str())?;
            let input = CString::new(args[4].as_str())?;
            let mode: i32 = args[5].parse()?;
            let (mut id, mut title, mut launch, mut folder) =
                (0u64, [0; 1024], [0; 32768], [0; 32768]);
            let status = unsafe {
                eh_citra_content(
                    core.as_ptr(),
                    user.as_ptr(),
                    input.as_ptr(),
                    mode,
                    &mut id,
                    title.as_mut_ptr(),
                    title.len(),
                    launch.as_mut_ptr(),
                    launch.len(),
                    folder.as_mut_ptr(),
                    folder.len(),
                )
            };
            let text = |bytes: &[c_char]| {
                unsafe { std::ffi::CStr::from_ptr(bytes.as_ptr()) }
                    .to_string_lossy()
                    .to_string()
            };
            Ok(
                serde_json::json!({"status":status,"titleId":format!("{id:016x}"),"title":text(&title),"launch":text(&launch),"folder":text(&folder)}),
            )
        })();
        match result {
            Ok(value) => println!("{value}"),
            Err(error) => {
                eprintln!("{error}");
                std::process::exit(2);
            }
        }
        return;
    }
    if args.len() == 2 && args[1] == "--input-test" {
        use std::{
            io::Read,
            sync::{
                atomic::{AtomicBool, Ordering},
                Arc,
            },
            time::{Duration, Instant},
        };
        if let Err(error) = runtime::initialize_profiles() {
            eprintln!("{error}");
            std::process::exit(2);
        }
        let alive = Arc::new(AtomicBool::new(true));
        let reader = alive.clone();
        std::thread::spawn(move || {
            let _ = std::io::stdin().read(&mut [0]);
            reader.store(false, Ordering::SeqCst);
        });
        unsafe {
            eh_pad_init();
            eh_pad_probe();
        }
        let start = Instant::now();
        let mut tick = 0;
        while alive.load(Ordering::SeqCst) && start.elapsed() < Duration::from_secs(120) {
            let mut first = emuhall_player_runtime::input::Pad::default();
            unsafe {
                eh_pad_read(&mut first);
            }
            if tick % 30 == 0 {
                unsafe {
                    eh_pad_list();
                }
            }
            let ports: Vec<_> = (0..4)
                .map(|port| {
                    let mut pad = emuhall_player_runtime::input::Pad::default();
                    unsafe {
                        eh_pad_read_port(port, &mut pad);
                    }
                    serde_json::json!({"buttons":pad.buttons,"axes":[pad.lx,pad.ly,pad.rx,pad.ry]})
                })
                .collect();
            println!(
                "{}",
                serde_json::json!({"event":"controller-input","ports":ports,"gestures":unsafe {eh_pad_gestures()}})
            );
            tick += 1;
            std::thread::sleep(Duration::from_millis(16));
        }
        unsafe {
            eh_pad_shutdown();
        }
        return;
    }
    if args.len() == 3 && args[1] == "--relay-listen" {
        let result = args[2]
            .parse()
            .map_err(anyhow::Error::from)
            .and_then(|address| {
                emuhall_player_runtime::relay::serve(
                    address,
                    std::env::var("EMUHALL_RELAY_SIGNING_KEY")?,
                )
                .map_err(anyhow::Error::from)
            });
        if let Err(error) = result {
            eprintln!("Relay: {error}");
            std::process::exit(2);
        }
        return;
    }
    if args.len() == 2 && args[1] == "--shader-smoke" {
        std::process::exit(if unsafe { eh_shader_smoke() } != 0 {
            0
        } else {
            1
        });
    }
    if args.len() == 2 && args[1] == "--sdl-smoke" {
        std::process::exit(if unsafe { eh_desktop_smoke() } != 0 {
            0
        } else {
            1
        });
    }
    if args.len() == 2 && args[1] == "--controllers" {
        if let Err(error) = runtime::initialize_profiles() {
            eprintln!("Controller profiles: {error}");
            std::process::exit(2);
        }
        unsafe {
            eh_pad_init();
            eh_pad_probe();
            eh_pad_shutdown();
        }
        return;
    }
    // "--ra-hash <system> <file>": prints the RetroAchievements fingerprint so the library can identify the game.
    if args.len() == 4 && args[1] == "--ra-hash" {
        let (system, path) = (
            CString::new(args[2].as_str()),
            CString::new(args[3].as_str()),
        );
        let mut out = [0 as c_char; 33];
        let ok = match (system, path) {
            (Ok(system), Ok(path)) => unsafe {
                eh_ra_hash(system.as_ptr(), path.as_ptr(), out.as_mut_ptr()) != 0
            },
            _ => false,
        };
        if !ok {
            std::process::exit(1);
        }
        println!(
            "{}",
            unsafe { std::ffi::CStr::from_ptr(out.as_ptr()) }.to_string_lossy()
        );
        return;
    }
    if args.len() == 3 && args[1] == "--export-test-core" {
        std::fs::copy(env!("EH_CORE_VALID"), &args[2]).expect("copy test engine");
        return;
    }
    if args.len() != 6 {
        eprintln!(
            "Usage: emuhall-core-host CORE CONTENT SAVE FRAMES headless|window|control|embed:HWND"
        );
        std::process::exit(2);
    }
    let frames = match args[4].parse::<u32>() {
        Ok(n) if n <= 10_000_000 => n,
        _ => {
            eprintln!("Invalid frame limit");
            std::process::exit(2)
        }
    };
    // `control` and `embed:` read launcher commands from stdin and report events on stdout.
    let (mode, parent) = match args[5].as_str() {
        "headless" => (0, 0),
        "window" => (1, 0),
        "control" => (3, 0),
        "inspect" => (4, 0),
        other => match other
            .strip_prefix("embed:")
            .and_then(|h| h.parse::<usize>().ok())
        {
            Some(parent) if parent != 0 => (2, parent),
            _ => std::process::exit(2),
        },
    };
    if mode != 4 {
        if let Err(error) = runtime::initialize(
            std::path::Path::new(&args[1]),
            std::path::Path::new(&args[2]),
            std::path::Path::new(&args[3]),
        ) {
            eprintln!("Cannot prepare runtime: {error}");
            std::process::exit(2);
        }
    }
    let core = CString::new(args[1].as_str()).unwrap();
    let content = CString::new(args[2].as_str()).unwrap();
    let save = CString::new(args[3].as_str()).unwrap();
    // Only our narrow bridge crosses Rust FFI. All Libretro types come from the pinned upstream header.
    let code = unsafe {
        eh_run(
            core.as_ptr(),
            content.as_ptr(),
            save.as_ptr(),
            frames,
            mode,
            parent,
        )
    };
    std::process::exit(code);
}
