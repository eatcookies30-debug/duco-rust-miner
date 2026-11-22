use std::collections::HashMap;
use std::env;
use std::fs;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

mod board;
mod pool;
mod rig;
mod sha1;
mod style;

use board::{Board, Link, RigBoard};
use pool::{discover, key_status, Pool};
use sha1::{hex, sha1};
use style::Out;

#[derive(Clone)]
struct Config {
    user: String,
    key: String,
    // Serial ports of boards plugged straight into the PC.
    ports: Vec<String>,
    // Serial ports of rig masters (firmware/i2c_rig). Every board hanging off a
    // rig master gets its own pool connection.
    rigs: Vec<String>,
    pool: Option<(String, u16)>,
    report_time: u64,
    board_timeout: Duration,
    pool_timeout: Duration,
    rounds: u32,
    // Rig identifiers from Settings.cfg `identifier = ...`, one per board.
    // Sent with every share, the same way the official AVR miner does.
    identifiers: Vec<String>,
    bench: bool,
    ping: bool,
    // Force the difficulty class sent to the pool (AVR/ARM/MEGA/DUE) instead of
    // deriving it from the measured hashrate.
    tier: Option<String>,
    // Switch the colours off even on a terminal.
    no_color: bool,
}

#[derive(Default)]
struct Totals {
    accepted: u32,
    rejected: u32,
    blocks: u32,
    rates: HashMap<String, f64>,
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
    let out = Out::new(cfg.no_color);

    if cfg.ping {
        ping_node(&cfg, &out);
        return;
    }

    if cfg.ports.is_empty() && cfg.rigs.is_empty() {
        eprintln!(
            "no boards given; use --ports /dev/ttyACM0 and --rig /dev/ttyACM1 \
             (or avrport/rigport in Settings.cfg)"
        );
        std::process::exit(1);
    }

    let lock = format!(
        "/tmp/duco-miner-{}.lock",
        cfg.ports
            .iter()
            .chain(cfg.rigs.iter())
            .cloned()
            .collect::<Vec<_>>()
            .join("_")
            .replace('/', "-")
    );
    if let Err(e) = take_lock(&lock) {
        eprintln!("{e}");
        std::process::exit(1);
    }

    if cfg.bench {
        bench(&cfg, &out);
        release_lock(&lock);
        return;
    }

    mine(&cfg, &out);
    release_lock(&lock);
}

fn usage() {
    println!(
        "usage: duco-miner [--config Settings.cfg] [--user NAME] [--key KEY] \\\n\
        \x20     [--ports /dev/ttyACM0] [--rig /dev/ttyACM1] [--pool ip:port] \\\n\
        \x20     [--report-time SECS] [--board-timeout SECS] [--rounds N] [--no-color] [--version]\n\
        \n\
        helpers:\n\
        \x20 duco-miner --bench     test the boards offline, no account needed\n\
        \x20 duco-miner --ping      print the pool node the picker gives and its latency\n"
    );
}

fn parse_args() -> Result<Config, String> {
    let args: Vec<String> = env::args().collect();
    let mut cfg = Config {
        user: String::new(),
        key: "None".to_string(),
        ports: Vec::new(),
        rigs: Vec::new(),
        pool: None,
        report_time: 60,
        board_timeout: Duration::from_secs(120),
        pool_timeout: Duration::from_secs(15),
        rounds: 0,
        identifiers: Vec::new(),
        bench: false,
        ping: false,
        tier: None,
        no_color: false,
    };

    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        let value = |i: usize| -> Result<String, String> {
            args.get(i + 1)
                .cloned()
                .ok_or_else(|| format!("{a} needs a value"))
        };
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
                cfg.ports = split_list(&value(i)?);
                i += 1;
            }
            "--rig" | "--rigs" => {
                cfg.rigs = split_list(&value(i)?);
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
            "--no-color" => cfg.no_color = true,
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

fn split_list(value: &str) -> Vec<String> {
    value
        .split(',')
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect()
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
            "avrport" | "port" => cfg.ports = split_list(v),
            "rigport" | "rig_port" | "rig" => cfg.rigs = split_list(v),
            "identifier" => cfg.identifiers = split_list(v),
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

/// Difficulty class the pool uses for a board. The pool checks a share against
/// the hashrate it was reported with: a share is only accepted when the
/// difficulty sits at about a tenth of the hashrate, so the class has to match
/// how fast the board really is. These are the points where each class stops
/// being accepted by the pool, given the difficulties the classes are handed -
/// AVR 10, MEGA 24, ARM 128, DUE 512.
fn tier_for(rate: f64) -> &'static str {
    if rate >= 2400.0 {
        "DUE"
    } else if rate >= 600.0 {
        "ARM"
    } else if rate >= 113.0 {
        "MEGA"
    } else {
        "AVR"
    }
}

const TEST_PREV: &str = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";

fn sha1_hex(s: &str) -> String {
    hex(&sha1(s.as_bytes()))
}

/* Replies arrive in the order the jobs were sent, so the first line a board
   hands back can belong to a job it was still searching when this one was
   handed over. A reply is only believed when the nonce in it really hashes to
   the target that was asked for; otherwise the job is sent again. */
fn ask_for(
    label: &str,
    out: &Out,
    link: &dyn Link,
    prev: &str,
    target: &str,
    difficulty: u32,
    allowance: Duration,
) -> Option<(u64, u64, String)> {
    let start = Instant::now();
    for attempt in 1..=3 {
        let spent = start.elapsed();
        if spent >= allowance {
            out.say(
                label,
                &format!("{label} did not answer the job within {}s", allowance.as_secs()),
                "error",
            );
            return None;
        }
        if let Err(e) = link.send(prev, target, difficulty) {
            out.say(label, &format!("could not send the job: {e}", ), "error");
            return None;
        }
        match link.reply(allowance - spent) {
            Ok((nonce, elapsed, ducoid)) => {
                if sha1_hex(&format!("{prev}{nonce}", )) == target {
                    return Some((nonce, elapsed, ducoid));
                }
                out.say(
                    label,
                    &format!(
                        "reply {} is not the nonce for this job, sending the job again (attempt {attempt})",
                        nonce
                    ),
                    "warning",
                );
            }
            Err(e) => {
                out.say(label, &e, "error");
                return None;
            }
        }
    }
    out.say(label, "no reply matched the job after three tries", "error");
    None
}

/* Nonce 50 sits inside the range difficulty 10 allows, so a board that answers
   at all answers quickly and the number measures the trip as well as the hash.
   It is the same test the pool's own difficulty tiers are built from. */
fn test_rate(label: &str, out: &Out, link: &dyn Link, timeout: Duration) -> Option<f64> {
    let target = hex(&sha1(format!("{TEST_PREV}50").as_bytes()));
    match ask_for(label, out, link, TEST_PREV, &target, 10, timeout) {
        Some((nonce, elapsed, _)) => {
            if elapsed == 0 {
                out.say(label, "the board reported no elapsed time for the test job", "error");
                return None;
            }
            Some(nonce as f64 / (elapsed as f64 / 1e6))
        }
        None => None,
    }
}

fn bench(cfg: &Config, out: &Out) {
    out.say("sys", "offline board test (no network, no account)", "info");

    for port in &cfg.ports {
        match Board::open(port) {
            Ok(b) => {
                run_board_test(&format!("avr {port}", ), cfg, out, &b);
            }
            Err(e) => out.say("avr", &format!("{port}: {e}", ), "error"),
        }
    }

    for port in &cfg.rigs {
        let r = match rig::Rig::open(port) {
            Ok(r) => Arc::new(r),
            Err(e) => {
                out.say("rig", &format!("{port}: {e}", ), "error");
                continue;
            }
        };
        let addresses = r.wait_for_slaves(Duration::from_secs(15));
        if addresses.is_empty() {
            out.say(
                "rig",
                &format!(
                    "{port}: no board answered on the bus. Check SDA to SDA, SCL to SCL \
                     and GND to GND."
                ),
                "error",
            );
            continue;
        }
        out.say(
            "rig",
            &format!(
                "{port}: boards on the bus: {}",
                addresses.iter().map(|a| a.to_string()).collect::<Vec<_>>().join(", ")
            ),
            "info",
        );
        for &address in &addresses {
            let link = RigBoard::new(r.clone(), address);
            run_board_test(&format!("rig{address}", ), cfg, out, &link);
        }
    }
}

/* How fast is this board, and does it hash correctly at every digit length the
   protocol can ask for? */
fn run_board_test(label: &str, cfg: &Config, out: &Out, link: &dyn Link) {
    let rate = match test_rate(label, out, link, cfg.board_timeout) {
        Some(rate) => rate,
        None => return,
    };
    out.say(
        label,
        &format!(
            "test nonce 50 in {:.0} us -> {:.2} H/s",
            50 as f64 / rate.max(f64::EPSILON) * 1e6,
            rate
        ),
        "info",
    );

    for (nonce, diff) in [(9_999u64, 100u32), (42_424u64, 655u32), (65_500u64, 655u32)] {
        let target = hex(&sha1(format!("{TEST_PREV}{nonce}").as_bytes()));
        match ask_for(label, out, link, TEST_PREV, &target, diff, cfg.board_timeout) {
            Some((found, elapsed, _)) => out.say(
                label,
                &format!(
                    "diff {diff}: found nonce {found} in {:.1}s -> {} {}",
                    elapsed as f64 / 1e6,
                    if elapsed > 0 {
                        format!("{:.0} H/s", found as f64 / (elapsed as f64 / 1e6))
                    } else {
                        "no time measured".to_string()
                    },
                    if found == nonce { "OK" } else { "MISMATCH" }
                ),
                if found == nonce { "success" } else { "error" },
            ),
            None => continue,
        }
    }
}

/// Report the pool node the picker hands out and its round-trip time, without
/// touching a board or an account. Handy for working out which node is closest.
fn ping_node(cfg: &Config, out: &Out) {
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
            out.say(
                "net",
                &format!(
                    "pool node {}:{} ({}): greeting in {} ms - \"{greeting}\"",
                    node.0, node.1, node.2, p.latency
                ),
                "success",
            );
        }
        Err(e) => out.say(
            "net",
            &format!("pool node {}:{} ({}): {e}", node.0, node.1, node.2),
            "error",
        ),
    }
}

fn mine(cfg: &Config, out: &Out) {
    if cfg.user.is_empty() {
        eprintln!("a username is required (--user or --config)");
        std::process::exit(1);
    }

    let node: (String, u16, String) = match cfg.pool.clone() {
        Some(p) => (p.0, p.1, "specified node".to_string()),
        None => discover().unwrap_or_else(|e| {
            eprintln!("pool discovery failed ({e}), using the master node");
            ("152.53.241.160".to_string(), 7913, "master2".to_string())
        }),
    };

    out.say(
        "sys",
        &format!(
            "pool node {}:{} ({}), user {}, {} board(s) on serial, {} rig master(s)",
            node.0, node.1, node.2, cfg.user, cfg.ports.len(), cfg.rigs.len()
        ),
        "info",
    );

    // The pool node does not reject a missing or wrong mining key - it just
    // hands out jobs. So ask the account service whether this user has a key,
    // and whether the one we are about to send matches, and say so plainly
    // instead of mining quietly with an invalid key.
    match key_status(&cfg.user, &cfg.key) {
        Ok((accepted, has_key)) => {
            if !has_key {
                out.say(
                    "sys",
                    &format!("account {}: no mining key set, none needed", cfg.user),
                    "info",
                );
            } else if accepted {
                out.say(
                    "sys",
                    &format!("account {}: mining key accepted", cfg.user),
                    "success",
                );
            } else {
                out.say(
                    "sys",
                    &format!(
                        "account {} has a mining key set, but this miner is not sending it. \
                         Run scripts/set-mining-key.sh, put it in Settings.cfg \
                         (mining_key = <key>), or start the miner with --key <key>. \
                         The pool still accepts shares, but they are logged as key-less \
                         miners, and the key exists to stop other people mining under \
                         your username.",
                        cfg.user
                    ),
                    "warning",
                );
            }
        }
        Err(e) => out.say(
            "sys",
            &format!("account {}: could not check the mining key ({e})", cfg.user),
            "warning",
        ),
    }

    let totals = Arc::new(Mutex::new(Totals::default()));
    /* One thread per board. The number of boards still mining is what keeps the
       report loop going, so one board that stops does not stop the rest. */
    let active = Arc::new(AtomicUsize::new(0));
    let mut handles = Vec::new();
    let mut board_index = 0usize;

    for port in &cfg.ports {
        let port = port.clone();
        let cfg = cfg.clone();
        let totals = totals.clone();
        let node = node.clone();
        let active = active.clone();
        let out = out.clone();
        let label = format!("avr{}", board_index + 1);
        let rigid = identifier_for(&cfg, board_index, &port);
        board_index += 1;

        active.fetch_add(1, Ordering::Relaxed);
        handles.push(thread::spawn(move || {
            let link = match Board::open(&port) {
                Ok(b) => b,
                Err(e) => {
                    out.say(&label, &format!("{port}: {e}", ), "error");
                    active.fetch_sub(1, Ordering::Relaxed);
                    return;
                }
            };
            let rate = mine_link(&label, &rigid, &cfg, &totals, &out, node, &link);
            totals.lock().unwrap().rates.insert(label.clone(), rate);
            active.fetch_sub(1, Ordering::Relaxed);
        }));
    }

    /* Every board on a rig gets its own thread, its own pool connection and its
       own DUCOID, so the pool counts each one as a separate miner. */
    let mut rig_links: Vec<(Arc<rig::Rig>, Vec<u8>, String)> = Vec::new();
    for port in &cfg.rigs {
        let r = match rig::Rig::open(port) {
            Ok(r) => Arc::new(r),
            Err(e) => {
                out.say("rig", &format!("{port}: {e}", ), "error");
                continue;
            }
        };
        let addresses = r.wait_for_slaves(Duration::from_secs(20));
        if addresses.is_empty() {
            out.say(
                "rig",
                &format!(
                    "{port}: the rig master reported no boards. Check SDA to SDA, SCL to SCL \
                     and GND to GND, or open the port with a serial monitor and read the \
                     master's own report."
                ),
                "error",
            );
            continue;
        }
        out.say(
            "rig",
            &format!(
                "{port}: driving boards {}",
                addresses.iter().map(|a| a.to_string()).collect::<Vec<_>>().join(", ")
            ),
            "success",
        );
        for &address in &addresses {
            spawn_rig_board(
                &mut handles, &active, r.clone(), address, board_index,
                cfg, &totals, out, node.clone(), port,
            );
            board_index += 1;
        }
        rig_links.push((r, addresses, port.clone()));
    }

    if active.load(Ordering::Relaxed) == 0 {
        eprintln!("no board could be reached");
        std::process::exit(1);
    }

    let start = Instant::now();
    let report = cfg.report_time.max(1);
    while active.load(Ordering::Relaxed) > 0 {
        thread::sleep(Duration::from_secs(report));

        /* A board plugged into the rig later is picked up by the master's
           periodic scan and gets a thread and a pool connection of its own. */
        for (link_rig, started, link_port) in &mut rig_links {
            for address in link_rig.slaves() {
                if started.contains(&address) {
                    continue;
                }
                out.say("rig", &format!("board {address} joined the rig on {link_port}", ), "success");
                started.push(address);
                spawn_rig_board(
                    &mut handles, &active, link_rig.clone(), address, board_index,
                    cfg, &totals, out, node.clone(), link_port,
                );
                board_index += 1;
            }
        }

        let t = totals.lock().unwrap();
        let total: f64 = t.rates.values().sum();
        let mut per_board: Vec<String> = t.rates
            .iter()
            .filter_map(|(name, rate)| {
                if *rate > 0.0 {
                    Some(format!("{name} {}", style::prefix(*rate)))
                } else {
                    None
                }
            })
            .collect();
        per_board.sort();
        out.say(
            "sys",
            &format!(
                "{}s: accepted {} rejected {} blocks {} - total {} {}",
                start.elapsed().as_secs(),
                t.accepted,
                t.rejected,
                t.blocks,
                style::prefix(total),
                per_board.join(" / ")
            ),
            "info",
        );
    }

    for h in handles {
        let _ = h.join();
    }
    let t = totals.lock().unwrap();
    out.say(
        "sys",
        &format!("mining stopped: {} accepted, {} rejected", t.accepted, t.rejected),
        "success",
    );
}

fn identifier_for(cfg: &Config, index: usize, port: &str) -> String {
    cfg.identifiers
        .get(index)
        .filter(|s| !s.is_empty())
        .cloned()
        .unwrap_or_else(|| format!("rust-miner-{port}", ))
}

/* One board hanging off a rig master. The thread and the pool connection are
   the board's own; only the bus traffic goes through the master. */
fn spawn_rig_board(
    handles: &mut Vec<thread::JoinHandle<()>>,
    active: &Arc<AtomicUsize>,
    rig: Arc<rig::Rig>,
    address: u8,
    board_index: usize,
    cfg: &Config,
    totals: &Arc<Mutex<Totals>>,
    out: &Out,
    node: (String, u16, String),
    port: &str,
) {
    let cfg = cfg.clone();
    let totals = totals.clone();
    let active = active.clone();
    let out = out.clone();
    let label = format!("rig{address}", );
    let rigid = identifier_for(&cfg, board_index, &format!("{port}-{address}", ));

    active.fetch_add(1, Ordering::Relaxed);
    handles.push(thread::spawn(move || {
        let link = RigBoard::new(rig, address);
        let rate = mine_link(&label, &rigid, &cfg, &totals, &out, node, &link);
        totals.lock().unwrap().rates.insert(label.clone(), rate);
        active.fetch_sub(1, Ordering::Relaxed);
    }));
}

/// The classes in order of difficulty, for stepping up or down when the pool
/// says the job it handed out did not fit the board.
const TIERS: [&str; 4] = ["AVR", "MEGA", "ARM", "DUE"];

fn step_tier(tier: &str, up: bool) -> String {
    let i = TIERS
        .iter()
        .position(|t| *t == tier)
        .unwrap_or(0);
    let next = if up {
        (i + 1).min(TIERS.len() - 1)
    } else {
        i.saturating_sub(1)
    };
    TIERS[next].to_string()
}

/* One board, however it is reached: straight over a serial port, or through a
   rig master. */
fn mine_link(
    label: &str,
    rigid: &str,
    cfg: &Config,
    totals: &Arc<Mutex<Totals>>,
    out: &Out,
    node: (String, u16, String),
    link: &dyn Link,
) -> f64 {
    let test_rate = match test_rate(label, out, link, cfg.board_timeout) {
        Some(rate) => rate,
        None => return 0.0,
    };
    let measured = tier_for(test_rate);
    let mut tier = cfg.tier.clone().unwrap_or_else(|| measured.to_string());
    out.say(
        label,
        &format!(
            "test {:.2} H/s -> difficulty class {tier}{}",
            test_rate,
            if cfg.tier.is_some() { " (requested)" } else { "" }
        ),
        "info",
    );

    let mut node = node;
    let mut p = match Pool::connect(&node.0, node.1, cfg.pool_timeout) {
        Ok(p) => p,
        Err(e) => {
            out.say(label, &format!("pool connect failed: {e}", ), "error");
            return 0.0;
        }
    };
    // No MOTD request here on purpose: the greeting is not newline-terminated,
    // so waiting for it idles the socket long enough for the node to drop the
    // connection, which then costs the first job request. --ping still asks for
    // the greeting because that is what it is for.
    let mut last_rate = 0.0;
    let mut pool_fails = 0u32;
    let mut round = 0u32;
    while cfg.rounds == 0 || round < cfg.rounds {
        round += 1;
        let (prev, target, diff) = match p.job(&cfg.user, &tier, &cfg.key) {
            Ok(j) => {
                pool_fails = 0;
                j
            }
            Err(e) => {
                pool_fails += 1;
                out.say(label, &format!("{e} - reconnecting to the pool", ), "warning");
                // The node may be gone or saturated. After a few failures, ask
                // the pool picker for a different node rather than hammering the
                // one that just closed on us.
                if pool_fails >= 3 {
                    let (ip, port_no, name) =
                        discover().unwrap_or((node.0.clone(), node.1, node.2.clone()));
                    out.say(label, &format!("switching pool node to {ip}:{port_no} ({name})", ), "info");
                    node = (ip, port_no, name);
                    pool_fails = 0;
                    match Pool::connect(&node.0, node.1, cfg.pool_timeout) {
                        Ok(np) => p = np,
                        Err(e2) => out.say(label, &format!("connect failed: {e2}", ), "error"),
                    }
                } else if let Err(e2) = p.reconnect() {
                    out.say(label, &format!("reconnect failed: {e2}", ), "warning");
                }
                thread::sleep(Duration::from_secs(5));
                continue;
            }
        };

        // The firmware refuses difficulty above 655 on AVR, so a share can
        // never take longer than ~65501 hashes.
        let allowance = Duration::from_secs(((diff as u64 * 100) / 500 + 60).max(60));
        let outcome = ask_for(label, &out, link, &prev, &target, diff, allowance);

        match outcome {
            Some((nonce, elapsed, ducoid)) => {
                let seconds = elapsed as f64 / 1e6;
                let rate = if seconds > 0.0 { nonce as f64 / seconds } else { 0.0 };
                last_rate = rate;
                totals.lock().unwrap().rates.insert(label.to_string(), rate);

                let feedback = match p.submit(nonce, rate, rigid, &ducoid) {
                    Ok(f) => f,
                    Err(e) => {
                        out.say(label, &format!("submit failed: {e}", ), "error");
                        continue;
                    }
                };

                let kind = match feedback.first().map(|s| s.as_str()) {
                    Some("GOOD") => "accept",
                    Some("BLOCK") => "block",
                    _ => "reject",
                };
                let (accepted, rejected, blocks, total) = {
                    let mut t = totals.lock().unwrap();
                    match kind {
                        "accept" | "block" => {
                            t.accepted += 1;
                            if kind == "block" {
                                t.blocks += 1;
                            }
                        }
                        _ => t.rejected += 1,
                    }
                    (t.accepted, t.rejected, t.blocks, t.rates.values().sum())
                };
                let _ = blocks;

                out.share(
                    label,
                    accepted,
                    rejected,
                    rate,
                    total,
                    seconds,
                    diff,
                    p.latency as u128,
                    kind,
                    &if kind == "reject" {
                        feedback.join(" ")
                    } else {
                        String::new()
                    },
                );

                /* The pool judges a share against the hashrate it was reported
                   with, and says so when the difficulty it handed out does not
                   suit the board. Moving the class one step in the direction
                   that fits gets the next job accepted. */
                if kind == "reject" && cfg.tier.is_none() {
                    let note = feedback.join(" ");
                    if note.contains("Too high starting difficulty") {
                        let next = step_tier(&tier, false);
                        if next != tier {
                            tier = next;
                            out.say(
                                label,
                                &format!("difficulty {diff} is too much for this board, next job at {tier}", ),
                                "warning",
                            );
                        }
                    } else if note.contains("Too low starting difficulty") {
                        let next = step_tier(&tier, true);
                        if next != tier {
                            tier = next;
                            out.say(
                                label,
                                &format!("difficulty {diff} is too little for this board, next job at {tier}", ),
                                "warning",
                            );
                        }
                    }
                }
            }
            None => {
                totals.lock().unwrap().rates.insert(label.to_string(), 0.0);
                // Whatever went wrong, the surest recovery is to reset the
                // board: opening the port toggles DTR, which restarts the sketch
                // with an empty serial buffer. On a rig board the master just
                // re-sends the job. Without this a board that mis-frames one job
                // can sit searching a stale target for minutes and look dead.
                for attempt in 1..=3 {
                    match link.reopen() {
                        Ok(()) => break,
                        Err(e2) => {
                            out.say(label, &format!("recover attempt {attempt} failed: {e2}", ), "warning");
                            thread::sleep(Duration::from_secs(5));
                        }
                    }
                }
                thread::sleep(Duration::from_secs(3));
            }
        }
    }
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

    #[test]
    fn tiers_match_what_the_pool_accepts() {
        assert_eq!(tier_for(3000.0), "DUE");
        assert_eq!(tier_for(1757.0), "ARM");
        assert_eq!(tier_for(600.0), "ARM");
        assert_eq!(tier_for(400.0), "MEGA");
        assert_eq!(tier_for(100.0), "AVR");
        assert_eq!(tier_for(50.0), "AVR");
    }

    #[test]
    fn the_tier_steps_but_never_leaves_the_list() {
        assert_eq!(step_tier("ARM", false), "MEGA");
        assert_eq!(step_tier("ARM", true), "DUE");
        assert_eq!(step_tier("AVR", false), "AVR");
        assert_eq!(step_tier("DUE", true), "DUE");
    }
}
