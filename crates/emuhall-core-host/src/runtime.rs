//! Narrow C callbacks into the portable runtime. No SDL/Win32 types cross here.
use emuhall_player_runtime::{
    input::{Pad, Profile},
    snapshots::{self, Identity},
    timeline::{Snapshot, Timeline},
};
use std::{
    ffi::{c_char, CStr},
    path::{Path, PathBuf},
    sync::{Mutex, OnceLock},
};

static IDENTITY: OnceLock<Identity> = OnceLock::new();
static PROFILES: Mutex<Vec<Profile>> = Mutex::new(Vec::new());
static HISTORY: OnceLock<Mutex<Timeline>> = OnceLock::new();
fn history() -> &'static Mutex<Timeline> {
    HISTORY.get_or_init(|| Mutex::new(Timeline::new(64 * 1024 * 1024)))
}
pub fn initialize(core: &Path, content: &Path, save: &Path) -> anyhow::Result<()> {
    let identity = match std::env::var("EMUHALL_STATE_IDENTITY") {
        Ok(json) => serde_json::from_str::<Identity>(&json)?,
        Err(_) => Identity::local(
            snapshots::file_digest(content)?,
            snapshots::file_digest(core)?,
            snapshots::digest(
                &std::fs::read(
                    save.parent()
                        .unwrap_or(Path::new("."))
                        .join("core-options.cfg"),
                )
                .unwrap_or_default(),
            ),
        ),
    };
    identity.validate()?;
    IDENTITY
        .set(identity)
        .map_err(|_| anyhow::anyhow!("Runtime identity already initialized"))?;
    initialize_profiles()
}
pub fn initialize_profiles() -> anyhow::Result<()> {
    if let Ok(raw) = std::env::var("EMUHALL_INPUT_PROFILES") {
        let profiles: Vec<Profile> = serde_json::from_str(&raw)?;
        emuhall_player_runtime::input::validate_profiles(&profiles)?;
        *PROFILES.lock().unwrap() = profiles;
    }
    Ok(())
}
#[no_mangle]
unsafe extern "C" fn eh_controller_key(
    guid: *const c_char,
    serial: *const c_char,
    ordinal: u32,
    out: *mut c_char,
    cap: usize,
) {
    if guid.is_null() || out.is_null() || cap < 2 {
        return;
    }
    let guid = CStr::from_ptr(guid).to_string_lossy();
    let serial = if serial.is_null() {
        None
    } else {
        CStr::from_ptr(serial)
            .to_str()
            .ok()
            .filter(|s| !s.is_empty())
    };
    let key = if let Some(serial) = serial {
        format!(
            "{guid}:{}:{ordinal}",
            &snapshots::digest(serial.as_bytes())[..16]
        )
    } else {
        format!("{guid}:{ordinal}")
    };
    if key.len() >= cap {
        *out = 0;
        return;
    }
    std::ptr::copy_nonoverlapping(key.as_ptr(), out.cast(), key.len());
    *out.add(key.len()) = 0;
}
#[no_mangle]
unsafe extern "C" fn eh_controller_assignment(id: *const c_char) -> i32 {
    if id.is_null() {
        return -1;
    }
    let Ok(id) = CStr::from_ptr(id).to_str() else {
        return -1;
    };
    PROFILES
        .lock()
        .unwrap()
        .iter()
        .find(|p| p.id == id)
        .map_or(-2, |p| {
            p.player
                .map_or(-1, |n| if n == 0 { -2 } else { n as i32 - 1 })
        })
}
unsafe fn path(ptr: *const c_char) -> Option<PathBuf> {
    if ptr.is_null() {
        None
    } else {
        CStr::from_ptr(ptr).to_str().ok().map(PathBuf::from)
    }
}
#[no_mangle]
unsafe extern "C" fn eh_atomic_write(
    file: *const c_char,
    bytes: *const u8,
    len: usize,
    backup: i32,
) -> i32 {
    if bytes.is_null() || len > snapshots::MAX_STATE {
        return 0;
    }
    let Some(file) = path(file) else {
        return 0;
    };
    match snapshots::atomic_write(&file, std::slice::from_raw_parts(bytes, len), backup != 0) {
        Ok(()) => 1,
        Err(e) => {
            eprintln!("Save commit: {e}");
            0
        }
    }
}
#[no_mangle]
unsafe extern "C" fn eh_snapshot_save(file: *const c_char, bytes: *const u8, len: usize) -> i32 {
    if bytes.is_null() || len == 0 || len > snapshots::MAX_STATE {
        return 0;
    }
    let Some(file) = path(file) else {
        return 0;
    };
    let Some(id) = IDENTITY.get() else {
        return 0;
    };
    match snapshots::save(&file, id, std::slice::from_raw_parts(bytes, len)) {
        Ok(()) => 1,
        Err(e) => {
            eprintln!("Snapshot save: {e}");
            0
        }
    }
}
#[no_mangle]
unsafe extern "C" fn eh_snapshot_size(file: *const c_char) -> usize {
    let Some(file) = path(file) else {
        return 0;
    };
    let Some(id) = IDENTITY.get() else {
        return 0;
    };
    snapshots::inspect(&file)
        .ok()
        .filter(|h| h.identity.restores(id))
        .map_or(0, |h| h.size)
}
/// 1 when a restorable state was saved under different engine settings: the likely reason an engine refuses it.
#[no_mangle]
unsafe extern "C" fn eh_snapshot_settings_differ(file: *const c_char) -> i32 {
    let (Some(file), Some(id)) = (path(file), IDENTITY.get()) else {
        return 0;
    };
    snapshots::inspect(&file)
        .is_ok_and(|h| h.identity.restores(id) && h.identity.options != id.options) as i32
}
#[no_mangle]
unsafe extern "C" fn eh_snapshot_load(
    file: *const c_char,
    bytes: *mut u8,
    capacity: usize,
) -> usize {
    if bytes.is_null() || capacity > snapshots::MAX_STATE {
        return 0;
    }
    let Some(file) = path(file) else {
        return 0;
    };
    let Some(id) = IDENTITY.get() else {
        return 0;
    };
    match snapshots::load_into(&file, id, std::slice::from_raw_parts_mut(bytes, capacity)) {
        Ok(len) => len,
        Err(e) => {
            eprintln!("Snapshot restore: {e}");
            0
        }
    }
}
#[no_mangle]
unsafe extern "C" fn eh_controller_profile(raw: *const c_char) -> i32 {
    if raw.is_null() {
        return 0;
    }
    let Ok(raw) = CStr::from_ptr(raw).to_str() else {
        return 0;
    };
    if raw.len() > 1024 {
        return 0;
    }
    let Ok(profile) = serde_json::from_str::<Profile>(raw) else {
        return 0;
    };
    if profile.validate().is_err() {
        return 0;
    }
    let mut profiles = PROFILES.lock().unwrap();
    let mut next = profiles.clone();
    if let Some(old) = next.iter_mut().find(|p| p.id == profile.id) {
        *old = profile;
    } else {
        next.push(profile);
    }
    if emuhall_player_runtime::input::validate_profiles(&next).is_err() {
        return 0;
    }
    *profiles = next;
    1
}
#[no_mangle]
unsafe extern "C" fn eh_controller_forget(id: *const c_char) {
    if !id.is_null() {
        if let Ok(id) = CStr::from_ptr(id).to_str() {
            PROFILES.lock().unwrap().retain(|p| p.id != id);
        }
    }
}
#[no_mangle]
unsafe extern "C" fn eh_controller_route(
    id: *const c_char,
    default_port: u32,
    pad: *mut Pad,
) -> i32 {
    if id.is_null() || pad.is_null() {
        return -1;
    }
    let Ok(id) = CStr::from_ptr(id).to_str() else {
        return -1;
    };
    let profiles = PROFILES.lock().unwrap();
    if let Some(profile) = profiles.iter().find(|p| p.id == id) {
        *pad = profile.apply(*pad);
        profile.player.map_or(-1, |p| {
            if p == 0 && default_port < 4 {
                default_port as i32
            } else {
                p as i32 - 1
            }
        })
    } else {
        ((*pad).lx, (*pad).ly) = emuhall_player_runtime::input::deadzone((*pad).lx, (*pad).ly, 24);
        ((*pad).rx, (*pad).ry) = emuhall_player_runtime::input::deadzone((*pad).rx, (*pad).ry, 26);
        if default_port < 4 {
            default_port as i32
        } else {
            -1
        }
    }
}
#[no_mangle]
unsafe extern "C" fn eh_history_push(
    bytes: *const u8,
    len: usize,
    pixels: *const u32,
    w: u32,
    h: u32,
) -> i32 {
    if bytes.is_null()
        || pixels.is_null()
        || len == 0
        || len > 32 * 1024 * 1024
        || w == 0
        || h == 0
        || w > 2048
        || h > 2048
    {
        return 0;
    }
    history().lock().unwrap().push(Snapshot {
        state: std::slice::from_raw_parts(bytes, len).to_vec(),
        pixels: std::slice::from_raw_parts(pixels, (w * h) as usize).to_vec(),
        width: w,
        height: h,
    }) as i32
}
#[no_mangle]
extern "C" fn eh_history_clear() {
    history().lock().unwrap().clear();
}
#[no_mangle]
extern "C" fn eh_history_size() -> usize {
    history().lock().unwrap().count()
}
// Two-stage pop keeps ownership in Rust until the C bridge has allocated enough space.
static PENDING: Mutex<Option<Snapshot>> = Mutex::new(None);
#[no_mangle]
extern "C" fn eh_history_back() -> usize {
    let item = history().lock().unwrap().back();
    let len = item.as_ref().map_or(0, |s| s.state.len());
    *PENDING.lock().unwrap() = item;
    len
}
#[no_mangle]
unsafe extern "C" fn eh_history_copy(
    bytes: *mut u8,
    len: usize,
    pixels: *mut u32,
    w: *mut u32,
    h: *mut u32,
) -> i32 {
    if bytes.is_null() || pixels.is_null() || w.is_null() || h.is_null() {
        return 0;
    }
    let Some(item) = PENDING.lock().unwrap().take() else {
        return 0;
    };
    if len != item.state.len() {
        return 0;
    }
    std::ptr::copy_nonoverlapping(item.state.as_ptr(), bytes, len);
    std::ptr::copy_nonoverlapping(item.pixels.as_ptr(), pixels, item.pixels.len());
    *w = item.width;
    *h = item.height;
    1
}

#[no_mangle]
unsafe extern "C" fn eh_video_filter(pixels: *mut u32, w: u32, h: u32, mode: u32) -> i32 {
    if pixels.is_null() || w == 0 || h == 0 || w > 2048 || h > 2048 {
        return 0;
    }
    emuhall_player_runtime::video::filter(
        std::slice::from_raw_parts_mut(pixels, (w * h) as usize),
        w as usize,
        h as usize,
        mode,
    ) as i32
}

#[no_mangle]
unsafe extern "C" fn eh_snapshot_legacy_size(file: *const c_char) -> usize {
    path(file)
        .and_then(|p| snapshots::legacy_size(&p).ok())
        .unwrap_or(0)
}
#[no_mangle]
unsafe extern "C" fn eh_snapshot_legacy_load(
    file: *const c_char,
    bytes: *mut u8,
    capacity: usize,
) -> usize {
    if bytes.is_null() || capacity > snapshots::MAX_STATE {
        return 0;
    }
    let Some(file) = path(file) else {
        return 0;
    };
    snapshots::load_legacy_into(&file, std::slice::from_raw_parts_mut(bytes, capacity)).unwrap_or(0)
}

static MEMORY_CHEATS_ACTIVE: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);
#[no_mangle]
pub extern "C" fn eh_memory_cheat_active() -> i32 {
    MEMORY_CHEATS_ACTIVE.load(std::sync::atomic::Ordering::Relaxed) as i32
}
static MEMORY_CHEATS: Mutex<Vec<Option<emuhall_player_runtime::cheats::Program>>> =
    Mutex::new(Vec::new());
#[no_mangle]
pub unsafe extern "C" fn eh_memory_cheat_set(
    index: u32,
    kind: u32,
    code: *const c_char,
    size: usize,
) -> i32 {
    if index >= 32 || code.is_null() {
        return 0;
    }
    let Ok(code) = CStr::from_ptr(code).to_str() else {
        return 0;
    };
    let Ok(program) = emuhall_player_runtime::cheats::Program::parse(kind, code) else {
        return 0;
    };
    if !program.fits(size) {
        return 0;
    }
    let mut list = MEMORY_CHEATS.lock().unwrap();
    list.resize_with(32, || None);
    list[index as usize] = Some(program);
    MEMORY_CHEATS_ACTIVE.store(true, std::sync::atomic::Ordering::Relaxed);
    1
}
#[no_mangle]
pub extern "C" fn eh_memory_cheat_reset() {
    MEMORY_CHEATS_ACTIVE.store(false, std::sync::atomic::Ordering::Relaxed);
    MEMORY_CHEATS.lock().unwrap().clear();
}
#[no_mangle]
pub unsafe extern "C" fn eh_memory_cheat_apply(pointer: *mut u8, size: usize) {
    if pointer.is_null() || size == 0 || size > 512 * 1024 * 1024 {
        return;
    }
    let list = MEMORY_CHEATS.lock().unwrap();
    if list.is_empty() {
        return;
    }
    let ram = std::slice::from_raw_parts_mut(pointer, size);
    for program in list.iter().flatten() {
        program.apply(ram);
    }
}
