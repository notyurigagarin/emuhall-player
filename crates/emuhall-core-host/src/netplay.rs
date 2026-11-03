//! Original bounded TCP lockstep protocol. The core and network run only in the worker.
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    io::{self, Read, Write},
    net::{IpAddr, SocketAddr, TcpListener, TcpStream, ToSocketAddrs},
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};
#[cfg(not(test))]
extern "C" {
    fn eh_netplay_cancelled() -> i32;
}
static SESSION: OnceLock<Mutex<Peer>> = OnceLock::new();
const DELAY: usize = 6;
enum Transport {
    Direct(TcpStream),
    Relay(Box<native_tls::TlsStream<TcpStream>>),
}
impl Transport {
    fn socket(&self) -> &TcpStream {
        match self {
            Self::Direct(s) => s,
            Self::Relay(s) => s.get_ref(),
        }
    }
}
impl Read for Transport {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        match self {
            Self::Direct(s) => s.read(bytes),
            Self::Relay(s) => s.read(bytes),
        }
    }
}
impl Write for Transport {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        match self {
            Self::Direct(s) => s.write(bytes),
            Self::Relay(s) => s.write(bytes),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Self::Direct(s) => s.flush(),
            Self::Relay(s) => s.flush(),
        }
    }
}
impl From<TcpStream> for Transport {
    fn from(stream: TcpStream) -> Self {
        Self::Direct(stream)
    }
}
fn relay_connect(config: &Value) -> io::Result<Transport> {
    let host = config["host"]
        .as_str()
        .filter(|s| {
            !s.is_empty()
                && s.len() <= 253
                && s.bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
        })
        .ok_or_else(|| invalid("Invalid relay host"))?;
    let port = config["port"]
        .as_u64()
        .filter(|p| *p > 0 && *p <= 65535)
        .ok_or_else(|| invalid("Invalid relay port"))? as u16;
    let address = (host, port)
        .to_socket_addrs()?
        .take(4)
        .find_map(|address| TcpStream::connect_timeout(&address, Duration::from_secs(5)).ok())
        .ok_or_else(|| invalid("Could not reach the relay"))?;
    address.set_nodelay(true)?;
    address.set_read_timeout(Some(Duration::from_secs(5)))?;
    address.set_write_timeout(Some(Duration::from_secs(3)))?;
    let mut stream = if config["tls"] == true {
        let connector = native_tls::TlsConnector::new().map_err(io::Error::other)?;
        Transport::Relay(Box::new(
            connector
                .connect(host, address)
                .map_err(|_| invalid("Could not verify the relay's TLS connection"))?,
        ))
    } else if address.peer_addr()?.ip().is_loopback() {
        Transport::Direct(address)
    } else {
        return Err(invalid("Internet relay connections require TLS"));
    };
    stream
        .socket()
        .set_read_timeout(Some(Duration::from_millis(100)))?;
    let ticket = config["ticket"]
        .as_str()
        .filter(|s| s.len() <= 2048)
        .ok_or_else(|| invalid("Missing relay ticket"))?;
    send_json(
        &mut stream,
        json!({"protocol":"EmuHallRelay/1","ticket":ticket}),
    )?;
    event("waiting");
    if read_json_timeout(&mut stream, Duration::from_secs(120))?["paired"] != true {
        return Err(invalid("Relay pairing failed"));
    }
    Ok(stream)
}
struct Peer {
    stream: Transport,
    host: bool,
    inputs: [u16; DELAY],
    pictures: [u32; DELAY],
}
fn cancelled() -> bool {
    #[cfg(not(test))]
    {
        unsafe { eh_netplay_cancelled() != 0 }
    }
    #[cfg(test)]
    {
        false
    }
}
fn event(status: &str) {
    println!("{}", json!({"event":"netplay","status":status}));
    let _ = io::stdout().flush();
}
fn failure(error: impl std::fmt::Display) -> i32 {
    println!("{}", json!({"event":"error","message":error.to_string()}));
    let _ = io::stdout().flush();
    -1
}
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn receive(stream: &mut impl Read, bytes: &mut [u8], timeout: Duration) -> io::Result<()> {
    let deadline = Instant::now() + timeout;
    let mut offset = 0;
    while offset < bytes.len() {
        if cancelled() {
            return Err(invalid("Netplay closed"));
        }
        if Instant::now() > deadline {
            return Err(invalid("The other player stopped responding"));
        }
        match stream.read(&mut bytes[offset..]) {
            Ok(0) => return Err(invalid("The other player disconnected")),
            Ok(n) => offset += n,
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
fn send_json(stream: &mut impl Write, value: Value) -> io::Result<()> {
    let bytes = serde_json::to_vec(&value)?;
    stream.write_all(&(bytes.len() as u32).to_be_bytes())?;
    stream.write_all(&bytes)
}
fn read_json(stream: &mut impl Read) -> io::Result<Value> {
    read_json_timeout(stream, Duration::from_secs(10))
}
fn read_json_timeout(stream: &mut impl Read, timeout: Duration) -> io::Result<Value> {
    let mut size = [0; 4];
    receive(stream, &mut size, timeout)?;
    let size = u32::from_be_bytes(size) as usize;
    if size > 4096 {
        return Err(invalid("Unsupported netplay message"));
    }
    let mut body = vec![0; size];
    receive(stream, &mut body, timeout)?;
    serde_json::from_slice(&body).map_err(|_| invalid("Unsupported netplay message"))
}
fn proof(code: &str, nonce: &str, identity: &str) -> String {
    format!(
        "{:x}",
        Sha256::digest(format!("EmuHall/{code}/{nonce}/{identity}"))
    )
}
fn start(config: Value) -> io::Result<Peer> {
    let host = config["role"] == "host";
    let port = config["port"]
        .as_u64()
        .filter(|n| *n > 0 && *n <= 65535)
        .ok_or_else(|| invalid("Invalid netplay port"))? as u16;
    let mut stream = if config["relay"].is_object() {
        relay_connect(&config["relay"])?
    } else {
        let tcp = if host {
            // IPv4 is the initial supported listening family. No router or firewall rules are changed.
            let listener = TcpListener::bind(("0.0.0.0", port))?;
            listener.set_nonblocking(true)?;
            event("waiting");
            let deadline = Instant::now() + Duration::from_secs(120);
            loop {
                if cancelled() {
                    return Err(invalid("Netplay closed"));
                }
                if Instant::now() > deadline {
                    return Err(invalid("No player joined. Create a room to try again."));
                }
                match listener.accept() {
                    Ok((s, _)) => break s,
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(20))
                    }
                    Err(e) => return Err(e),
                }
            }
        } else {
            event("connecting");
            let ip: IpAddr = config["address"]
                .as_str()
                .unwrap_or("")
                .parse()
                .map_err(|_| invalid("Invalid host address"))?;
            TcpStream::connect_timeout(&SocketAddr::new(ip, port), Duration::from_secs(5))?
        };
        Transport::Direct(tcp)
    };
    stream.socket().set_nodelay(true)?;
    stream
        .socket()
        .set_read_timeout(Some(Duration::from_millis(100)))?;
    stream
        .socket()
        .set_write_timeout(Some(Duration::from_secs(3)))?;
    let identity = config["identity"]
        .as_str()
        .ok_or_else(|| invalid("Missing game identity"))?;
    let code = config["code"]
        .as_str()
        .ok_or_else(|| invalid("Missing room code"))?;
    let nonce = uuid::Uuid::new_v4().simple().to_string();
    send_json(
        &mut stream,
        json!({"protocol":"EmuHall/1","nonce":nonce,"identity":identity,"host":host}),
    )?;
    let hello = read_json(&mut stream)?;
    if hello["protocol"] != "EmuHall/1" || hello["identity"] != identity || hello["host"] == host {
        return Err(invalid(
            "Both players need the same game, emulator build and settings",
        ));
    }
    let remote_nonce = hello["nonce"]
        .as_str()
        .filter(|n| n.len() == 32)
        .ok_or_else(|| invalid("Invalid room handshake"))?;
    send_json(
        &mut stream,
        json!({"proof":proof(code,remote_nonce,identity)}),
    )?;
    if read_json(&mut stream)?["proof"] != proof(code, &nonce, identity) {
        return Err(invalid("Room codes do not match"));
    }
    event("connected");
    Ok(Peer {
        stream,
        host,
        inputs: [0; DELAY],
        pictures: [0; DELAY],
    })
}
#[no_mangle]
pub extern "C" fn eh_netplay_start() -> i32 {
    let Ok(raw) = std::env::var("EMUHALL_NETPLAY") else {
        return 0;
    };
    let result = serde_json::from_str(&raw)
        .map_err(io::Error::other)
        .and_then(start);
    match result {
        Ok(peer) => {
            let _ = SESSION.set(Mutex::new(peer));
            1
        }
        Err(_) if cancelled() => -1,
        Err(e) => failure(e),
    }
}
impl Peer {
    fn exchange(&mut self, frame: u32, local: u16, picture: u32) -> io::Result<i64> {
        let slot = frame as usize % DELAY;
        let old_input = self.inputs[slot];
        let old_picture = self.pictures[slot];
        self.inputs[slot] = local;
        self.pictures[slot] = picture;
        let mut packet = [0u8; 10];
        packet[..4].copy_from_slice(&(frame + DELAY as u32).to_be_bytes());
        packet[4..6].copy_from_slice(&local.to_be_bytes());
        packet[6..].copy_from_slice(&picture.to_be_bytes());
        self.stream.write_all(&packet)?;
        // Both peers start with neutral controls. Six frames in flight cover up to ~100ms RTT
        // without tying the emulation rate to one network round trip per frame.
        if frame < DELAY as u32 {
            return Ok(0);
        }
        let mut remote = [0; 10];
        receive(&mut self.stream, &mut remote, Duration::from_secs(5))?;
        if remote[..4] != frame.to_be_bytes() {
            return Err(invalid("Netplay lost its frame order"));
        }
        if frame.is_multiple_of(120) && remote[6..] != old_picture.to_be_bytes() {
            return Err(invalid(
                "The games went out of sync. Close the room and try again.",
            ));
        }
        let input = u16::from_be_bytes([remote[4], remote[5]]);
        Ok(if self.host {
            old_input as i64 | ((input as i64) << 16)
        } else {
            input as i64 | ((old_input as i64) << 16)
        })
    }
}
#[no_mangle]
pub extern "C" fn eh_netplay_frame(frame: u32, local: u16, picture: u32) -> i64 {
    let Some(session) = SESSION.get() else {
        return -1;
    };
    match session.lock().unwrap().exchange(frame, local, picture) {
        Ok(input) => input,
        Err(_) if cancelled() => -1,
        Err(e) => {
            event("disconnected");
            failure(e) as i64
        }
    }
}
/// Half-close and drain queued input before exit; dropping unread TCP input can send RST on Windows.
#[no_mangle]
pub extern "C" fn eh_netplay_finish() {
    let Some(session) = SESSION.get() else {
        return;
    };
    if let Ok(mut peer) = session.lock() {
        let _ = peer.stream.socket().shutdown(std::net::Shutdown::Write);
        let end = Instant::now() + Duration::from_millis(300);
        let mut pending = [0; 256];
        while Instant::now() < end {
            match peer.stream.read(&mut pending) {
                Ok(0) => break,
                Ok(_) => {}
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock
                            | io::ErrorKind::TimedOut
                            | io::ErrorKind::Interrupted
                    ) => {}
                Err(_) => break,
            }
        }
        let _ = peer.stream.socket().shutdown(std::net::Shutdown::Both);
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn pair() -> (Peer, Peer) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let a = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let b = listener.accept().unwrap().0;
        for s in [&a, &b] {
            s.set_nodelay(true).unwrap();
            s.set_read_timeout(Some(Duration::from_millis(100)))
                .unwrap();
        }
        (
            Peer {
                stream: a.into(),
                host: true,
                inputs: [0; DELAY],
                pictures: [0; DELAY],
            },
            Peer {
                stream: b.into(),
                host: false,
                inputs: [0; DELAY],
                pictures: [0; DELAY],
            },
        )
    }
    #[test]
    fn two_peers_receive_the_same_two_player_inputs_in_order() {
        let (mut host, mut guest) = pair();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
        let guest_barrier = barrier.clone();
        let other = std::thread::spawn(move || {
            let inputs = (0..240)
                .map(|f| guest.exchange(f, 0x8000 | (f as u16), f).unwrap())
                .collect::<Vec<_>>();
            guest_barrier.wait();
            inputs
        });
        let inputs = (0..240)
            .map(|f| host.exchange(f, 0x4000 | (f as u16), f).unwrap())
            .collect::<Vec<_>>();
        barrier.wait();
        assert_eq!(inputs, other.join().unwrap());
        assert_eq!(&inputs[..DELAY], &[0; DELAY]);
        for (f, input) in inputs.iter().enumerate().skip(DELAY) {
            let n = (f - DELAY) as i64;
            assert_eq!(*input, (0x4000 | n) | ((0x8000 | n) << 16));
        }
    }
    #[test]
    fn mismatched_picture_and_disconnection_stop_exchange() {
        let (mut host, mut guest) = pair();
        let other = std::thread::spawn(move || {
            for f in 0..121 {
                if guest.exchange(f, 0, f + 1).is_err() {
                    return;
                }
            }
            panic!("Mismatch not rejected");
        });
        let mut rejected = false;
        for f in 0..121 {
            if host.exchange(f, 0, f).is_err() {
                rejected = true;
                break;
            }
        }
        assert!(rejected);
        other.join().unwrap();
        let (mut host, guest) = pair();
        drop(guest);
        for f in 0..DELAY as u32 {
            let _ = host.exchange(f, 0, 0);
        }
        assert!(host.exchange(DELAY as u32, 0, 0).is_err());
    }
}
