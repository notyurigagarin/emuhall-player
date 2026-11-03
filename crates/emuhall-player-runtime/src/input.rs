use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};

pub const PORTS: usize = 4;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Pad {
    pub buttons: u32,
    pub lx: i16,
    pub ly: i16,
    pub rx: i16,
    pub ry: i16,
    pub lt: i16,
    pub rt: i16,
    pub guide: i32,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub id: String,
    pub name: String,
    /// None disables this device; Some(0) is automatic, 1–4 are explicit players.
    pub player: Option<u8>,
    pub deadzone: u8,
    pub buttons: [u8; 16],
}
impl Profile {
    pub fn validate(&self) -> Result<()> {
        if self.id.is_empty()
            || self.id.len() > 80
            || !self
                .id
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"-_:".contains(&c))
        {
            bail!("Invalid controller identity");
        }
        if self.name.len() > 96
            || self.name.chars().any(char::is_control)
            || self.player.is_some_and(|p| p > 4)
            || !(5..=40).contains(&self.deadzone)
            || self.buttons.iter().any(|b| *b > 15)
        {
            bail!("Invalid controller profile");
        }
        Ok(())
    }
    pub fn apply(&self, mut pad: Pad) -> Pad {
        let mut bits = 0;
        for (target, source) in self.buttons.iter().enumerate() {
            if pad.buttons & (1 << source) != 0 {
                bits |= 1 << target;
            }
        }
        pad.buttons = bits;
        (pad.lx, pad.ly) = deadzone(pad.lx, pad.ly, self.deadzone);
        (pad.rx, pad.ry) = deadzone(pad.rx, pad.ry, self.deadzone);
        pad
    }
}
pub fn deadzone(x: i16, y: i16, percent: u8) -> (i16, i16) {
    let (x, y) = (x as f64, y as f64);
    let length = x.hypot(y);
    let edge = percent as f64 * 32767.0 / 100.0;
    if length <= edge {
        return (0, 0);
    }
    let scale = ((length.min(32767.0) - edge) / (32767.0 - edge)) * 32767.0 / length;
    (
        (x * scale).clamp(-32767.0, 32767.0) as i16,
        (y * scale).clamp(-32767.0, 32767.0) as i16,
    )
}
pub fn validate_profiles(profiles: &[Profile]) -> Result<()> {
    if profiles.len() > 16 {
        bail!("Too many controller profiles");
    }
    let mut ids = std::collections::HashSet::new();
    let mut players = std::collections::HashSet::new();
    for profile in profiles {
        profile.validate()?;
        if let Some(player) = profile.player.filter(|p| *p > 0) {
            if !players.insert(player) {
                bail!("That player already has a saved controller assignment");
            }
        }
        if !ids.insert(&profile.id) {
            bail!("Duplicate controller identity");
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn normalized_profiles_remap_without_merging_players_and_deadzone_has_full_travel() {
        let mut p = Profile {
            id: "device:0".into(),
            name: "Test controller".into(),
            player: Some(2),
            deadzone: 20,
            buttons: std::array::from_fn(|n| n as u8),
        };
        p.buttons.swap(0, 8);
        p.validate().unwrap();
        let pad = p.apply(Pad {
            buttons: 1,
            lx: 300,
            ly: 200,
            rx: 32767,
            ..Pad::default()
        });
        assert_eq!(pad.buttons, 1 << 8);
        assert_eq!((pad.lx, pad.ly), (0, 0));
        assert_eq!(pad.rx, 32767);
        assert_eq!(p.player, Some(2));
        p.player = Some(5);
        assert!(p.validate().is_err());
        p.player = Some(1);
        p.buttons[0] = 16;
        assert!(p.validate().is_err());
    }
}
