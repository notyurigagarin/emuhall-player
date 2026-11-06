//! HTTPS for RetroAchievements where the player has no WinHTTP (Linux). The official rcheevos client
//! (native/achievements.c) hands each request here; it runs on its own thread so a slow server never stalls a frame,
//! and the answer goes back through the C callback on that thread, as WinHTTP's does on Windows. rcheevos locks its
//! own state for callbacks from other threads.
//!
//! Badge images (public, official RetroAchievements pictures) are downloaded the same way, decoded from PNG and scaled
//! to the overlay's size. Only RetroAchievements' own hosts are contacted for badges.
use std::{
    ffi::{c_char, c_int, c_void, CStr},
    io::{Cursor, Read},
    sync::Arc,
    time::Duration,
};

type HttpDone =
    unsafe extern "C" fn(status: c_int, body: *const c_char, length: usize, context: *mut c_void);
type BadgeDone = unsafe extern "C" fn(pixels: *const u32, context: *mut c_void);

/// The C side's context pointer travels to the worker thread and back untouched.
struct Context(*mut c_void);
// SAFETY: the pointer is opaque here; only the C callback dereferences it, exactly once.
unsafe impl Send for Context {}

const BADGE_HOSTS: &[&str] = &["media.retroachievements.org", "retroachievements.org"];
/// Badges are composited over the overlay card's colour, as the Windows decoder does.
const BADGE_BACKGROUND: (u32, u32, u32) = (42, 27, 18);

fn text(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    // SAFETY: the C side passes NUL-terminated strings that live until this call returns.
    unsafe { CStr::from_ptr(pointer) }
        .to_str()
        .ok()
        .map(str::to_owned)
}

fn agent(user_agent: &str, timeout: Duration, redirects: u32) -> Option<ureq::Agent> {
    let tls = native_tls::TlsConnector::new().ok()?;
    Some(
        ureq::AgentBuilder::new()
            .tls_connector(Arc::new(tls))
            .user_agent(user_agent)
            .timeout_connect(Duration::from_secs(10))
            .timeout(timeout)
            .redirects(redirects)
            .build(),
    )
}

/// Status and body; status 0 means the request never completed (rcheevos retries those).
fn request(
    url: &str,
    post: Option<&str>,
    content_type: Option<&str>,
    user_agent: &str,
    limit: usize,
) -> (i32, Vec<u8>) {
    let Some(agent) = agent(user_agent, Duration::from_secs(30), 5) else {
        return (0, Vec::new());
    };
    let mut call = if post.is_some() {
        agent.post(url)
    } else {
        agent.get(url)
    };
    if let Some(kind) = content_type {
        call = call.set("Content-Type", kind);
    }
    let result = match post {
        Some(body) => call.send_string(body),
        None => call.call(),
    };
    let response = match result {
        Ok(response) | Err(ureq::Error::Status(_, response)) => response,
        Err(_) => return (0, Vec::new()),
    };
    let status = i32::from(response.status());
    let mut body = Vec::new();
    if response
        .into_reader()
        .take(limit as u64)
        .read_to_end(&mut body)
        .is_err()
    {
        return (0, Vec::new());
    }
    (status, body)
}

/// Starts one request; `done` is called exactly once, on another thread. Returns 0 (and never calls `done`) when
/// the request could not be started.
///
/// # Safety
/// String arguments are NUL-terminated or null; `done` must be safe to call from any thread with `context`.
#[no_mangle]
pub unsafe extern "C" fn eh_http_request(
    url: *const c_char,
    post: *const c_char,
    content_type: *const c_char,
    user_agent: *const c_char,
    limit: usize,
    done: HttpDone,
    context: *mut c_void,
) -> c_int {
    let Some(url) = text(url) else {
        return 0;
    };
    let (post, content_type) = (text(post), text(content_type));
    let user_agent = text(user_agent).unwrap_or_else(|| "EmuHall".into());
    let context = Context(context);
    std::thread::Builder::new()
        .name("ra-http".into())
        .spawn(move || {
            let context = context;
            let (status, mut body) = request(
                &url,
                post.as_deref(),
                content_type.as_deref(),
                &user_agent,
                limit,
            );
            let length = body.len();
            body.push(0);
            // SAFETY: the body is NUL-terminated and outlives the callback; the callback owns `context`.
            unsafe { done(status, body.as_ptr().cast(), length, context.0) };
        })
        .map(|_| 1)
        .unwrap_or(0)
}

/// Only HTTPS on the default port, to RetroAchievements' own hosts.
fn badge_url_allowed(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("https://") else {
        return false;
    };
    let host = rest.split(['/', '?', '#']).next().unwrap_or_default();
    BADGE_HOSTS
        .iter()
        .any(|allowed| host.eq_ignore_ascii_case(allowed))
}

/// RGBA8 pixels of a PNG.
fn decode_png(bytes: &[u8]) -> Option<(usize, usize, Vec<u8>)> {
    let mut decoder = png::Decoder::new(Cursor::new(bytes));
    decoder.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = decoder.read_info().ok()?;
    let mut buffer = vec![0; reader.output_buffer_size()?];
    let info = reader.next_frame(&mut buffer).ok()?;
    let (width, height) = (info.width as usize, info.height as usize);
    if width == 0 || height == 0 || width > 2048 || height > 2048 {
        return None;
    }
    let channels = match info.color_type {
        png::ColorType::Grayscale => 1,
        png::ColorType::GrayscaleAlpha => 2,
        png::ColorType::Rgb => 3,
        png::ColorType::Rgba => 4,
        png::ColorType::Indexed => return None,
    };
    let stride = info.line_size;
    let mut rgba = Vec::with_capacity(width * height * 4);
    for y in 0..height {
        let row = buffer.get(y * stride..y * stride + width * channels)?;
        for pixel in row.chunks_exact(channels) {
            rgba.extend_from_slice(&match channels {
                1 => [pixel[0], pixel[0], pixel[0], 255],
                2 => [pixel[0], pixel[0], pixel[0], pixel[1]],
                3 => [pixel[0], pixel[1], pixel[2], 255],
                _ => [pixel[0], pixel[1], pixel[2], pixel[3]],
            });
        }
    }
    Some((width, height, rgba))
}

/// Box-filtered to `size`×`size`, composited over the overlay colour: opaque 0xFFRRGGBB, top-down.
pub(crate) fn badge_pixels(width: usize, height: usize, rgba: &[u8], size: usize) -> Vec<u32> {
    let mut out = Vec::with_capacity(size * size);
    for dy in 0..size {
        let (y0, y1) = (
            dy * height / size,
            ((dy + 1) * height / size).max(dy * height / size + 1),
        );
        for dx in 0..size {
            let (x0, x1) = (
                dx * width / size,
                ((dx + 1) * width / size).max(dx * width / size + 1),
            );
            let (mut r, mut g, mut b, mut a, mut n) = (0u32, 0u32, 0u32, 0u32, 0u32);
            for y in y0..y1.min(height) {
                for x in x0..x1.min(width) {
                    let p = &rgba[(y * width + x) * 4..][..4];
                    let alpha = u32::from(p[3]);
                    r += u32::from(p[0]) * alpha;
                    g += u32::from(p[1]) * alpha;
                    b += u32::from(p[2]) * alpha;
                    a += alpha;
                    n += 1;
                }
            }
            let n = n.max(1);
            let alpha = a / n;
            let (br, bg, bb) = BADGE_BACKGROUND;
            // Averaged premultiplied colour plus the background where the badge is transparent:
            // (colour·alpha + background·(255 − alpha)) / 255, as the Windows decoder blends.
            let mix = |sum: u32, back: u32| ((sum / n + back * (255 - alpha)) / 255).min(255);
            let (r, g, b) = (mix(r, br), mix(g, bg), mix(b, bb));
            out.push(0xff00_0000 | (r << 16) | (g << 8) | b);
        }
    }
    out
}

fn fetch_badge(url: &str, size: usize) -> Option<Vec<u32>> {
    let agent = agent("EmuHall", Duration::from_secs(6), 0)?;
    let response = agent.get(url).call().ok()?;
    if response.status() != 200 {
        return None;
    }
    let mut bytes = Vec::new();
    response
        .into_reader()
        .take(1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    if bytes.len() > 1024 * 1024 {
        return None;
    }
    let (width, height, rgba) = decode_png(&bytes)?;
    Some(badge_pixels(width, height, &rgba, size))
}

/// Downloads and decodes one badge; `done` receives `size`×`size` pixels, or null on failure, on another thread.
/// Returns 0 (and never calls `done`) when the address is not allowed or no thread could start.
///
/// # Safety
/// `url` is NUL-terminated; `done` must be safe to call from any thread with `context`.
#[no_mangle]
pub unsafe extern "C" fn eh_badge_fetch(
    url: *const c_char,
    size: u32,
    done: BadgeDone,
    context: *mut c_void,
) -> c_int {
    let Some(url) = text(url).filter(|url| badge_url_allowed(url)) else {
        return 0;
    };
    let size = size.clamp(1, 256) as usize;
    let context = Context(context);
    std::thread::Builder::new()
        .name("ra-badge".into())
        .spawn(move || {
            let context = context;
            let pixels = fetch_badge(&url, size);
            let pointer = pixels.as_ref().map_or(std::ptr::null(), |p| p.as_ptr());
            // SAFETY: the pixels outlive the callback; the callback owns `context`.
            unsafe { done(pointer, context.0) };
        })
        .map(|_| 1)
        .unwrap_or(0)
}

/// `emuhall-core-host --check-retroachievements`: whether this system can reach RetroAchievements over HTTPS and decode
/// its badges. The game lookup asked for is a public, anonymous one for an empty fingerprint; nothing is submitted.
pub(crate) fn self_check() -> serde_json::Value {
    let (status, body) = request(
        "https://retroachievements.org/dorequest.php?r=gameid&m=00000000000000000000000000000000",
        None,
        None,
        concat!("EmuHall/", env!("CARGO_PKG_VERSION"), " (diagnostic)"),
        64 * 1024,
    );
    let answer = String::from_utf8_lossy(&body[..body.len().min(160)]).into_owned();
    let badge = fetch_badge("https://media.retroachievements.org/Badge/00000.png", 64).is_some();
    serde_json::json!({
        "ok": status == 200 && answer.contains("Success") && badge,
        "status": status,
        "answer": answer,
        "badge": badge,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn badges_only_come_from_retroachievements_over_https() {
        assert!(badge_url_allowed(
            "https://media.retroachievements.org/Badge/12345.png"
        ));
        assert!(badge_url_allowed("https://RetroAchievements.org/x.png"));
        assert!(!badge_url_allowed(
            "http://media.retroachievements.org/a.png"
        ));
        assert!(!badge_url_allowed(
            "https://media.retroachievements.org.evil.example/a.png"
        ));
        assert!(!badge_url_allowed(
            "https://media.retroachievements.org:8443/a.png"
        ));
        assert!(!badge_url_allowed(
            "https://user@media.retroachievements.org/a.png"
        ));
    }

    #[test]
    fn badges_scale_and_blend_over_the_overlay_colour() {
        // 2×2: opaque red, opaque green, transparent, opaque blue.
        let rgba = [255, 0, 0, 255, 0, 255, 0, 255, 9, 9, 9, 0, 0, 0, 255, 255];
        let same = badge_pixels(2, 2, &rgba, 2);
        assert_eq!(same[0], 0xffff0000);
        assert_eq!(same[1], 0xff00ff00);
        assert_eq!(
            same[2], 0xff2a1b12,
            "transparent pixels show the background"
        );
        assert_eq!(same[3], 0xff0000ff);
        let up = badge_pixels(2, 2, &rgba, 4);
        assert_eq!(up.len(), 16);
        assert_eq!(up[0], 0xffff0000);
        let down = badge_pixels(2, 2, &rgba, 1);
        assert_eq!(down.len(), 1);
        assert_eq!(down[0] >> 24, 0xff);
    }

    #[test]
    fn png_badges_decode_to_rgba() {
        let mut bytes = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut bytes, 2, 1);
            encoder.set_color(png::ColorType::Rgb);
            encoder.set_depth(png::BitDepth::Eight);
            let mut writer = encoder.write_header().unwrap();
            writer.write_image_data(&[1, 2, 3, 4, 5, 6]).unwrap();
        }
        let (w, h, rgba) = decode_png(&bytes).unwrap();
        assert_eq!((w, h), (2, 1));
        assert_eq!(rgba, vec![1, 2, 3, 255, 4, 5, 6, 255]);
        assert!(decode_png(b"not a png").is_none());
    }
}
