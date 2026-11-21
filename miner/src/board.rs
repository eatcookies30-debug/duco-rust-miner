//! Serial driver for the Duino-Coin board firmware.
//!
//! The board protocol is one line in, one line out:
//!     <prevHash>,<targetHash>,<difficulty>,\n
//!     <nonce in binary>,<elapsed us in binary>,<DUCOID>\n
//!
//! Reading is done by a background thread feeding complete lines to a channel,
//! so the mining loop can enforce a hard deadline on a board that stops
//! answering instead of blocking forever (the Python miner has no such timeout
//! on the serial read).

use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::sync::mpsc::{self, Receiver};
use std::sync::Mutex;
use std::process::Command;
use std::thread;
use std::time::Duration;

pub struct Board {
    pub port: String,
    file: Mutex<std::fs::File>,
    rx: Receiver<String>,
}

impl Board {
    pub fn open(port: &str) -> Result<Board, String> {
        // The kernel tty needs the right baud; the boards always run at 115200.
        let status = Command::new("stty")
            .arg("-F")
            .arg(port)
            .arg("115200")
            .status();
        if !status.unwrap_or_default().success() {
            return Err(format!("could not set baud on {port} (is it connected?)"));
        }

        let file = match OpenOptions::new().read(true).write(true).open(port) {
            Ok(f) => f,
            Err(e) => return Err(format!("could not open {port}: {e}")),
        };

        let (tx, rx) = mpsc::channel();
        /* A duplicate of the same file descriptor, held by the reader thread so
           the mining thread can write without queueing behind a blocked read. */
        let mut reader = file.try_clone().map_err(|e| format!("{port}: {e}"))?;
        let port_display = port.to_string();
        let port_log = port_display.clone();
        thread::spawn(move || {
            let debug = std::env::var("DUCO_DEBUG").is_ok();
            let started = std::time::Instant::now();
            let mut pending = String::new();
            let mut buf = [0u8; 256];
            loop {
                match reader.read(&mut buf) {
                    Ok(0) => thread::sleep(Duration::from_millis(5)),
                    Ok(n) => {
                        if debug {
                            eprintln!(
                                "[{port_log} rx t={:.1}s] {:?}",
                                started.elapsed().as_secs_f64(),
                                &buf[..n]
                            );
                        }
                        for &b in &buf[..n] {
                            if b == b'\n' {
                                if tx.send(pending.clone()).is_err() {
                                    return;
                                }
                                pending.clear();
                            } else if b != b'\r' {
                                pending.push(b as char);
                            }
                        }
                    }
                    Err(_) => return,
                }
            }
        });

        let board = Board {
            port: port_display,
            file: Mutex::new(file),
            rx,
        };

        // Opening the port toggles DTR, which resets an AVR board. Give the
        // sketch time to boot before we ask it for anything.
        thread::sleep(Duration::from_millis(2500));
        board.drain();
        Ok(board)
    }

    fn drain(&self) {
        // Throw away anything the board said before we asked for a job.
        while self.rx.recv_timeout(Duration::from_millis(400)).is_ok() {}
    }

    /// Drop any reply the board sent for a job we already gave up on.
    pub fn discard_pending(&self) {
        while self.rx.recv_timeout(Duration::from_millis(400)).is_ok() {}
    }

    pub fn send_job(&self, prev: &str, target: &str, difficulty: u32) -> Result<(), String> {
        let line = format!("{prev},{target},{difficulty},\n");
        let mut file = self.file.lock().map_err(|_| "board closed".to_string())?;
        file.write_all(line.as_bytes())
            .map_err(|e| format!("write to {} failed: {e}", self.port))?;
        file.flush().ok();
        Ok(())
    }

    /// Read one reply line from the board.
    pub fn read_reply(&self, allowance: Duration) -> Result<(u64, u64, String), String> {
        match self.rx.recv_timeout(allowance) {
            Ok(reply) => {
                if reply.is_empty() {
                    return Err(format!("empty reply from {}", self.port));
                }
                let parts: Vec<&str> = reply.split(',').collect();
                if parts.len() < 2 {
                    return Err(format!("bad board reply: {reply}"));
                }
                let nonce = parse_binary(parts[0])?;
                let elapsed = parse_binary(parts[1])?;
                let ducoid = parts.get(2).unwrap_or(&"").to_string();
                Ok((nonce, elapsed, ducoid))
            }
            Err(_) => Err(format!(
                "no reply from {} within {}s",
                self.port,
                allowance.as_secs()
            )),
        }
    }

    /* Whatever went wrong, the surest recovery is to reset the board: opening
       the port toggles DTR, which restarts the sketch with an empty serial
       buffer. Without this a board that mis-frames one job can sit searching a
       stale target for minutes and look dead. */
    pub fn reopen(&self) -> Result<(), String> {
        let file = OpenOptions::new()
            .write(true)
            .open(&self.port)
            .map_err(|e| format!("could not reopen {}: {e}", self.port))?;
        let mut guard = self.file.lock().map_err(|_| "board closed".to_string())?;
        *guard = file;
        drop(guard);
        thread::sleep(Duration::from_millis(2500));
        self.drain();
        Ok(())
    }
}

/// The same protocol through the rig master: the master is the only board on
/// the USB side of a rig, and it shuttles jobs to the boards on the I2C bus.
pub struct RigBoard {
    rig: std::sync::Arc<crate::rig::Rig>,
    pub address: u8,
}

impl RigBoard {
    pub fn new(rig: std::sync::Arc<crate::rig::Rig>, address: u8) -> RigBoard {
        RigBoard { rig, address }
    }
}

/// A board the miner can hand a job to, whether it is plugged into the PC or
/// hanging off a rig master.
pub trait Link {
    fn send(&self, prev: &str, target: &str, difficulty: u32) -> Result<(), String>;
    fn reply(&self, allowance: Duration) -> Result<(u64, u64, String), String>;
    fn reopen(&self) -> Result<(), String>;
}

impl Link for Board {
    fn send(&self, prev: &str, target: &str, difficulty: u32) -> Result<(), String> {
        self.discard_pending();
        self.send_job(prev, target, difficulty)
    }
    fn reply(&self, allowance: Duration) -> Result<(u64, u64, String), String> {
        self.read_reply(allowance)
    }
    fn reopen(&self) -> Result<(), String> {
        self.reopen()
    }
}

impl Link for RigBoard {
    fn send(&self, prev: &str, target: &str, difficulty: u32) -> Result<(), String> {
        self.rig.give(self.address, prev, target, difficulty)
    }
    fn reply(&self, allowance: Duration) -> Result<(u64, u64, String), String> {
        self.rig.take(self.address, allowance)
    }
    fn reopen(&self) -> Result<(), String> {
        self.rig.ping()
    }
}

/// The firmware prints the nonce and the elapsed time in base 2.
fn parse_binary(s: &str) -> Result<u64, String> {
    if s.is_empty() {
        return Err("empty field".into());
    }
    let mut v = 0u64;
    for c in s.bytes() {
        match c {
            b'0' | b'1' => v = v * 2 + (c - b'0') as u64,
            _ => return Err(format!("not binary: {s}")),
        }
    }
    Ok(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn binary_parses() {
        assert_eq!(parse_binary("1100010").unwrap(), 98);
        assert_eq!(parse_binary("1010101").unwrap(), 85);
        assert!(parse_binary("abc").is_err());
    }
}
