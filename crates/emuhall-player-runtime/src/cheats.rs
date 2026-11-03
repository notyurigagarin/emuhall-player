//! Bounded data-memory adapters for cores whose libretro cheat setter cannot be used.
//! No encrypted formats, executable hooks, bulk copies or arbitrary host addresses.
use anyhow::{bail, Context, Result};

#[derive(Clone, Debug)]
pub struct Operation {
    address: usize,
    width: usize,
    value: u32,
    condition: u8,
}
#[derive(Clone, Debug)]
pub struct Program {
    kind: u32,
    operations: Vec<Operation>,
}
impl Program {
    pub fn parse(kind: u32, code: &str) -> Result<Self> {
        if ![10, 11].contains(&kind) || code.len() > 4608 || !code.is_ascii() {
            bail!("Unsupported memory cheat");
        }
        let mut operations = Vec::new();
        for line in code.split('+') {
            let words: Vec<_> = line.split_whitespace().collect();
            if words.len() != 2
                || words[0].len() != 8
                || words[1].len() != if kind == 10 { 4 } else { 8 }
            {
                bail!("Invalid code line");
            }
            let first = u32::from_str_radix(words[0], 16).context("Invalid address")?;
            let value = u32::from_str_radix(words[1], 16).context("Invalid value")?;
            let (address, width, condition, limit) = if kind == 10 {
                let opcode = first >> 24;
                let (width, condition) = match opcode {
                    0x80 | 0xa0 => (1, 0),
                    0x81 | 0xa1 => (2, 0),
                    0xd0 => (1, 1),
                    0xd1 => (2, 1),
                    0xd2 => (1, 2),
                    0xd3 => (2, 2),
                    _ => bail!("Only constant writes and single-line conditions are supported"),
                };
                (
                    (first & 0xffffff) as usize,
                    width,
                    condition,
                    8 * 1024 * 1024,
                )
            } else {
                let width = match first >> 28 {
                    0 => 1,
                    1 => 2,
                    2 => 4,
                    _ => bail!("Use decrypted byte, halfword or word writes"),
                };
                ((first & 0x0fffffff) as usize, width, 0, 32 * 1024 * 1024)
            };
            if address + width > limit
                || !address.is_multiple_of(width)
                || width < 4 && value >= (1u32 << (width * 8))
            {
                bail!("Code is outside supported data RAM or has an invalid value/alignment");
            }
            operations.push(Operation {
                address,
                width,
                value,
                condition,
            });
        }
        if operations.is_empty() || operations.len() > 256 {
            bail!("Use 1 to 256 lines");
        }
        for (index, op) in operations.iter().enumerate() {
            if op.condition != 0
                && operations
                    .get(index + 1)
                    .is_none_or(|next| next.condition != 0)
            {
                bail!("Each condition must be followed by one data write");
            }
        }
        Ok(Self { kind, operations })
    }
    pub fn fits(&self, size: usize) -> bool {
        self.operations
            .iter()
            .all(|op| op.address + op.width <= size)
    }
    /// The core owns this buffer. N64 exposes word-swapped RDRAM on little-endian hosts.
    pub fn apply(&self, ram: &mut [u8]) {
        let mut skip = false;
        for op in &self.operations {
            if skip {
                skip = false;
                continue;
            }
            let mut locations = [0usize; 4];
            let mut valid = true;
            for (i, location) in locations.iter_mut().enumerate().take(op.width) {
                *location = if self.kind == 10 && cfg!(target_endian = "little") {
                    (op.address + i) ^ 3
                } else {
                    op.address + i
                };
                if *location >= ram.len() {
                    valid = false;
                }
            }
            if !valid {
                if op.condition != 0 {
                    skip = true;
                }
                continue;
            }
            if op.condition != 0 {
                let mut actual = 0u32;
                for location in locations.iter().take(op.width) {
                    actual = (actual << 8) | u32::from(ram[*location]);
                }
                skip = if op.condition == 1 {
                    actual != op.value
                } else {
                    actual == op.value
                };
            } else {
                for (i, location) in locations.iter().enumerate().take(op.width) {
                    let shift = if self.kind == 10 {
                        (op.width - 1 - i) * 8
                    } else {
                        i * 8
                    };
                    ram[*location] = (op.value >> shift) as u8;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn n64_word_order_conditions_and_bounds_are_explicit() {
        let mut ram = vec![0; 32];
        let program = Program::parse(
            10,
            "80000000 0042+81000002 1234+D0000000 0042+80000004 0077",
        )
        .unwrap();
        program.apply(&mut ram);
        let lane = if cfg!(target_endian = "little") { 3 } else { 0 };
        assert_eq!(ram[lane], 0x42);
        assert_eq!(ram[4 ^ lane], 0x77);
        assert_eq!(ram[2 ^ lane], 0x12);
        assert_eq!(ram[3 ^ lane], 0x34);
        Program::parse(10, "D0000000 0000+80000004 0099")
            .unwrap()
            .apply(&mut ram);
        assert_eq!(ram[4 ^ lane], 0x77);
        assert!(Program::parse(10, "50000000 0001").is_err());
        assert!(Program::parse(10, "D0000000 0001").is_err());
        assert!(Program::parse(10, "81800000 1234").is_err());
        Program::parse(10, "807FFFFF 00FF").unwrap().apply(&mut ram);
        assert_eq!(ram[4 ^ lane], 0x77);
    }
    #[test]
    fn ps2_decrypted_writes_are_little_endian_and_reject_encrypted_commands() {
        let mut ram = vec![0; 16];
        Program::parse(11, "00000000 00000042+10000002 00001234+20000004 ABCDEF01")
            .unwrap()
            .apply(&mut ram);
        assert_eq!(&ram[..8], &[0x42, 0, 0x34, 0x12, 1, 0xef, 0xcd, 0xab]);
        assert!(Program::parse(11, "90000004 ABCDEF01").is_err());
        assert!(Program::parse(11, "20000003 00000001").is_err());
    }
}
