use std::env;
use std::fs;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

mod board;
mod mock;
mod pool;
mod sha1;

use board::Board;
use pool::{discover, key_status, Pool};
use sha1::{hex, sha1};

#[derive(Clone)]
struct Config {
    user: String,
    key: String,
    ports: Vec<String>,
    pool: Option<(String, u16)>,
    report_time: u64,
    board_timeout: Duration,
    pool_timeout: Duration,
    rounds: u32,
    // Rig identifiers from Settings.cfg `identifier = ...`, one per board.
    // Sent with every share, the same way the official AVR miner does.
    identifiers: Vec<String>,
    mock: bool,
    mock_bind: String,
    mock_port: u16,
    bench: bool,
    ping: bool,
    // Force the difficulty class sent to the pool (AVR/ARM/MEGA/DUE) instead
    // of deriving it from the measured hashrate.
    tier: Option<String>,
}

#[derive(Default)]
struct Totals {
    accepted: u32,
    rejected: u32,
    blocks: u32,
    per_board: Vec<f64>,
}

/* Only one duco-miner may own the boards at a time: two processes reading the
   same serial port make each other's jobs unreadable. The lock is a file that
   holds the pid of the miner that holds it; a stale lock (process gone) is
   taken over. */
fn take_lock(path: &str) -> Result<(), String> {
    match fs::read_to_string(path) {
        Ok(text) => {
            let pid: i32 = match text.trim().parse() {
                Ok(p) => p,
                Err(_) => return Err(format!("lock file {path} is corrupt - delete it")),
            };
            if std::path::Path::new(&format!("/proc/{pid}")).exists() {
                return Err(format!(
                    "another duco-miner (pid {pid}) already has these boards; \
                     stop it or remove {path}"
                ));
            }
            println!("taking over a stale lock at {path} (pid {pid} is gone)");
        }
        Err(_) => {}
    }
    fs::write(path, format!("{}\n", std::process::id()))
        .map_err(|e| format!("cannot write lock file {path}: {e}"))?;
    Ok(())
}

fn release_lock(path: &str) {
    if let Ok(text) = fs::read_to_string(path) {
        if text.trim() == format!("{}", std::process::id()) {
            let _ = fs::remove_file(path);
        }
    }
}

fn main() {
    let cfg = match parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("{e}");
            usage();
            std::process::exit(2);
        }
    };

    if cfg.ping {
        ping_node(&cfg);
        return;
    }

    if cfg.ports.is_empty() {
        eprintln!("no serial ports given; use --ports /dev/ttyACM0,/dev/ttyACM1");
        std::process::exit(1);
    }

    let lock = format!(
        "/tmp/duco-miner-{}.lock",
        cfg.ports.join("_").replace('/', "-")
    );
    if let Err(e) = take_lock(&lock) {
        eprintln!("{e}");
        std::process::exit(1);
    }

    if cfg.bench {
        bench(&cfg);
        return;
    }

    if cfg.mock {
        // Offline end-to-end test: the mock node and the miner run together.
        let handle = {
            let c = cfg.clone();
            thread::spawn(move || {
                mock::run(mock::MockConfig {
                    bind: c.mock_bind.clone(),
                    port: c.mock_port,
                    rounds: if c.rounds == 0 { 20 } else { c.rounds },
                    tier_difficulty: (10, 65, 300, 655),
                })
            })
        };
        let _ = handle;
        thread::sleep(Duration::from_millis(400));
        mine(&Config {
            pool: Some((cfg.mock_bind.clone(), cfg.mock_port)),
            ..cfg
        });
        release_lock(&lock);
        return;
    }

    mine(&cfg);
    release_lock(&lock);
}

fn usage() {
    println!(
        "usage: duco-miner [--config Settings.cfg] [--user NAME] [--key KEY] \
                 [--ports /dev/ttyACM0,/dev/ttyACM1] [--pool ip:port] \
                 [--report-time SECS] [--board-timeout SECS] [--rounds N] [--version]\n\
        \n\
        offline helpers:\n\
        \x20 duco-miner --mock                    mock pool node + miner, no account needed\n\
        \x20 duco-miner --bench                   hashrate benchmark straight against the boards\n\
        \x20 duco-miner --ping                    print the pool node the picker gives and its latency\n"
    );
}

fn parse_args() -> Result<Config, String> {
    let args: Vec<String> = env::args().collect();
    let mut cfg = Config {
        user: String::new(),
        key: "None".to_string(),
        ports: Vec::new(),
        pool: None,
        report_time: 60,
        board_timeout: Duration::from_secs(120),
        pool_timeout: Duration::from_secs(15),
        rounds: 0,
        mock: false,
        mock_bind: "127.0.0.1".to_string(),
        mock_port: 7913,
        bench: false,
        ping: false,
        identifiers: Vec::new(),
        tier: None,
    };

    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        let value = |i: usize| -> Result<String, String> {
            args.get(i + 1)
                .cloned()
                .ok_or_else(|| format!("{a} needs a value"))
        };
        // options that take a value consume two slots
        match a.as_str() {
            "--config" => {
                apply_config(&mut cfg, &value(i)?)?;
                i += 1;
            }
            "--user" => {
                cfg.user = value(i)?;
                i += 1;
            }
            "--key" | "--mining-key" => {
                cfg.key = normalise_key(&value(i)?);
                i += 1;
            }
            "--ports" => {
                cfg.ports = value(i)?.split(',').map(|s| s.trim().to_string()).collect();
                i += 1;
            }
            "--pool" => {
                let v = value(i)?;
                let (h, p) = v.rsplit_once(':').ok_or("--pool expects ip:port")?;
                cfg.pool = Some((h.to_string(), p.parse().map_err(|_| "bad port")?));
                i += 1;
            }
            "--report-time" => {
                cfg.report_time = value(i)?.parse().map_err(|_| "bad number")?;
                i += 1;
            }
            "--board-timeout" => {
                cfg.board_timeout = Duration::from_secs(value(i)?.parse().map_err(|_| "bad number")?);
                i += 1;
            }
            "--rounds" => {
                cfg.rounds = value(i)?.parse().map_err(|_| "bad number")?;
                i += 1;
            }
            "--tier" => {
                cfg.tier = Some(value(i)?.to_ascii_uppercase());
                i += 1;
            }
            "--mock-port" => {
                cfg.mock_port = value(i)?.parse().map_err(|_| "bad number")?;
                i += 1;
            }
            "--mock" => cfg.mock = true,
            "--bench" => cfg.bench = true,
            "--ping" => cfg.ping = true,
            "--version" | "-V" => {
                println!(
                    "duco-miner {} - DUCO-S1 AVR miner, Rust standard library only",
                    env!("CARGO_PKG_VERSION")
                );
                std::process::exit(0);
            }
            "help" | "--help" | "-h" => {
                usage();
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
        i += 1;
    }
    if let Ok(k) = env::var("DUCO_MINING_KEY") {
        cfg.key = normalise_key(&k);
    }
    Ok(cfg)
}

fn apply_config(cfg: &mut Config, path: &str) -> Result<(), String> {
    let text = fs::read_to_string(path).map_err(|e| format!("cannot read {path}: {e}"))?;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('[') || line.starts_with('#') {
            continue;
        }
        let (k, v) = match line.split_once('=') {
            Some(p) => p,
            None => continue,
        };
        let v = v.trim();
        match k.trim() {
            "username" => cfg.user = v.to_string(),
            "avrport" => cfg.ports = v.split(',').map(|s| s.trim().to_string()).collect(),
            "identifier" => cfg.identifiers = v.split(',').map(|s| s.trim().to_string()).collect(),
            "mining_key" => cfg.key = normalise_key(v),
            "report_time" | "periodic_report" => cfg.report_time = v.parse().unwrap_or(60),
            "avr_timeout" => {
                // seconds the board is given to answer one job
                cfg.board_timeout = Duration::from_secs(v.parse().unwrap_or(120))
            }
            "soc_timeout" => {
                // seconds the pool socket is given to answer one request
                cfg.pool_timeout = Duration::from_secs(v.parse().unwrap_or(15))
            }
            _ => {}
        }
    }
    Ok(())
}

/// The official miner stores the mining key base64-encoded in Settings.cfg and
/// decodes it before talking to the pool, so accept both forms here: a key
/// copied out of the official config and a key typed straight into the file.
fn normalise_key(raw: &str) -> String {
    let raw = raw.trim();
    if raw.is_empty() || raw == "None" {
        return "None".to_string();
    }
    if let Some(decoded) = base64_decode(raw) {
        if !decoded.is_empty() && decoded.chars().all(|c| c.is_ascii_graphic() || c == ' ') {
            return decoded;
        }
    }
    raw.to_string()
}

fn base64_decode(s: &str) -> Option<String> {
    fn val(c: char) -> Option<u8> {
        match c {
            'A'..='Z' => Some(c as u8 - b'A'),
            'a'..='z' => Some(c as u8 - b'a' + 26),
            '0'..='9' => Some(c as u8 - b'0' + 52),
            '+' => Some(62),
            '/' => Some(63),
            _ => None,
        }
    }
    let chars: Vec<char> = s
        .chars()
        .filter(|c| *c != '=' && !c.is_whitespace())
        .collect();
    if chars.is_empty() || chars.iter().any(|c| val(*c).is_none()) {
        return None;
    }
    let mut out = Vec::new();
    for chunk in chars.chunks(4) {
        let mut acc = 0u32;
        for &c in chunk {
            acc = (acc << 6) | val(c)? as u32;
        }
        // A full group of 4 characters carries 3 bytes; a short trailing group
        // carries len-1 bytes, the leftover low bits being padding.
        let nbytes = chunk.len() - 1;
        for i in 0..nbytes {
            out.push(((acc >> (chunk.len() * 6 - 8 * (i + 1))) & 0xff) as u8);
        }
    }
    String::from_utf8(out).ok()
}

/// Difficulty class the pool uses for a board, derived the same way the
/// official miner does: from the 50-hash speed test.
fn tier_for(rate: f64) -> &'static str {
    if rate > 1000.0 {
        "DUE"
    } else if rate > 550.0 {
        "ARM"
    } else if rate > 380.0 {
        "MEGA"
    } else {
        "AVR"
    }
}

const TEST_PREV: &str = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";

fn sha1_hex(s: &str) -> String {
    hex(&sha1(s.as_bytes()))
}

fn bench(cfg: &Config) {
    println!("offline board benchmark (no network, no account)");
    for port in &cfg.ports {
        let mut b = match Board::open(port, cfg.board_timeout) {
            Ok(b) => b,
            Err(e) => {
                println!("{port}: {e}");
                continue;
            }
        };

        // short test: nonce 50 is inside the range, measures startup overhead
        let (nonce, elapsed, ducoid) = match b.search(TEST_PREV, &hex(&sha1(format!("{TEST_PREV}50").as_bytes())), 10) {
            Ok(r) => r,
            Err(e) => {
                println!("{port}: {e}");
                continue;
            }
        };
        println!(
            "{port}: {ducoid} - test nonce {nonce} in {} us -> {:.2} H/s",
            elapsed,
            nonce as f64 / (elapsed as f64 / 1e6)
        );

        // full range runs so every digit length is exercised
        for (nonce, diff) in [(9_999u64, 100u32), (42_424u64, 655u32), (65_500u64, 655u32)] {
            let target = hex(&sha1(format!("{TEST_PREV}{nonce}").as_bytes()));
            match b.search(TEST_PREV, &target, diff) {
                Ok((found, us, _)) => println!(
                    "  diff {diff}: found nonce {found} in {} s -> {:.2} H/s {}",
                    us as f64 / 1e6,
                    found as f64 / (us as f64 / 1e6),
                    if found == nonce { "OK" } else { "MISMATCH" }
                ),
                Err(e) => println!("  diff {diff}: {e}"),
            }
        }
        b.close();
    }
}

/// Report the pool node the picker hands out and its round-trip time, without
/// touching a board or an account. Handy for working out which node is closest.
fn ping_node(cfg: &Config) {
    let node = match cfg.pool.clone() {
        Some(p) => (p.0, p.1, "specified node".to_string()),
        None => discover().unwrap_or((
            "152.53.241.160".to_string(),
            7913,
            "master2".to_string(),
        )),
    };
    match Pool::connect(&node.0, node.1, cfg.pool_timeout) {
        Ok(mut p) => {
            let greeting = p.motd();
            println!(
                "pool node {}:{} ({}): greeting in {} ms - \"{greeting}\"",
                node.0, node.1, node.2, p.latency
            );
        }
        Err(e) => println!("pool node {}:{} ({}): {e}", node.0, node.1, node.2),
    }
}

fn mine(cfg: &Config) {
    if cfg.user.is_empty() {
        eprintln!("a username is required (--user or --config)");
        std::process::exit(1);
    }

    if cfg.ports.len() > 2 {
        eprintln!(
            "warning: {} ports given but the account allows 2 concurrent miners; \
             only the first two are used",
            cfg.ports.len()
        );
    }
    let ports: Vec<String> = cfg.ports.iter().take(2).cloned().collect();

    // Pool node from --pool, otherwise the pool picker.
    let node: (String, u16, String) = match cfg.pool.clone() {
        Some(p) => (p.0, p.1, "mock node".to_string()),
        None => discover().unwrap_or_else(|e| {
            eprintln!("pool discovery failed ({e}), using the master node");
            ("152.53.241.160".to_string(), 7913, "master2".to_string())
        }),
    };
    println!(
        "pool node {}:{} ({}), user {}, boards: {}",
        node.0, node.1, node.2, cfg.user, ports.join(", ")
    );

    // The pool node does not reject a missing or wrong mining key - it just
    // hands out jobs. So ask the account service whether this user has a key,
    // and whether the one we are about to send matches, and say so plainly
    // instead of mining quietly with an invalid key. Skipped in --mock mode,
    // which never touches the real account.
    if cfg.mock {
        println!("account {}: mock pool, no key check", cfg.user);
    } else {
        match key_status(&cfg.user, &cfg.key) {
            Ok((accepted, has_key)) => {
                if !has_key {
                    println!("account {}: no mining key set, none needed", cfg.user);
                } else if accepted {
                    println!("account {}: mining key accepted", cfg.user);
                } else {
                    println!(
                        "WARNING: account {} has a mining key set, but this miner is not \
                         sending it. Run scripts/set-mining-key.sh, put it in \
                         Settings.cfg (mining_key = <key>), or start the miner with --key <key>. \
                         The pool still accepts shares, but they are logged as key-less miners, \
                         and the key exists to stop other people mining under your username.",
                        cfg.user
                    );
                }
            }
            Err(e) => println!("account {}: could not check the mining key ({e})", cfg.user),
        }
    }

    let totals = Arc::new(Mutex::new(Totals {
        per_board: vec![0.0; ports.len()],
        ..Default::default()
    }));
    let mut handles = Vec::new();

    let running = Arc::new(std::sync::atomic::AtomicBool::new(true));
    for (idx, port) in ports.iter().enumerate() {
        let port = port.clone();
        let cfg = cfg.clone();
        let totals = totals.clone();
        let node = node.clone();
        let running = running.clone();
        handles.push(thread::spawn(move || {
            let rate = mine_board(idx, &port, &cfg, &totals, node);
            let mut t = totals.lock().unwrap();
            t.per_board[idx] = rate;
            running.store(false, std::sync::atomic::Ordering::Relaxed);
        }));
    }

    // Reporting thread: prints the running totals every report_time seconds.
    let start = Instant::now();
    let report = cfg.report_time.max(1);
    while running.load(std::sync::atomic::Ordering::Relaxed) {
        thread::sleep(Duration::from_secs(report));
        let t = totals.lock().unwrap();
        let total: f64 = t.per_board.iter().sum();
        println!(
            "[{}s] accepted {} rejected {} blocks {} - total {:.2} H/s {}",
            start.elapsed().as_secs(),
            t.accepted,
            t.rejected,
            t.blocks,
            total,
            t.per_board
                .iter()
                .map(|h| format!("{h:.0}"))
                .collect::<Vec<_>>()
                .join(" / ")
        );
    }
    for h in handles {
        let _ = h.join();
    }
    let t = totals.lock().unwrap();
    println!("mining stopped: {} accepted, {} rejected", t.accepted, t.rejected);
}

fn mine_board(
    idx: usize,
    port: &str,
    cfg: &Config,
    totals: &Arc<Mutex<Totals>>,
    node: (String, u16, String),
) -> f64 {
    // The rig identifier the official miner sends with each share. If Settings.cfg
    // has one for this board we use it, otherwise a stable per-port name.
    let rigid = cfg
        .identifiers
        .get(idx)
        .filter(|s| !s.is_empty())
        .cloned()
        .unwrap_or_else(|| format!("rust-miner-{port}"));

    let mut b = match Board::open(port, cfg.board_timeout) {
        Ok(b) => b,
        Err(e) => {
            println!("board {port}: {e}");
            return 0.0;
        }
    };

    let target = hex(&sha1(format!("{TEST_PREV}50").as_bytes()));
    let (nonce, elapsed, _) = match b.search(TEST_PREV, &target, 10) {
        Ok(r) => r,
        Err(e) => {
            println!("board {port}: hash test failed: {e}");
            return 0.0;
        }
    };
    let test_rate = nonce as f64 / (elapsed as f64 / 1e6);
    let measured = tier_for(test_rate);
    let tier_name = cfg.tier.clone().unwrap_or_else(|| measured.to_string());
    let tier = tier_name.as_str();
    println!(
        "board {port}: test {:.2} H/s -> difficulty class {tier}{}",
        test_rate,
        if cfg.tier.is_some() { " (requested)" } else { "" }
    );

    let mut node = node;
    let mut p = match Pool::connect(&node.0, node.1, cfg.pool_timeout) {
        Ok(p) => p,
        Err(e) => {
            println!("board {port}: pool connect failed: {e}");
            return 0.0;
        }
    };
    // No MOTD request here on purpose: the greeting is not newline-terminated,
    // so waiting for it idles the socket long enough for the node to drop the
    // connection, which then costs the first job request. The node name comes
    // from the pool picker and is printed by mine(); --ping still asks for the
    // greeting because that is what it is for.
    let mut last_rate = 0.0;
    let mut pool_fails = 0u32;
    let mut round = 0u32;
    while cfg.rounds == 0 || round < cfg.rounds {
        round += 1;
        let (prev, target, diff) = match p.job(&cfg.user, tier, &cfg.key) {
            Ok(j) => {
                pool_fails = 0;
                j
            }
            Err(e) => {
                pool_fails += 1;
                println!("board {port}: {e} - reconnecting to the pool");
                // The node may be gone or saturated. After a few failures, ask
                // the pool picker for a different node rather than hammering
                // the one that just closed on us.
                if pool_fails >= 3 {
                    let (ip, port_no, name) =
                        discover().unwrap_or((node.0.clone(), node.1, node.2.clone()));
                    println!("board {port}: switching pool node to {ip}:{port_no} ({name})");
                    node = (ip, port_no, name);
                    pool_fails = 0;
                    match Pool::connect(&node.0, node.1, cfg.pool_timeout) {
                        Ok(np) => p = np,
                        Err(e2) => println!("board {port}: connect failed: {e2}"),
                    }
                } else if let Err(e2) = p.reconnect() {
                    println!("board {port}: reconnect failed: {e2}");
                }
                thread::sleep(Duration::from_secs(5));
                continue;
            }
        };

        // The firmware refuses difficulty above 655 on AVR, so a share can
        // never take longer than ~65501 hashes.
        let allowance = Duration::from_secs(((diff as u64 * 100) / 500 + 60).max(60));
        if let Err(e) = b.send_job(&prev, &target, diff) {
            println!("board {port}: {e}");
            continue;
        }
        let start = Instant::now();
        let outcome = loop {
            let spent = start.elapsed();
            if spent >= allowance {
                break Err(format!("board {port} exceeded the {}s allowance", allowance.as_secs()));
            }
            match b.read_reply(allowance - spent) {
                Ok((nonce, elapsed, ducoid)) => {
                    // Replies arrive in the order the jobs were sent. If the
                    // board was still searching an earlier job when we sent
                    // this one, the first line we see belongs to that job.
                    if sha1_hex(&format!("{prev}{nonce}")) == target {
                        break Ok((nonce, elapsed, ducoid));
                    }
                    println!("board {port}: discarding stale reply (nonce {nonce})");
                    continue;
                }
                Err(e) => break Err(e),
            }
        };

        match outcome {
            Ok((nonce, elapsed, ducoid)) => {
                let seconds = elapsed as f64 / 1e6;
                let rate = if seconds > 0.0 { nonce as f64 / seconds } else { 0.0 };
                last_rate = rate;
                totals.lock().unwrap().per_board[idx] = rate;

                let feedback = match p.submit(nonce, rate, &rigid, &ducoid) {
                    Ok(f) => f,
                    Err(e) => {
                        println!("board {port}: submit failed: {e}");
                        continue;
                    }
                };

                let mut t = totals.lock().unwrap();
                match feedback.first().map(|s| s.as_str()) {
                    Some("GOOD") | Some("BLOCK") => {
                        t.accepted += 1;
                        if feedback.first().map(|s| s.as_str()) == Some("BLOCK") {
                            t.blocks += 1;
                        }
                        println!(
                            "board {port}: accepted nonce {nonce} ({:.2} H/s, diff {diff})",
                            rate
                        );
                    }
                    _ => {
                        t.rejected += 1;
                        let joined = feedback.join(" ");
                        println!("board {port}: rejected: {joined}");
                    }
                }
            }
            Err(e) => {
                println!("board {port}: {e}");
                b.discard_pending();
                totals.lock().unwrap().per_board[idx] = 0.0;
                // Whatever went wrong, the surest recovery is to reset the
                // board: opening the port toggles DTR, which restarts the
                // sketch with an empty serial buffer. Without this a board that
                // mis-frames one job can sit searching a stale target for
                // minutes and look dead.
                println!("board {port}: reopening {port} to reset it");
                b.close();
                for attempt in 1..=3 {
                    match Board::open(port, cfg.board_timeout) {
                        Ok(nb) => {
                            b = nb;
                            break;
                        }
                        Err(e2) => {
                            println!("board {port}: reopen attempt {attempt} failed: {e2}");
                            thread::sleep(Duration::from_secs(5));
                        }
                    }
                }
                thread::sleep(Duration::from_secs(3));
            }
        }
    }
    b.close();
    last_rate
}

#[cfg(test)]
mod key_tests {
    use super::*;

    #[test]
    fn base64_roundtrip() {
        // vectors taken from python's base64: "hello" -> aGVsbG8=, "ab" -> YWI=
        assert_eq!(base64_decode("aGVsbG8=").unwrap(), "hello");
        assert_eq!(base64_decode("YWI=").unwrap(), "ab");
        assert_eq!(base64_decode("YQ==").unwrap(), "a");
        assert_eq!(base64_decode("ZHVpbm9jb2lu").unwrap(), "duinocoin");
        assert_eq!(base64_decode("czNjcmV0LWtleSE=").unwrap(), "s3cret-key!");
    }

    #[test]
    fn key_is_normalised() {
        // the official miner stores the key base64-encoded
        assert_eq!(normalise_key("aGVsbG8="), "hello");
        // a key typed straight into the file is used as-is
        assert_eq!(normalise_key("s3cret-key!"), "s3cret-key!");
        assert_eq!(normalise_key("None"), "None");
        assert_eq!(normalise_key(""), "None");
        assert_eq!(normalise_key("  "), "None");
    }
}
