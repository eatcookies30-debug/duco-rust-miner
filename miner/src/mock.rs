//! A stand-in for the Duino-Coin pool node so the boards and the Rust miner can
//! be exercised end to end without an account. It speaks the same line
//! protocol as the real node: version greeting, MOTD, JOB, share feedback.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::thread;
use std::time::{Duration, Instant};

use crate::sha1::{hex, sha1};

#[derive(Clone)]
pub struct MockConfig {
    pub bind: String,
    pub port: u16,
    /// How many accepted shares to process before exiting (0 = run forever).
    pub rounds: u32,
    /// Difficulty the node assigns per tier.
    pub tier_difficulty: (u32, u32, u32, u32),
}

pub fn run(cfg: MockConfig) -> std::io::Result<()> {
    let listener = TcpListener::bind((cfg.bind.as_str(), cfg.port))?;
    let rounds_txt = if cfg.rounds == 0 {
        "unlimited".to_string()
    } else {
        cfg.rounds.to_string()
    };
    println!(
        "mock pool node listening on {}:{} (rounds: {})",
        cfg.bind, cfg.port, rounds_txt
    );

    let accepted = std::sync::Arc::new(std::sync::atomic::AtomicU32::new(0));

    for stream in listener.incoming() {
        let stream = stream?;
        let cfg = cfg.clone();
        let accepted = accepted.clone();
        thread::spawn(move || {
            handle(stream, &cfg, accepted);
        });
    }
    Ok(())
}

fn tier_diff(tier: &str, cfg: &MockConfig) -> u32 {
    match tier {
        "DUE" => cfg.tier_difficulty.3,
        "ARM" => cfg.tier_difficulty.2,
        "MEGA" => cfg.tier_difficulty.1,
        _ => cfg.tier_difficulty.0,
    }
}

fn handle(mut stream: TcpStream, cfg: &MockConfig, accepted: std::sync::Arc<std::sync::atomic::AtomicU32>) {
    let _ = stream.set_read_timeout(Some(Duration::from_secs(600)));
    let _ = stream.write_all(b"3.0\n");

    // Every miner message is written as one TCP write, so one read = one message.
    let mut buf = vec![0u8; 2048];
    let mut prev_hash = String::new();
    let mut target = String::new();
    let mut difficulty = 0u32;
    let mut nonce = 0u64;
    let mut shares = 0u32;
    let mut rejected = 0u32;
    let start = Instant::now();

    loop {
        let n = match stream.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => n,
        };
        let msg = String::from_utf8_lossy(&buf[..n]).trim_end_matches('\n').to_string();
        if msg.is_empty() {
            continue;
        }

        if msg.starts_with("MOTD") {
            let _ = stream.write_all(b"Mock node for offline testing\n");
            continue;
        }

        if msg.starts_with("PING") {
            let _ = stream.write_all(b"2\n");
            continue;
        }

        if msg.starts_with("JOB") {
            let parts: Vec<&str> = msg.split(',').collect();
            let user = parts.get(1).unwrap_or(&"anonymous").to_string();
            let tier = parts.get(2).unwrap_or(&"AVR").to_string();
            difficulty = tier_diff(&tier, cfg);

            // Random 40 hex char "previous block hash".
            let mut seed = start.elapsed().as_nanos() as u64
                ^ (user.len() as u64) << 17
                ^ (difficulty as u64) << 33;
            static HEX: [u8; 16] = *b"0123456789abcdef";
            let mut chars = String::new();
            for i in 0..40 {
                seed ^= seed >> 33;
                seed ^= seed << 7;
                seed ^= seed >> 11;
                chars.push(HEX[((seed >> (i % 32)) & 15) as usize] as char);
            }
            prev_hash = chars;

            nonce = 1 + (seed % (difficulty as u64 * 100));
            target = hex(&sha1(format!("{prev_hash}{nonce}").as_bytes()));

            let _ = stream.write_all(format!("{prev_hash},{target},{difficulty}\n").as_bytes());
            continue;
        }

        // Anything else is a share submission.
        let parts: Vec<&str> = msg.split(',').collect();
        if parts.len() < 2 {
            let _ = stream.write_all(b"BAD,malformed share\n");
            continue;
        }
        let claimed: u64 = parts[0].parse().unwrap_or(0);
        let ok = claimed == nonce
            && hex(&sha1(format!("{prev_hash}{claimed}").as_bytes())) == target
            && claimed <= difficulty as u64 * 100;

        if ok {
            shares += 1;
            let total = accepted.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
            let secs = start.elapsed().as_secs_f64();
            println!(
                "share #{total} accepted: nonce {claimed} difficulty {difficulty} \
                 claimed hashrate {} H/s (reward 0.01 DUCO simulated) - {shares}/{total} from this connection, uptime {:.1}s",
                parts[1], secs
            );
            let _ = stream.write_all(b"GOOD,0.01\n");
            if cfg.rounds > 0 && total >= cfg.rounds {
                println!("mock node: round target reached, closing");
                std::process::exit(0);
            }
        } else {
            rejected += 1;
            println!("share rejected: claimed nonce {claimed}, expected {nonce}");
            let _ = stream.write_all(b"BAD,wrong nonce\n");
        }
    }

    println!("connection closed: {shares} accepted, {rejected} rejected");
}


