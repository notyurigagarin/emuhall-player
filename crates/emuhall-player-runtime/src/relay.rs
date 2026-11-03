//! Bounded two-peer byte relay behind a TLS terminator. No emulator or save data is loaded here.
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    io::{self, Read, Write},
    net::{Shutdown, SocketAddr, TcpListener, TcpStream},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn unhex(text: &str) -> io::Result<Vec<u8>> {
    if !text.len().is_multiple_of(2)
        || text.len() > 4096
        || !text.bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Err(invalid("Invalid relay ticket"));
    }
    (0..text.len())
        .step_by(2)
        .map(|i| {
            u8::from_str_radix(&text[i..i + 2], 16).map_err(|_| invalid("Invalid relay ticket"))
        })
        .collect()
}
pub fn hmac(key: &[u8], body: &[u8]) -> [u8; 32] {
    let mut block = [0u8; 64];
    if key.len() > 64 {
        block[..32].copy_from_slice(&Sha256::digest(key));
    } else {
        block[..key.len()].copy_from_slice(key);
    }
    let inner: Vec<u8> = block
        .iter()
        .map(|b| b ^ 0x36)
        .chain(body.iter().copied())
        .collect();
    let mut outer: Vec<u8> = block.iter().map(|b| b ^ 0x5c).collect();
    outer.extend(Sha256::digest(inner));
    Sha256::digest(outer).into()
}
pub fn verify(secret: &str, ticket: &str, time: u64) -> io::Result<Value> {
    if secret.len() < 32 || ticket.len() > 2048 {
        return Err(invalid("Invalid relay ticket"));
    }
    let parts: Vec<_> = ticket.split('.').collect();
    if parts.len() != 2 {
        return Err(invalid("Invalid relay ticket"));
    }
    let signature = unhex(parts[1])?;
    let expected = hmac(secret.as_bytes(), parts[0].as_bytes());
    if signature.len() != 32
        || signature
            .iter()
            .zip(expected)
            .fold(0u8, |n, (a, b)| n | (a ^ b))
            != 0
    {
        return Err(invalid("Invalid relay ticket"));
    }
    let data: Value =
        serde_json::from_slice(&unhex(parts[0])?).map_err(|_| invalid("Invalid relay ticket"))?;
    let room = data["room"].as_str().unwrap_or("");
    let role = data["role"].as_str().unwrap_or("");
    let nonce = data["nonce"].as_str().unwrap_or("");
    let expiry = data["exp"].as_u64().unwrap_or(0);
    if data["v"] != 1
        || uuid::Uuid::parse_str(room).is_err()
        || !["host", "join"].contains(&role)
        || nonce.len() != 32
        || !nonce.bytes().all(|b| b.is_ascii_hexdigit())
        || expiry <= time
        || expiry > time + 330000
    {
        return Err(invalid("Expired or invalid relay ticket"));
    }
    Ok(data)
}
pub fn write_json(stream: &mut impl Write, value: &Value) -> io::Result<()> {
    let bytes = serde_json::to_vec(value)?;
    if bytes.len() > 4096 {
        return Err(invalid("Relay message too large"));
    }
    stream.write_all(&(bytes.len() as u32).to_be_bytes())?;
    stream.write_all(&bytes)?;
    stream.flush()
}
fn read_json(stream: &mut TcpStream) -> io::Result<Value> {
    let mut length = [0; 4];
    stream.read_exact(&mut length)?;
    let size = u32::from_be_bytes(length) as usize;
    if size > 4096 {
        return Err(invalid("Relay message too large"));
    }
    let mut bytes = vec![0; size];
    stream.read_exact(&mut bytes)?;
    serde_json::from_slice(&bytes).map_err(|_| invalid("Invalid relay request"))
}
fn acquire(counter: &AtomicUsize, maximum: usize) -> bool {
    let mut value = counter.load(Ordering::SeqCst);
    loop {
        if value >= maximum {
            return false;
        }
        match counter.compare_exchange_weak(value, value + 1, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(current) => value = current,
        }
    }
}
struct Pending {
    stream: TcpStream,
    host: bool,
    until: Instant,
}
#[derive(Default)]
struct Hub {
    waiting: Mutex<HashMap<String, Pending>>,
    seen: Mutex<HashMap<String, u64>>,
    pairs: AtomicUsize,
    handshakes: AtomicUsize,
}
impl Hub {
    fn sweep(&self) {
        self.waiting
            .lock()
            .unwrap()
            .retain(|_, p| p.until > Instant::now());
        self.seen
            .lock()
            .unwrap()
            .retain(|_, expiry| *expiry > now());
    }
    fn accept(self: Arc<Self>, mut stream: TcpStream, secret: Arc<String>) -> io::Result<()> {
        stream.set_read_timeout(Some(Duration::from_secs(3)))?;
        stream.set_write_timeout(Some(Duration::from_secs(3)))?;
        stream.set_nodelay(true)?;
        let hello = read_json(&mut stream)?;
        if hello["protocol"] != "EmuHallRelay/1" {
            return Err(invalid("Invalid relay protocol"));
        }
        let ticket = verify(&secret, hello["ticket"].as_str().unwrap_or(""), now())?;
        {
            let mut seen = self.seen.lock().unwrap();
            if seen.len() >= 4096 || seen.contains_key(ticket["nonce"].as_str().unwrap()) {
                return Err(invalid("Relay ticket was already used"));
            }
            seen.insert(
                ticket["nonce"].as_str().unwrap().into(),
                ticket["exp"].as_u64().unwrap(),
            );
        }
        let room = ticket["room"].as_str().unwrap().to_owned();
        let host = ticket["role"] == "host";
        let mut waiting = self.waiting.lock().unwrap();
        if let Some(mut other) = waiting.remove(&room) {
            if other.host == host {
                waiting.insert(room, other);
                return Err(invalid("This relay role is occupied"));
            }
            drop(waiting);
            if !acquire(&self.pairs, 64) {
                return Err(invalid("Relay is full"));
            }
            let ready = json!({"paired":true});
            if write_json(&mut stream, &ready)
                .and_then(|_| write_json(&mut other.stream, &ready))
                .is_err()
            {
                self.pairs.fetch_sub(1, Ordering::SeqCst);
                return Err(invalid("Relay peer left"));
            }
            let hub = self.clone();
            std::thread::spawn(move || {
                let _ = bridge(stream, other.stream);
                hub.pairs.fetch_sub(1, Ordering::SeqCst);
            });
        } else {
            if waiting.len() >= 64 {
                return Err(invalid("Relay is full"));
            }
            waiting.insert(
                room,
                Pending {
                    stream,
                    host,
                    until: Instant::now() + Duration::from_secs(120),
                },
            );
        }
        Ok(())
    }
}
fn pump(mut source: TcpStream, mut destination: TcpStream) -> io::Result<()> {
    source.set_read_timeout(Some(Duration::from_millis(200)))?;
    destination.set_write_timeout(Some(Duration::from_secs(3)))?;
    let started = Instant::now();
    let mut last = started;
    let mut bucket = started;
    let mut rate = 0usize;
    let mut total = 0usize;
    let mut bytes = [0; 4096];
    loop {
        if started.elapsed() > Duration::from_secs(6 * 3600)
            || last.elapsed() > Duration::from_secs(30)
        {
            return Err(invalid("Relay session expired"));
        }
        match source.read(&mut bytes) {
            Ok(0) => return Ok(()),
            Ok(count) => {
                if bucket.elapsed() >= Duration::from_secs(1) {
                    bucket = Instant::now();
                    rate = 0;
                }
                rate += count;
                total += count;
                if rate > 65536 || total > 128 * 1024 * 1024 {
                    return Err(invalid("Relay traffic limit exceeded"));
                }
                destination.write_all(&bytes[..count])?;
                last = Instant::now();
            }
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) => {}
            Err(error) => return Err(error),
        }
    }
}
fn bridge(a: TcpStream, b: TcpStream) -> io::Result<()> {
    let aa = a.try_clone()?;
    let bb = b.try_clone()?;
    let shutdown_a = a.try_clone()?;
    let shutdown_b = b.try_clone()?;
    let main_a = a.try_clone()?;
    let main_b = b.try_clone()?;
    let worker = std::thread::spawn(move || {
        let result = pump(aa, bb);
        let _ = shutdown_a.shutdown(Shutdown::Both);
        let _ = shutdown_b.shutdown(Shutdown::Both);
        result
    });
    let result = pump(b, a);
    let _ = main_a.shutdown(Shutdown::Both);
    let _ = main_b.shutdown(Shutdown::Both);
    let _ = worker.join();
    result
}
pub fn serve(address: SocketAddr, secret: String) -> io::Result<()> {
    if !address.ip().is_loopback() || secret.len() < 32 {
        return Err(invalid(
            "Bind the relay to loopback behind TLS; configure a 32-character server key",
        ));
    }
    let listener = TcpListener::bind(address)?;
    listener.set_nonblocking(true)?;
    let hub = Arc::new(Hub::default());
    let secret = Arc::new(secret);
    loop {
        hub.sweep();
        match listener.accept() {
            Ok((stream, _)) => {
                if !acquire(&hub.handshakes, 128) {
                    drop(stream);
                    continue;
                }
                let state = hub.clone();
                let key = secret.clone();
                std::thread::spawn(move || {
                    let _ = state.clone().accept(stream, key);
                    state.handshakes.fetch_sub(1, Ordering::SeqCst);
                });
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(100))
            }
            Err(error) => return Err(error),
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hmac_matches_rfc4231() {
        assert_eq!(
            hex(&hmac(&[0x0b; 20], b"Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
    }
    #[test]
    fn tickets_reject_tampering_and_expiration() {
        let secret = "original synthetic relay secret at least 32";
        let data = json!({"v":1,"room":uuid::Uuid::new_v4().to_string(),"role":"host","nonce":"a".repeat(32),"exp":now()+60000});
        let body = hex(&serde_json::to_vec(&data).unwrap());
        let ticket = format!("{body}.{}", hex(&hmac(secret.as_bytes(), body.as_bytes())));
        assert!(verify(secret, &ticket, now()).is_ok());
        assert!(verify("other secret at least 32 characters long", &ticket, now()).is_err());
        assert!(verify(secret, &ticket, now() + 60001).is_err());
    }
    #[test]
    fn paired_relay_forwards_both_directions_and_rejects_replay() {
        let secret = Arc::new("original synthetic relay test secret 32 characters".to_owned());
        let hub = Arc::new(Hub::default());
        let room = uuid::Uuid::new_v4().to_string();
        fn connect(
            hub: Arc<Hub>,
            secret: Arc<String>,
            room: &str,
            role: &str,
            nonce: &str,
        ) -> (TcpStream, std::thread::JoinHandle<io::Result<()>>, String) {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let mut client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
            client
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            let (server, _) = listener.accept().unwrap();
            let body = hex(&serde_json::to_vec(
                &json!({"v":1,"room":room,"role":role,"nonce":nonce,"exp":now()+60000}),
            )
            .unwrap());
            let ticket = format!("{body}.{}", hex(&hmac(secret.as_bytes(), body.as_bytes())));
            write_json(
                &mut client,
                &json!({"protocol":"EmuHallRelay/1","ticket":ticket}),
            )
            .unwrap();
            let worker = std::thread::spawn(move || hub.accept(server, secret));
            (client, worker, ticket)
        }
        let (mut host, first, ticket) =
            connect(hub.clone(), secret.clone(), &room, "host", &"a".repeat(32));
        first.join().unwrap().unwrap();
        let (mut guest, second, _) =
            connect(hub.clone(), secret.clone(), &room, "join", &"b".repeat(32));
        second.join().unwrap().unwrap();
        assert_eq!(read_json(&mut host).unwrap()["paired"], true);
        assert_eq!(read_json(&mut guest).unwrap()["paired"], true);
        host.write_all(b"host-input").unwrap();
        let mut bytes = [0u8; 10];
        guest.read_exact(&mut bytes).unwrap();
        assert_eq!(&bytes, b"host-input");
        guest.write_all(b"join-input").unwrap();
        host.read_exact(&mut bytes).unwrap();
        assert_eq!(&bytes, b"join-input");
        host.shutdown(Shutdown::Both).unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let mut replay = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();
        write_json(
            &mut replay,
            &json!({"protocol":"EmuHallRelay/1","ticket":ticket}),
        )
        .unwrap();
        assert!(hub.clone().accept(server, secret).is_err());
        let deadline = Instant::now() + Duration::from_secs(3);
        while hub.pairs.load(Ordering::SeqCst) != 0 {
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}
