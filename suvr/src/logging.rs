//! Fixed file logging with startup rotation (max 3), mirroring the C
//! `setup_logging`: suvr.3.log dropped, 2->3, 1->2, fresh suvr.1.log. Captures
//! our logs plus egui/winit via the `log` facade. Level: Debug during tests.

use log::{Level, LevelFilter, Log, Metadata, Record};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;

struct FileLogger {
    file: Mutex<File>,
}

impl Log for FileLogger {
    fn enabled(&self, m: &Metadata) -> bool {
        m.level() <= Level::Debug
    }
    fn log(&self, r: &Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        let line = format!("[{:<5} {}] {}\n", r.level(), r.target(), r.args());
        if let Ok(mut f) = self.file.lock() {
            let _ = f.write_all(line.as_bytes());
        }
        // Mirror errors to stderr so run-suvr.sh's tee still shows them.
        if r.level() <= Level::Error {
            eprint!("{line}");
        }
    }
    fn flush(&self) {
        if let Ok(mut f) = self.file.lock() {
            let _ = f.flush();
        }
    }
}

fn log_dir() -> PathBuf {
    if let Ok(p) = std::env::var("SUVR_LOG_DIR") {
        return PathBuf::from(p);
    }
    // Next to the executable, like the C side keeps its log/ folder.
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("log")))
        .unwrap_or_else(|| PathBuf::from("log"))
}

fn rotate(dir: &PathBuf) {
    let p = |i: u32| dir.join(format!("suvr.{i}.log"));
    let _ = fs::remove_file(p(3));
    let _ = fs::rename(p(2), p(3));
    let _ = fs::rename(p(1), p(2));
}

/// Rotate and start logging. Call once at startup. Never panics on failure.
pub fn init() {
    let dir = log_dir();
    let _ = fs::create_dir_all(&dir);
    rotate(&dir);
    let file = match OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(dir.join("suvr.1.log"))
    {
        Ok(f) => f,
        Err(e) => {
            eprintln!("logging disabled: {e}");
            return;
        }
    };
    let logger = Box::new(FileLogger { file: Mutex::new(file) });
    if log::set_boxed_logger(logger).is_ok() {
        log::set_max_level(LevelFilter::Debug);
        log::info!("suvr logging started, dir={}", dir.display());
    }
}
