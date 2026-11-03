use std::collections::VecDeque;
pub struct Snapshot {
    pub state: Vec<u8>,
    pub pixels: Vec<u32>,
    pub width: u32,
    pub height: u32,
}
impl Snapshot {
    fn bytes(&self) -> usize {
        self.state.len() + self.pixels.len() * 4
    }
}
pub struct Timeline {
    snapshots: VecDeque<Snapshot>,
    used: usize,
    budget: usize,
}
impl Timeline {
    pub fn new(budget: usize) -> Self {
        Self {
            snapshots: VecDeque::new(),
            used: 0,
            budget,
        }
    }
    pub fn clear(&mut self) {
        self.snapshots.clear();
        self.used = 0;
    }
    pub fn push(&mut self, snapshot: Snapshot) -> bool {
        let size = snapshot.bytes();
        if size > self.budget || snapshot.state.is_empty() {
            return false;
        }
        while self.used + size > self.budget || self.snapshots.len() >= 100 {
            if let Some(old) = self.snapshots.pop_front() {
                self.used -= old.bytes();
            } else {
                break;
            }
        }
        self.used += size;
        self.snapshots.push_back(snapshot);
        true
    }
    pub fn back(&mut self) -> Option<Snapshot> {
        let snapshot = self.snapshots.pop_back()?;
        self.used -= snapshot.bytes();
        Some(snapshot)
    }
    pub fn count(&self) -> usize {
        self.snapshots.len()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn frame(n: u8) -> Snapshot {
        Snapshot {
            state: vec![n; 4],
            pixels: vec![n as u32; 2],
            width: 2,
            height: 1,
        }
    }
    #[test]
    fn bounded_history_evicts_oldest_and_discards_future_after_rewind() {
        let mut t = Timeline::new(24);
        assert!(t.push(frame(1)));
        t.push(frame(2));
        t.push(frame(3));
        assert_eq!(t.count(), 2);
        assert_eq!(t.back().unwrap().state, vec![3; 4]);
        t.push(frame(4));
        assert_eq!(t.back().unwrap().state, vec![4; 4]);
        assert_eq!(t.back().unwrap().state, vec![2; 4]);
        assert!(t.back().is_none());
        let mut t = Timeline::new(11);
        assert!(!t.push(frame(1)));
        assert_eq!(t.count(), 0);
    }
}
