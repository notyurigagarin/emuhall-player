use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Seek, Write},
    path::Path,
};

const MAGIC: &[u8; 8] = b"EHSAVE01";
pub const MAX_STATE: usize = 256 * 1024 * 1024;
const MAX_HEADER: usize = 4096;

// Reviewed Windows Mupen build transition: only GLSM texture-deletion bookkeeping
// changed. The CPU/RAM serializer is unchanged; an old saved scene was restored,
// paused/unpaused, and re-saved with the fixed build. No version-wide bypass.
const MUPEN_BEFORE_TEXTURE_FIX: &str =
    "11f7116285e60c961650f855ad108634cf7cd3445b83372d65f1dd29e85563c2";
const MUPEN_AFTER_TEXTURE_FIX: &str =
    "21f97e4e7e123e1c698c1e3322153ccd009933b9badd0ccb107d5a26f79b8c90";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Identity {
    pub game: String,
    pub core: String,
    pub options: String,
    pub target: String,
}
impl Identity {
    /// Whether a state saved under `self` may be restored by `running`: same game bytes, exact engine build
    /// (or the one reviewed graphics-only transition below) and platform. Engine options are not part of a state's format; the payload
    /// checksum still applies and the engine itself rejects a state it cannot restore.
    pub fn restores(&self, running: &Identity) -> bool {
        self.game == running.game
            && self.target == running.target
            && (self.core == running.core
                || (self.target == "x86_64-windows"
                    && self.core == MUPEN_BEFORE_TEXTURE_FIX
                    && running.core == MUPEN_AFTER_TEXTURE_FIX))
    }
    /// The first identity part that differs, for an actionable message.
    pub fn mismatch(&self, running: &Identity) -> &'static str {
        if self.game != running.game {
            "This state belongs to a different version of the game"
        } else if self.core != running.core {
            "This state was saved with a different emulator version. Choose that version in Game engines to load it"
        } else {
            "This state was saved on a different kind of computer"
        }
    }
    pub fn validate(&self) -> Result<()> {
        for hash in [&self.game, &self.core, &self.options] {
            if hash.len() != 64 || !hash.bytes().all(|b| b.is_ascii_hexdigit()) {
                bail!("Invalid snapshot identity");
            }
        }
        if self.target.is_empty()
            || self.target.len() > 64
            || !self
                .target
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        {
            bail!("Invalid snapshot target");
        }
        Ok(())
    }
    pub fn local(game: String, core: String, options: String) -> Self {
        Self {
            game,
            core,
            options,
            target: format!("{}-{}", std::env::consts::ARCH, std::env::consts::OS),
        }
    }
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Header {
    pub identity: Identity,
    pub size: usize,
    pub digest: String,
}
pub fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
pub fn file_digest(path: &Path) -> Result<String> {
    let mut file = fs::File::open(path)?;
    let mut sha = Sha256::new();
    let mut chunk = [0; 65536];
    loop {
        let n = file.read(&mut chunk)?;
        if n == 0 {
            break;
        }
        sha.update(&chunk[..n]);
    }
    Ok(format!("{:x}", sha.finalize()))
}
fn read_header(file: &mut fs::File) -> Result<Header> {
    let mut prefix = [0u8; 12];
    file.read_exact(&mut prefix)
        .context("This is an older or incomplete save state")?;
    if &prefix[..8] != MAGIC {
        bail!("This older state has no compatibility information");
    }
    let n = u32::from_le_bytes(prefix[8..12].try_into()?) as usize;
    if !(1..=MAX_HEADER).contains(&n) {
        bail!("Invalid save-state header");
    }
    let mut bytes = vec![0; n];
    file.read_exact(&mut bytes)?;
    let header: Header = serde_json::from_slice(&bytes)?;
    header.identity.validate()?;
    if header.size == 0
        || header.size > MAX_STATE
        || file.metadata()?.len() != (12 + n + header.size) as u64
    {
        bail!("Incomplete or oversized save state");
    }
    if header.digest.len() != 64 || !header.digest.bytes().all(|b| b.is_ascii_hexdigit()) {
        bail!("Invalid save-state checksum");
    }
    Ok(header)
}
pub fn inspect(path: &Path) -> Result<Header> {
    read_header(&mut fs::File::open(path)?)
}
/// Older raw states require an explicit user-selected restore; never use this as
/// an automatic fallback for a damaged or mismatched verified state.
pub fn legacy_size(path: &Path) -> Result<usize> {
    legacy_file_size(&mut fs::File::open(path)?)
}
fn legacy_file_size(file: &mut fs::File) -> Result<usize> {
    let size = file.metadata()?.len();
    if size == 0 || size > MAX_STATE as u64 {
        bail!("Invalid older save-state size");
    }
    let mut prefix = [0; 8];
    let n = file.read(&mut prefix)?;
    if prefix[..n] == MAGIC[..n] {
        bail!("Verified-format states cannot bypass compatibility checks");
    }
    Ok(size as usize)
}
pub fn load_legacy_into(path: &Path, target: &mut [u8]) -> Result<usize> {
    let mut file = fs::File::open(path)?;
    let size = legacy_file_size(&mut file)?;
    if size > target.len() {
        bail!("Older state does not fit this engine");
    }
    file.rewind()?;
    file.read_exact(&mut target[..size])?;
    Ok(size)
}
pub fn load(path: &Path, expected: &Identity) -> Result<Vec<u8>> {
    expected.validate()?;
    let mut file = fs::File::open(path)?;
    let header = read_header(&mut file)?;
    if !header.identity.restores(expected) {
        bail!("{}", header.identity.mismatch(expected));
    }
    let mut bytes = vec![0; header.size];
    file.read_exact(&mut bytes)?;
    if digest(&bytes) != header.digest {
        bail!("The save-state checksum does not match; the previous backup is preserved");
    }
    Ok(bytes)
}
pub fn load_into(path: &Path, expected: &Identity, target: &mut [u8]) -> Result<usize> {
    expected.validate()?;
    let mut file = fs::File::open(path)?;
    let header = read_header(&mut file)?;
    if !header.identity.restores(expected) || header.size > target.len() {
        bail!("Incompatible save-state buffer");
    }
    file.read_exact(&mut target[..header.size])?;
    if digest(&target[..header.size]) != header.digest {
        bail!("Save-state checksum mismatch");
    }
    Ok(header.size)
}
/// Replace only a fully synced temporary file. Failed replacements retain the original.
/// Backups are staged and synced before replacing the live generation.
pub fn atomic_write(path: &Path, bytes: &[u8], backup: bool) -> Result<()> {
    write_parts(path, &[bytes], backup)
}
fn write_parts(path: &Path, parts: &[&[u8]], backup: bool) -> Result<()> {
    if parts.iter().map(|part| part.len()).sum::<usize>() > MAX_STATE + MAX_HEADER + 12 {
        bail!("Save exceeds the supported size");
    }
    let parent = path.parent().context("Missing save folder")?;
    fs::create_dir_all(parent)?;
    let temp = parent.join(format!(".emuhall-{}.tmp", uuid::Uuid::new_v4()));
    let result = (|| -> Result<()> {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        for part in parts {
            file.write_all(part)?;
        }
        file.sync_all()?;
        drop(file);
        if backup && path.is_file() {
            if fs::metadata(path)?.len() > (MAX_STATE + MAX_HEADER + 12) as u64 {
                bail!("Previous save exceeds the supported size");
            }
            let name = path
                .file_name()
                .context("Missing save filename")?
                .to_string_lossy();
            let backup_temp = parent.join(format!(".emuhall-{}.tmp", uuid::Uuid::new_v4()));
            let copy = (|| -> Result<()> {
                let mut source = fs::File::open(path)?;
                let mut target = fs::OpenOptions::new()
                    .create_new(true)
                    .write(true)
                    .open(&backup_temp)?;
                std::io::copy(&mut source, &mut target)?;
                target.sync_all()?;
                drop(target);
                replace(&backup_temp, &parent.join(format!("{name}.bak")))
            })();
            if copy.is_err() {
                let _ = fs::remove_file(&backup_temp);
            }
            copy?;
        }
        replace(&temp, path)?;
        #[cfg(unix)]
        fs::File::open(parent)?.sync_all()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}
#[cfg(not(windows))]
fn replace(from: &Path, to: &Path) -> Result<()> {
    fs::rename(from, to)?;
    Ok(())
}
#[cfg(windows)]
fn replace(from: &Path, to: &Path) -> Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
    }
    let from: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let to: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 1 | 8) } == 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(())
}
pub fn save(path: &Path, identity: &Identity, state: &[u8]) -> Result<()> {
    identity.validate()?;
    if state.is_empty() || state.len() > MAX_STATE {
        bail!("Unsupported save-state size");
    }
    let header = serde_json::to_vec(&Header {
        identity: identity.clone(),
        size: state.len(),
        digest: digest(state),
    })?;
    if header.len() > MAX_HEADER {
        bail!("Oversized save-state header");
    }
    let mut prefix = [0; 12];
    prefix[..8].copy_from_slice(MAGIC);
    prefix[8..].copy_from_slice(&(header.len() as u32).to_le_bytes());
    write_parts(path, &[&prefix, &header, state], true)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn identity() -> Identity {
        Identity::local("a".repeat(64), "b".repeat(64), "c".repeat(64))
    }
    #[test]
    fn graphics_only_mupen_transition_accepts_only_the_reviewed_forward_hash_pair() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("state.state");
        let mut old = identity();
        old.target = "x86_64-windows".into();
        old.core = MUPEN_BEFORE_TEXTURE_FIX.into();
        let mut fixed = old.clone();
        fixed.core = MUPEN_AFTER_TEXTURE_FIX.into();
        save(&path, &old, b"original synthetic state").unwrap();
        assert_eq!(load(&path, &fixed).unwrap(), b"original synthetic state");
        assert!(!fixed.restores(&old));
        for part in [0, 1, 2] {
            let mut other = fixed.clone();
            match part {
                0 => other.game = "d".repeat(64),
                1 => other.core = "d".repeat(64),
                _ => other.target = "aarch64-macos".into(),
            }
            assert!(load(&path, &other).is_err());
        }
        let mut damaged = fs::read(&path).unwrap();
        *damaged.last_mut().unwrap() ^= 1;
        fs::write(path, damaged).unwrap();
        assert!(load(&dir.path().join("state.state"), &fixed).is_err());
    }
    #[test]
    fn snapshots_verify_game_engine_platform_and_checksum_but_not_engine_settings() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("state.state");
        let id = identity();
        save(&p, &id, b"original state").unwrap();
        assert_eq!(load(&p, &id).unwrap(), b"original state");
        for field in 0..3 {
            let mut other = id.clone();
            match field {
                0 => other.game = "d".repeat(64),
                1 => other.core = "d".repeat(64),
                _ => other.target = "different-platform".into(),
            };
            assert!(load(&p, &other).is_err());
        }
        // Changing a renderer or speed setting keeps existing states loadable.
        let mut tuned = id.clone();
        tuned.options = "d".repeat(64);
        assert_eq!(load(&p, &tuned).unwrap(), b"original state");
        let mut damaged = fs::read(&p).unwrap();
        *damaged.last_mut().unwrap() ^= 1;
        fs::write(&p, damaged).unwrap();
        assert!(load(&p, &id).is_err());
    }
    #[test]
    fn interrupted_and_legacy_states_never_reach_the_core_and_backups_survive() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("state.state");
        let id = identity();
        save(&p, &id, b"first").unwrap();
        save(&p, &id, b"second-variable-length").unwrap();
        assert_eq!(
            load(&dir.path().join("state.state.bak"), &id).unwrap(),
            b"first"
        );
        let bytes = fs::read(&p).unwrap();
        for size in [0, 7, 12, bytes.len() - 1] {
            fs::write(&p, &bytes[..size]).unwrap();
            assert!(load(&p, &id).is_err());
            assert!(legacy_size(&p).is_err());
            assert!(load_legacy_into(&p, &mut [0; 1024]).is_err());
        }
        fs::write(&p, b"legacy-raw-state").unwrap();
        assert!(load(&p, &id).is_err());
        let mut raw = [0; 16];
        assert_eq!(load_legacy_into(&p, &mut raw).unwrap(), 16);
        assert_eq!(&raw, b"legacy-raw-state");
        assert_eq!(
            load(&dir.path().join("state.state.bak"), &id).unwrap(),
            b"first"
        );
    }
}
