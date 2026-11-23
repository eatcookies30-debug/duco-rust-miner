//! The mining rig master on a serial port.
//!
//! Only the rig master is connected to the PC. It drives the I2C bus and
//! shuttles jobs to every board hanging off it (firmware/i2c_rig explains the
//! protocol on the board side). The miner opens its own pool connection for
//! each board on the bus, so every board is its own miner at the pool - the
//! same thing the reference rig masters do.
//!
//! Lines the master understands:
//!     MODE rig                         stop the self-test, take jobs from here
//!     GIVE <addr> <prev>,<target>,<diff>
//!     PING                             keep-alive, answers PONG
//!
//! Lines the master sends:
//!     RIG <addr> <addr> ...            the boards on the bus
//!     SHARE <addr> <nonce>,<elapsed>,<DUCOID>
//!     BUSY <addr>                      that board is still searching
//!     MISS <addr> <reason>             that board never answered
//!     STAT <report>                    bus health, only when something changed
//!
//! Unlike the serial protocol of a board, the numbers on the bus are plain
//! decimal.

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Default)]
struct State {
    slaves: Vec<u8>,
    shares: HashMap<u8, (u64, u64, String)>,
    misses: HashMap<u8, String>,
    // The master's own bus health report, empty until it has something to say.
    stat: String,
}

pub struct Rig {
    pub port: String,
    tx: Mutex<std::fs::File>,
    state: Arc<Mutex<State>>,
}

impl Rig {
    pub fn open(port: &str) -> Result<Rig, String> {
        let status = Command::new("stty")
            .arg("-F")
            .arg(port)
            .arg("115200")
            .arg("raw")
            .arg("-echo")
            .status();
        if !status.unwrap_or_default().success() {
            return Err(format!("could not set baud on {port} (is the rig master connected?)"));
        }

        /* Two handles on the same tty: the reader thread blocks in read(), and
           sharing one handle with the job writer would put a line behind that
           block. Opening the port toggles DTR and resets the master, so both
           handles are opened before anything is said. */
        let mut reader = OpenOptions::new()
            .read(true)
            .open(port)
            .map_err(|e| format!("could not open {port}: {e}"))?;
        let writer = OpenOptions::new()
            .write(true)
            .open(port)
            .map_err(|e| format!("could not open {port} for writing: {e}"))?;

        let rig = Rig {
            port: port.to_string(),
            tx: Mutex::new(writer),
            state: Arc::new(Mutex::new(State::default())),
        };

        let state = rig.state.clone();
        let port_display = port.to_string();
        thread::spawn(move || {
            let debug = std::env::var("DUCO_DEBUG").is_ok();
            let started = Instant::now();
            let mut line = String::new();
            let mut buf = [0u8; 512];
            loop {
                match reader.read(&mut buf) {
                    Ok(0) => thread::sleep(Duration::from_millis(20)),
                    Ok(n) => {
                        if debug {
                            eprintln!(
                                "[{port_display} rx t={:.1}s] {:?}",
                                started.elapsed().as_secs_f64(),
                                &buf[..n]
                            );
                        }
                        for &b in &buf[..n] {
                            if b == b'\n' {
                                note(&state, &line);
                                line.clear();
                            } else if b != b'\r' {
                                line.push(b as char);
                            }
                        }
                    }
                    Err(_) => return,
                }
            }
        });

        /* Opening the port resets the master, and it spends the first couple of
           seconds walking the bus. The mode line is sent after that settles, and
           twice, because a line that lands while the board is still coming up is
           simply lost. */
        thread::sleep(Duration::from_millis(2500));
        rig.send("MODE rig\n")?;
        thread::sleep(Duration::from_millis(300));
        rig.send("MODE rig\n")?;

        /* The master's self-test is already handing out jobs when the port is
           opened, so a reply to one of those jobs can land in the queue before
           the miner has asked for anything. Those replies are dropped rather than
           read as shares: the master stops self-testing on the MODE line, and
           anything still in flight is ignored on its way up. */
        thread::sleep(Duration::from_millis(200));
        {
            let mut state = rig.state.lock().unwrap();
            state.shares.clear();
            state.misses.clear();
        }
        Ok(rig)
    }

    /// The boards the master has on the bus. Empty until the master reports
    /// them, which takes a couple of seconds after the port is opened.
    pub fn slaves(&self) -> Vec<u8> {
        self.state.lock().unwrap().slaves.clone()
    }

    /// The master's last bus health report, as it printed it. Empty while the
    /// bus is clean: the master only reports when bytes were lost or a board
    /// stopped answering.
    pub fn stat(&self) -> String {
        self.state.lock().unwrap().stat.clone()
    }

    /// Wait for the master to report at least one board.
    pub fn wait_for_slaves(&self, timeout: Duration) -> Vec<u8> {
        let start = Instant::now();
        loop {
            let slaves = self.slaves();
            if !slaves.is_empty() || start.elapsed() >= timeout {
                return slaves;
            }
            thread::sleep(Duration::from_millis(200));
        }
    }

    fn send(&self, line: &str) -> Result<(), String> {
        let mut file = self.tx.lock().unwrap();
        /* The master keeps the bus busy and its serial buffer is only 64 bytes,
           so a whole job line has to be fed in small pieces with time to drain
           between them. */
        let bytes = line.as_bytes();
        let mut written = 0;
        while written < bytes.len() {
            let n = (bytes.len() - written).min(16);
            file.write_all(&bytes[written..written + n])
                .map_err(|e| format!("write to {} failed: {e}", self.port))?;
            file.flush().ok();
            written += n;
            if written < bytes.len() {
                thread::sleep(Duration::from_millis(8));
            }
        }
        Ok(())
    }

    pub fn ping(&self) -> Result<(), String> {
        self.send("PING\n")
    }

    /// Hand a job to one board. The master answers BUSY when that board is
    /// still working on the previous job, so the caller waits and tries again.
    pub fn give(&self, address: u8, prev: &str, target: &str, difficulty: u32) -> Result<(), String> {
        if std::env::var("DUCO_DEBUG").is_ok() {
            eprintln!("[rig tx] GIVE {address} diff {difficulty}");
        }
        self.send(&format!("GIVE {address} {prev},{target},{difficulty}\n"))
    }

    /// Wait for the share this board sends back. Errors if the master says the
    /// board went silent, or nothing comes back inside the allowance.
    pub fn take(&self, address: u8, allowance: Duration) -> Result<(u64, u64, String), String> {
        let start = Instant::now();
        loop {
            {
                let mut s = self.state.lock().unwrap();
                if let Some(share) = s.shares.remove(&address) {
                    return Ok(share);
                }
                if let Some(reason) = s.misses.remove(&address) {
                    return Err(format!("board {address}: {reason}"));
                }
            }
            if start.elapsed() >= allowance {
                return Err(format!(
                    "board {address} did not answer within {}s",
                    allowance.as_secs()
                ));
            }
            thread::sleep(Duration::from_millis(50));
        }
    }
}

/* One line from the rig master. Shares, board lists, misses and bus health
   reports are kept; anything else is the self-test chatter of the master and is
   ignored. */
fn note(state: &Arc<Mutex<State>>, line: &str) {
    let line = line.trim();
    if line.is_empty() {
        return;
    }
    let mut parts = line.split_whitespace();
    let kind = match parts.next() {
        Some(k) => k,
        None => return,
    };
    match kind {
        "RIG" => {
            let addresses: Vec<u8> = parts.filter_map(|p| p.parse::<u8>().ok()).collect();
            let mut s = state.lock().unwrap();
            s.slaves = addresses;
        }
        "SHARE" => {
            let address = match parts.next().and_then(|a| a.parse::<u8>().ok()) {
                Some(a) => a,
                None => return,
            };
            let rest = match parts.next() {
                Some(r) => r,
                None => return,
            };
            let fields: Vec<&str> = rest.split(',').collect();
            if fields.len() < 2 {
                return;
            }
            let nonce = match fields[0].parse::<u64>() {
                Ok(n) => n,
                Err(_) => return,
            };
            let elapsed = match fields[1].parse::<u64>() {
                Ok(e) => e,
                Err(_) => return,
            };
            let ducoid = fields.get(2).unwrap_or(&"").to_string();
            state.lock().unwrap().shares.insert(address, (nonce, elapsed, ducoid));
        }
        "STAT" => {
            /* The master's own bus health report: how many bytes of job text a
               board refused, how many boards went silent. Kept so the status
               line can show it. */
            let report = parts.collect::<Vec<_>>().join(" ");
            state.lock().unwrap().stat = report;
        }
        "MISS" => {
            let address = match parts.next().and_then(|a| a.parse::<u8>().ok()) {
                Some(a) => a,
                None => return,
            };
            let reason = parts.collect::<Vec<_>>().join(" ");
            state
                .lock()
                .unwrap()
                .misses
                .insert(address, if reason.is_empty() { "no answer".to_string() } else { reason });
        }
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_share_is_stored_by_address() {
        let state = Arc::new(Mutex::new(State::default()));
        note(&state, "SHARE 3 9999,5689744,DUCOIDFFDFFFDFFFFFFF03");
        let s = state.lock().unwrap();
        let share = s.shares.get(&3).unwrap();
        assert_eq!(share.0, 9999);
        assert_eq!(share.1, 5689744);
        assert_eq!(share.2, "DUCOIDFFDFFFDFFFFFFF03");
    }

    #[test]
    fn the_board_list_is_parsed() {
        let state = Arc::new(Mutex::new(State::default()));
        note(&state, "RIG 1 2 3 4 5 6");
        assert_eq!(state.lock().unwrap().slaves, vec![1, 2, 3, 4, 5, 6]);
    }

    #[test]
    fn a_miss_is_reported_as_a_reason() {
        let state = Arc::new(Mutex::new(State::default()));
        note(&state, "MISS 7 silent");
        assert_eq!(state.lock().unwrap().misses.get(&7).unwrap(), "silent");
    }

    #[test]
    fn self_test_chatter_is_ignored() {
        let state = Arc::new(Mutex::new(State::default()));
        note(&state, "Totals: 6 slaves, 58 jobs, 49 correct");
        let s = state.lock().unwrap();
        assert!(s.shares.is_empty() && s.slaves.is_empty() && s.misses.is_empty());
    }

    #[test]
    fn a_bus_health_report_is_kept() {
        let state = Arc::new(Mutex::new(State::default()));
        note(
            &state,
            "STAT 6 boards, 120 jobs sent, 118 shares, 0 jobs never answered, \
             2 searches cut short, 3 of 11040 job bytes refused by a board, \
             board 4 refused 3, 10542 H/s across 6 mining boards",
        );
        let s = state.lock().unwrap();
        assert!(s.stat.starts_with("6 boards, 120 jobs sent"));
        assert!(s.shares.is_empty() && s.misses.is_empty());
    }
}
