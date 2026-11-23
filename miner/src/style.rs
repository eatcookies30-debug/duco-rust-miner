//! Terminal output in the shape the official AVR miner uses: a dim
//! timestamp, a |tag| for who is speaking, and the message coloured by how it
//! turned out.
//!
//!     12:34:56 |rig3| ⛏ Accepted 3/3 (100%) ∙ 37.4s ∙ 1.76 kH/s (10.54 kH/s total) ⚙ diff 655 ∙ ping 120ms
//!
//! Colours are left out when the output is not a terminal (a pipe, a redirect,
//! a desktop shortcut that captures the output) or when NO_COLOR is set, so
//! the same lines stay readable when they are saved to a file.

use std::env;
use std::io::IsTerminal;
use std::process::Command;
use std::time::SystemTime;

const RESET: &str = "\x1b[0m";
const BRIGHT: &str = "\x1b[1m";
const DIM: &str = "\x1b[2m";
const GREEN: &str = "\x1b[32m";
const YELLOW: &str = "\x1b[33m";
const BLUE: &str = "\x1b[34m";
const CYAN: &str = "\x1b[36m";
const RED: &str = "\x1b[31m";
const MAGENTA: &str = "\x1b[35m";
const WHITE_ON_MAGENTA: &str = "\x1b[37m\x1b[45m";
const WHITE_ON_BLUE: &str = "\x1b[37m\x1b[44m";
const WHITE_ON_GREEN: &str = "\x1b[37m\x1b[42m";

#[derive(Clone)]
pub struct Out {
    colour: bool,
    /* Seconds between the clock the machine reports and UTC, worked out once at
       start-up so a timestamp is not a process spawn per line. */
    offset: i64,
}

impl Out {
    pub fn new(force_off: bool) -> Out {
        Out {
            colour: !force_off
                && std::io::stdout().is_terminal()
                && env::var("NO_COLOR").is_err(),
            offset: local_offset(),
        }
    }

    fn paint(&self, code: &str, text: &str) -> String {
        if self.colour {
            format!("{code}{text}{RESET}")
        } else {
            text.to_string()
        }
    }

    fn stamp(&self) -> String {
        let epoch = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let mut local = (epoch + self.offset) % 86400;
        if local < 0 {
            local += 86400;
        }
        self.paint(
            DIM,
            &format!("{:02}:{:02}:{:02}", local / 3600, (local / 60) % 60, local % 60),
        )
    }

    fn tag(&self, sender: &str) -> String {
        if !self.colour {
            return format!("[{sender}]");
        }
        let background = if sender.starts_with("net") {
            WHITE_ON_BLUE
        } else if sender.starts_with("avr") || sender.starts_with("rig") {
            WHITE_ON_MAGENTA
        } else {
            WHITE_ON_GREEN
        };
        format!("{background}{BRIGHT} {sender} {RESET}")
    }

    pub fn say(&self, sender: &str, msg: &str, state: &str) {
        let foreground = match state {
            "success" => GREEN,
            "info" => BLUE,
            "error" => RED,
            _ => YELLOW,
        };
        println!(
            "{} {} {}",
            self.stamp(),
            self.tag(sender),
            self.paint(foreground, msg.trim())
        );
    }

    /* One share the pool answered for one board. `outcome` is "accept",
       "block" or "reject", and `reason` is what the pool said when it refused
       the share. */
    pub fn share(
        &self,
        sender: &str,
        accepted: u32,
        rejected: u32,
        board_rate: f64,
        total_rate: f64,
        seconds: f64,
        difficulty: u32,
        ping_ms: u128,
        outcome: &str,
        reason: &str,
    ) {
        let foreground = match outcome {
            "block" => YELLOW,
            "reject" => RED,
            _ => GREEN,
        };
        let word = match outcome {
            "block" => "⛏ Block found",
            "reject" => "⛏ Rejected",
            _ => "⛏ Accepted",
        };
        let total = accepted + rejected;
        let percent = if total > 0 {
            format!(
                "{}%",
                (accepted as f64 / total as f64 * 100.0).round()
            )
        } else {
            "0%".to_string()
        };

        let head = self.paint(foreground, word);
        let count = format!("{accepted}/{total}");
        let ratio = self.paint(MAGENTA, &format!(" ({percent})"));
        let time = format!(" ∙ {seconds:.1}s");
        let board = prefix(board_rate);
        let all = prefix(total_rate);
        let rate = if self.colour {
            format!(" ∙ {BLUE}{BRIGHT}{board}{DIM} ({all} total){RESET}")
        } else {
            format!(" ∙ {board} ({all} total)")
        };
        let tail = self.paint(CYAN, &format!(" ⚙ diff {difficulty} ∙ ping {ping_ms}ms"));
        let cause = if reason.is_empty() {
            String::new()
        } else {
            self.paint(RED, &format!(" - {reason}"))
        };

        println!(
            "{} {} {}{}{}{}{}{}{}",
            self.stamp(),
            self.tag(sender),
            head,
            count,
            ratio,
            time,
            rate,
            tail,
            cause
        );
    }
}

/* `date +%z` prints the offset from UTC as ±HHMM. */
fn local_offset() -> i64 {
    let out = match Command::new("date").arg("+%z").output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).trim().to_string(),
        Err(_) => return 0,
    };
    let sign: i64 = match out.chars().next() {
        Some('-') => -1,
        Some('+') => 1,
        _ => 0,
    };
    let digits: String = out.chars().skip(1).collect();
    if digits.len() < 4 {
        return 0;
    }
    let hours: i64 = digits[..2].parse().unwrap_or(0);
    let minutes: i64 = digits[2..4].parse().unwrap_or(0);
    sign * (hours * 3600 + minutes * 60)
}

pub fn prefix(rate: f64) -> String {
    /* A rig with no board reporting yet sums to negative zero, and Rust prints
       that as "-0.00". Nothing is mining until a rate is measured, so show a
       plain zero. */
    if rate <= 0.0 {
        return "0.00 H/s".to_string();
    }
    if rate >= 1000.0 {
        format!("{:.2} kH/s", rate / 1000.0)
    } else {
        format!("{:.2} H/s", rate)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_output_has_no_escape_codes_when_colour_is_off() {
        let out = Out { colour: false, offset: 0 };
        assert_eq!(out.tag("rig3"), "[rig3]");
        assert!(!out.paint(GREEN, "hi").contains('\x1b'));
    }

    #[test]
    fn a_board_tag_gets_the_magenta_background() {
        let out = Out { colour: true, offset: 0 };
        let line = out.tag("rig3");
        assert!(line.contains("\x1b[45m"));
        assert!(line.ends_with(RESET));
    }

    #[test]
    fn a_rig_that_has_not_mined_yet_reads_as_zero() {
        /* Rust's float sum over an empty iterator is negative zero, which would
           otherwise print as "-0.00 H/s" in the status line. */
        let nothing: Vec<f64> = Vec::new();
        assert_eq!(prefix(nothing.iter().sum()), "0.00 H/s");
        assert_eq!(prefix(0.0), "0.00 H/s");
    }

    #[test]
    fn rates_are_scaled_like_the_official_miner() {
        assert_eq!(prefix(1757.0), "1.76 kH/s");
        assert_eq!(prefix(12.5), "12.50 H/s");
    }
}
