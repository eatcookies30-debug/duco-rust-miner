//! Pool client: node discovery over plain HTTP and the line protocol on the
//! pool socket. No external crates, so the JSON is parsed with string
//! operations rather than a parser library.

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::{Duration, Instant};

pub const SEPARATOR: &str = ",";
pub const MINER_NAME: &str = concat!(
    "Duino-Coin AVR Miner (Rust) ",
    env!("CARGO_PKG_VERSION")
);

pub struct Pool {
    pub host: String,
    pub port: u16,
    /// Round trip of the protocol greeting, measured when the socket is opened.
    pub latency: u32,
    timeout: Duration,
    sock: TcpStream,
    leftover: String,
}

pub fn discover() -> Result<(String, u16, String), String> {
    // The pool picker only speaks HTTP on port 80, which keeps this binary free
    // of a TLS dependency.
    let body = http_get("server.duinocoin.com", 80, "/getPool")
        .unwrap_or_else(|_| String::new());
    if let Some((ip, port, name)) = parse_pool(&body) {
        return Ok((ip, port, name));
    }
    // Static fallback from the master node if the picker is unreachable.
    Ok(("152.53.241.160".to_string(), 7913, "master2".to_string()))
}

fn parse_pool(body: &str) -> Option<(String, u16, String)> {
    let ip = extract(body, "\"ip\"")?;
    let port: u16 = extract(body, "\"port\"")?.parse().ok()?;
    let name = extract(body, "\"name\"").unwrap_or_else(|| "unknown".to_string());
    Some((ip, port, name))
}

fn extract(body: &str, key: &str) -> Option<String> {
    let at = body.find(key)?;
    let rest = &body[at + key.len()..];
    let colon = rest.find(':')?;
    let val = rest[colon + 1..].trim_start();
    if val.starts_with('"') {
        let end = val[1..].find('"')?;
        Some(val[1..=end].to_string())
    } else {
        let end = val
            .find(|c: char| !c.is_ascii_digit())
            .unwrap_or(val.len());
        Some(val[..end].to_string())
    }
}

fn url_encode(s: &str) -> String {
    let mut out = String::new();
    for c in s.chars() {
        if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' {
            out.push(c);
        } else {
            for b in c.to_string().bytes() {
                out.push_str(&format!("%{b:02x}", b = b));
            }
        }
    }
    out
}

/// Ask the account service whether `user` has a mining key, and whether the
/// supplied `key` is the right one. Returns (accepted, account_has_a_key).
pub fn key_status(user: &str, key: &str) -> Result<(bool, bool), String> {
    let path = format!(
        "/mining_key?u={}",
        url_encode(user),
    );
    let path = if key == "None" || key.is_empty() {
        path
    } else {
        format!("{path}&k={}", url_encode(key))
    };
    let body = http_get("server.duinocoin.com", 80, &path)?;
    let success = body.contains("\"success\":true");
    let has_key = body.contains("\"has_key\":true");
    Ok((success, has_key))
}

fn http_get(host: &str, port: u16, path: &str) -> Result<String, String> {
    let addr = (host, port)
        .to_socket_addrs()
        .map_err(|e| format!("dns failed for {host}: {e}"))?
        .next()
        .ok_or("no address")?;
    let mut sock = TcpStream::connect(addr).map_err(|e| format!("connect {host}: {e}"))?;
    sock.set_read_timeout(Some(Duration::from_secs(10)))
        .map_err(|e| e.to_string())?;
    sock.set_write_timeout(Some(Duration::from_secs(10)))
        .map_err(|e| e.to_string())?;
    let req = format!(
        "GET {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: {MINER_NAME}\r\nConnection: close\r\n\r\n"
    );
    sock.write_all(req.as_bytes()).map_err(|e| e.to_string())?;
    let mut body = String::new();
    let mut buf = [0u8; 4096];
    while let Ok(n) = sock.read(&mut buf) {
        if n == 0 {
            break;
        }
        body.push_str(&String::from_utf8_lossy(&buf[..n]));
    }
    let cut = body.find("\r\n\r\n").map(|i| i + 4).unwrap_or(body.len());
    Ok(body[cut..].to_string())
}

impl Pool {
    pub fn connect(host: &str, port: u16, timeout: Duration) -> Result<Pool, String> {
        let addr = (host, port)
            .to_socket_addrs()
            .map_err(|e| format!("dns failed for {host}: {e}"))?
            .next()
            .ok_or("no address")?;
        let sock = TcpStream::connect(addr).map_err(|e| format!("connect {host}:{port}: {e}"))?;
        sock.set_read_timeout(Some(timeout))
            .map_err(|e| e.to_string())?;
        sock.set_write_timeout(Some(timeout))
            .map_err(|e| e.to_string())?;
        let mut p = Pool {
            host: host.to_string(),
            port,
            latency: 0,
            timeout,
            sock,
            leftover: String::new(),
        };
        // The server greets us with its protocol version before any request;
        // that round trip is the closest thing to a ping the protocol has.
        let start = Instant::now();
        let _ = p.recv(64);
        p.latency = (Instant::now() - start).as_micros() as u32 / 1000;
        Ok(p)
    }

    /// Open a fresh socket to the same node. The node hangs up on a connection
    /// that has been idle too long, so a reconnect is just a new socket with the
    /// reply buffer emptied.
    pub fn reconnect(&mut self) -> Result<(), String> {
        let addr = (self.host.as_str(), self.port)
            .to_socket_addrs()
            .map_err(|e| format!("dns failed for {}: {e}", self.host))?
            .next()
            .ok_or("no address")?;
        let sock = TcpStream::connect(addr)
            .map_err(|e| format!("connect {}:{}: {e}", self.host, self.port))?;
        sock.set_read_timeout(Some(self.timeout))
            .map_err(|e| e.to_string())?;
        sock.set_write_timeout(Some(self.timeout))
            .map_err(|e| e.to_string())?;
        self.sock = sock;
        self.leftover.clear();
        let _ = self.recv(64);
        Ok(())
    }

    pub fn motd(&mut self) -> String {
        self.send("MOTD");
        self.recv(1024).unwrap_or_default()
    }

    /// Request a job for `user`. The tier is the difficulty class the board is
    /// expected to handle: AVR / MEGA / ARM / DUE.
    pub fn job(&mut self, user: &str, tier: &str, key: &str) -> Result<(String, String, u32), String> {
        self.send(&format!("JOB{SEPARATOR}{user}{SEPARATOR}{tier}{SEPARATOR}{key}"));
        let raw = self.recv(128)?;
        let parts: Vec<&str> = raw.split(SEPARATOR).collect();
        if parts.len() < 3 {
            return Err(format!("unexpected job reply: {raw}"));
        }
        let diff = parts[2].parse().map_err(|_| format!("bad difficulty in {raw}"))?;
        Ok((parts[0].to_string(), parts[1].to_string(), diff))
    }

    pub fn submit(&mut self, nonce: u64, hashrate: f64, rigid: &str, ducoid: &str) -> Result<Vec<String>, String> {
        self.send(&format!(
            "{nonce}{SEPARATOR}{hashrate}{SEPARATOR}{MINER_NAME}{SEPARATOR}{rigid}{SEPARATOR}{ducoid}"
        ));
        let raw = self.recv(64)?;
        Ok(raw.split(SEPARATOR).map(|s| s.to_string()).collect())
    }

    fn send(&mut self, msg: &str) {
        if let Err(e) = self.sock.write_all(msg.as_bytes()) {
            eprintln!("pool write failed: {e}");
        }
    }

    /// One reply line. The node sends short lines, but they can arrive split
    /// across packets or bunched together, so anything past the first newline
    /// is kept in `leftover` for the next call instead of being thrown away.
    fn recv(&mut self, _limit: usize) -> Result<String, String> {
        loop {
            if let Some(nl) = self.leftover.find('\n') {
                let line = self.leftover[..nl].to_string();
                self.leftover = self.leftover[nl + 1..].to_string();
                return Ok(line.trim_end().to_string());
            }
            let mut buf = [0u8; 256];
            match self.sock.read(&mut buf) {
                Ok(0) => {
                    // The node closed the connection. If it answered without a
                    // trailing newline, use what we already have.
                    if !self.leftover.is_empty() {
                        let line = std::mem::take(&mut self.leftover);
                        return Ok(line.trim_end().to_string());
                    }
                    return Err("pool closed the connection".to_string());
                }
                Ok(n) => self.leftover.push_str(&String::from_utf8_lossy(&buf[..n])),
                Err(e) => {
                    // The node does not always terminate its reply with a
                    // newline (the MOTD greeting does not), so if the socket
                    // goes quiet after we already have a reply, use it.
                    if !self.leftover.is_empty() {
                        let line = std::mem::take(&mut self.leftover);
                        return Ok(line.trim_end().to_string());
                    }
                    return Err(format!("pool read failed: {e}"));
                }
            }
        }
    }
}
