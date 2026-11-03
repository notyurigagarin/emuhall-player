//! Original metadata adapter. Reads a separately licensed engine database as data;
//! no emulator implementation or catalog contents are compiled into the MIT player.
use anyhow::{ensure, Result};
use std::{collections::HashMap, fs::File, io::Read, path::Path};

pub fn n64_title(database: &Path, fingerprint: &str) -> Result<Option<String>> {
    ensure!(
        fingerprint.len() == 32 && fingerprint.bytes().all(|b| b.is_ascii_hexdigit()),
        "Invalid N64 fingerprint"
    );
    let mut bytes = Vec::new();
    File::open(database)?
        .take(8 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= 8 * 1024 * 1024, "Oversized N64 catalog");
    let mut records: HashMap<String, (Option<String>, Option<String>)> = HashMap::new();
    let mut section = None;
    for line in String::from_utf8_lossy(&bytes).lines() {
        let line = line.trim();
        if let Some(hash) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            section = (hash.len() == 32 && hash.bytes().all(|b| b.is_ascii_hexdigit()))
                .then(|| hash.to_ascii_lowercase());
        } else if let (Some(hash), Some((key, value))) = (section.as_ref(), line.split_once('=')) {
            let record = records.entry(hash.clone()).or_default();
            if key == "GoodName"
                && !value.is_empty()
                && value.chars().count() <= 200
                && !value.chars().any(char::is_control)
            {
                record.0 = Some(value.to_owned());
            } else if key == "RefMD5"
                && value.len() == 32
                && value.bytes().all(|b| b.is_ascii_hexdigit())
            {
                record.1 = Some(value.to_ascii_lowercase());
            }
        }
    }
    let mut hash = fingerprint.to_ascii_lowercase();
    for _ in 0..8 {
        let Some((title, reference)) = records.get(&hash) else {
            return Ok(None);
        };
        if let Some(title) = title {
            return Ok(Some(title.clone()));
        }
        let Some(reference) = reference else {
            return Ok(None);
        };
        hash = reference.clone();
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn catalog_identification_uses_exact_fingerprints_and_bounded_references() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.ini");
        fs_write(
            &path,
            &format!(
                "[{}]\nGoodName=Signal Garden (U)\n[{}]\nRefMD5={}\n[{}]\nRefMD5={}\n",
                "a".repeat(32),
                "b".repeat(32),
                "a".repeat(32),
                "c".repeat(32),
                "c".repeat(32)
            ),
        );
        assert_eq!(
            n64_title(&path, &"B".repeat(32)).unwrap().as_deref(),
            Some("Signal Garden (U)")
        );
        assert!(n64_title(&path, &"c".repeat(32)).unwrap().is_none());
        assert!(n64_title(&path, &"d".repeat(32)).unwrap().is_none());
        assert!(n64_title(&path, "invalid").is_err());
    }
    fn fs_write(path: &Path, text: &str) {
        std::fs::write(path, text).unwrap();
    }
}
