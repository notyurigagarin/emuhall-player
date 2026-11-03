//! Small deterministic presentation filters, independent of a graphics API.
//! Input remains untouched by callers: filtering belongs on a presentation copy.
pub fn filter(pixels: &mut [u32], width: usize, height: usize, mode: u32) -> bool {
    if width == 0
        || height == 0
        || width > 2048
        || height > 2048
        || pixels.len() != width * height
        || mode > 2
    {
        return false;
    }
    if mode == 0 {
        return true;
    }
    for y in 0..height {
        for x in 0..width {
            let pixel = pixels[y * width + x];
            let mut channels = [(pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255];
            for (channel, value) in channels.iter_mut().enumerate() {
                let gain = if mode == 1 {
                    let line = if y % 2 == 1 { 82 } else { 100 };
                    if x % 3 == channel {
                        line
                    } else {
                        line * 94 / 100
                    }
                } else if x % 4 == 3 || y % 4 == 3 {
                    86
                } else {
                    100
                };
                *value = *value * gain / 100;
            }
            pixels[y * width + x] = (channels[0] << 16) | (channels[1] << 8) | channels[2];
        }
    }
    true
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn presentation_filters_are_bounded_preserve_black_and_leave_off_unchanged() {
        let original = vec![0xffffff; 16];
        let mut image = original.clone();
        assert!(filter(&mut image, 4, 4, 0));
        assert_eq!(image, original);
        filter(&mut image, 4, 4, 1);
        assert!(image[4] < image[0]);
        assert_ne!(image, original);
        let mut black = vec![0; 16];
        filter(&mut black, 4, 4, 2);
        assert_eq!(black, vec![0; 16]);
        assert!(!filter(&mut black, 2049, 1, 1));
        assert!(!filter(&mut black, 4, 4, 3));
    }
}
