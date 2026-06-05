#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod prefs;
mod vm;

use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::process::{Child, Stdio};
use std::sync::Mutex;
use tauri::State;
use vm::{CreateVmRequest, VmProfile};

struct RunningVm {
    child: Child,
    vncport: u16,
}

struct AppState {
    running: Mutex<HashMap<String, RunningVm>>,
}

#[tauri::command]
fn list_vms() -> Vec<VmProfile> {
    vm::list_profiles()
}

#[tauri::command]
fn create_vm(request: CreateVmRequest) -> Result<VmProfile, String> {
    vm::create_profile(&request)
}

#[tauri::command]
fn delete_vm(id: String) -> Result<(), String> {
    vm::delete_profile(&id)
}

#[tauri::command]
fn get_vm(id: String) -> Result<VmProfile, String> {
    vm::get_profile(&id)
}

#[tauri::command]
fn duplicate_vm(id: String, new_name: String) -> Result<VmProfile, String> {
    vm::duplicate_profile(&id, &new_name)
}

#[tauri::command]
fn verify_rom(path: String) -> Result<vm::RomInfo, String> {
    vm::verify_rom(&path)
}

#[tauri::command]
fn get_vm_prefs(id: String) -> Result<Vec<prefs::PrefEntry>, String> {
    let vm_dir = vm::vm_dir_for(&id);
    let prefs_path = vm_dir.join("prefs");
    let pf = prefs::load_prefs(&prefs_path)?;
    Ok(pf.entries)
}

#[tauri::command]
fn save_vm_prefs(id: String, entries: Vec<prefs::PrefEntry>) -> Result<(), String> {
    let vm_dir = vm::vm_dir_for(&id);
    let prefs_path = vm_dir.join("prefs");
    let pf = prefs::PrefsFile::from_entries(entries);
    prefs::save_prefs(&prefs_path, &pf)?;
    vm::sync_profile_from_prefs(&id)
}

#[tauri::command]
fn add_vm_disk(id: String, path: String, is_cdrom: bool) -> Result<(), String> {
    let vm_dir = vm::vm_dir_for(&id);
    let prefs_path = vm_dir.join("prefs");
    let mut pf = prefs::load_prefs(&prefs_path)?;
    let key = if is_cdrom { "cdrom" } else { "disk" };
    pf.add(key, &path);
    prefs::save_prefs(&prefs_path, &pf)?;
    vm::sync_profile_from_prefs(&id)
}

#[tauri::command]
fn update_vm_setting(id: String, key: String, value: String) -> Result<(), String> {
    if key == "name" {
        return vm::rename_profile(&id, &value);
    }
    let vm_dir = vm::vm_dir_for(&id);
    let prefs_path = vm_dir.join("prefs");
    let mut pf = prefs::load_prefs(&prefs_path)?;
    pf.set(&key, &value);
    prefs::save_prefs(&prefs_path, &pf)?;
    vm::sync_profile_from_prefs(&id)
}

#[tauri::command]
fn launch_vm(id: String, env_vars: Option<std::collections::HashMap<String, String>>, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if running.contains_key(&id) {
        return Err(format!("VM '{}' is already running", id));
    }

    // Check for shared disk images with any running VM
    let profile = vm::get_profile(&id)?;
    for (other_id, _) in running.iter() {
        if let Ok(other) = vm::get_profile(other_id) {
            for disk in &profile.disk_paths {
                if other.disk_paths.contains(disk) {
                    return Err(format!(
                        "Cannot start: disk '{}' is in use by VM '{}'",
                        disk.split('/').last().unwrap_or(disk), other.name
                    ));
                }
            }
        }
    }
    let vm_dir = vm::vm_dir_for(&id);

    let emu_path = find_emulator_binary()
        .ok_or("SheepShaver binary not found. Build it first: cd SheepShaver && make build-ss")?;

    let mut cmd = std::process::Command::new(&emu_path);
    cmd.arg(vm_dir.to_str().unwrap_or("."))
        .current_dir(&vm_dir)
        .stderr(Stdio::piped());

    if let Some(ref vars) = env_vars {
        for (k, v) in vars {
            if !v.is_empty() {
                cmd.env(k, v);
            }
        }
    }

    let mut child = cmd.spawn()
        .map_err(|e| format!("Failed to launch SheepShaver: {}", e))?;

    // Read VNC port from prefs (for screenshot capture)
    let vncport = {
        let prefs_path = vm_dir.join("prefs");
        prefs::load_prefs(&prefs_path)
            .ok()
            .and_then(|pf| pf.get_int("vncport").map(|v| v as u16))
            .unwrap_or(5900)
    };

    // Spawn a thread to parse stderr for boot signals and OS version
    let stderr = child.stderr.take();
    let vm_id = id.clone();
    let vm_dir_clone = vm_dir.clone();
    if let Some(stderr) = stderr {
        std::thread::spawn(move || {
            let reader = BufReader::new(stderr);

            // Timestamped log: logs/<timestamp>.log, plus symlink last_run.log → latest
            let logs_dir = vm_dir_clone.join("logs");
            std::fs::create_dir_all(&logs_dir).ok();
            let ts = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs();
            let log_name = format!("{}.log", ts);
            let log_path = logs_dir.join(&log_name);
            let mut log_file = std::fs::File::create(&log_path).ok();

            // Symlink last_run.log → latest timestamped log
            let symlink_path = vm_dir_clone.join("last_run.log");
            std::fs::remove_file(&symlink_path).ok();
            #[cfg(unix)]
            std::os::unix::fs::symlink(&log_path, &symlink_path).ok();

            // Prune old logs (keep last 10)
            if let Ok(entries) = std::fs::read_dir(&logs_dir) {
                let mut logs: Vec<_> = entries.filter_map(|e| e.ok()).collect();
                logs.sort_by_key(|e| std::cmp::Reverse(e.file_name()));
                for old in logs.into_iter().skip(10) {
                    std::fs::remove_file(old.path()).ok();
                }
            }
            for line in reader.lines() {
                if let Ok(line) = line {
                    // Write to log file in the .sheepvm bundle
                    if let Some(ref mut f) = log_file {
                        use std::io::Write;
                        writeln!(f, "{}", line).ok();
                    }
                    // Detect OS version from guest Gestalt read
                    // Format: [SYSV] osVersion=0x0860 (8.6.0)
                    if line.contains("[SYSV] osVersion=") {
                        if let Some(ver_str) = line.split('(').nth(1) {
                            let ver = ver_str.trim_end_matches(')').trim();
                            if !ver.is_empty() {
                                let os_name = format!("Mac OS {}", ver);
                                let _ = vm::update_os_version(&vm_id, &os_name);
                            }
                        }
                    }
                    // Detect boot-ready signal
                    // Format: [BOOT] idle frontApp='Finder' modal=0
                    if line.contains("[BOOT]") && line.contains("frontApp='Finder'") {
                        let _ = vm::update_last_booted(&vm_id);
                    }
                }
            }
        });
    }

    running.insert(id, RunningVm { child, vncport });

    Ok(())
}

#[tauri::command]
fn get_vm_screenshot(id: String) -> Result<Option<String>, String> {
    let vm_dir = vm::vm_dir_for(&id);
    let screenshot_path = vm_dir.join("screenshot.png");
    if screenshot_path.exists() {
        use std::fs;
        let data = fs::read(&screenshot_path).map_err(|e| e.to_string())?;
        let b64 = base64_encode(&data);
        Ok(Some(format!("data:image/png;base64,{}", b64)))
    } else {
        Ok(None)
    }
}

fn base64_encode(data: &[u8]) -> String {
    const CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = if chunk.len() > 1 { chunk[1] as u32 } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] as u32 } else { 0 };
        let n = (b0 << 16) | (b1 << 8) | b2;
        result.push(CHARS[((n >> 18) & 63) as usize] as char);
        result.push(CHARS[((n >> 12) & 63) as usize] as char);
        if chunk.len() > 1 { result.push(CHARS[((n >> 6) & 63) as usize] as char); } else { result.push('='); }
        if chunk.len() > 2 { result.push(CHARS[(n & 63) as usize] as char); } else { result.push('='); }
    }
    result
}

fn find_vncdotool() -> Option<String> {
    let candidates = [
        "../SheepShaver/e2e/.venv/bin/vncdotool",
    ];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(exe_dir) = exe.parent() {
            let p = exe_dir.join("../../../../SheepShaver/e2e/.venv/bin/vncdotool");
            if p.exists() {
                return Some(p.to_string_lossy().to_string());
            }
        }
    }
    for path in &candidates {
        if std::path::Path::new(path).exists() {
            return Some(path.to_string());
        }
    }
    if let Ok(output) = std::process::Command::new("which").arg("vncdotool").output() {
        if output.status.success() {
            let p = String::from_utf8_lossy(&output.stdout).trim().to_string();
            if !p.is_empty() { return Some(p); }
        }
    }
    None
}

#[tauri::command]
fn list_vm_logs(id: String) -> Result<Vec<String>, String> {
    let vm_dir = vm::vm_dir_for(&id);
    let logs_dir = vm_dir.join("logs");
    if !logs_dir.exists() {
        return Ok(Vec::new());
    }
    let mut logs: Vec<String> = std::fs::read_dir(&logs_dir)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.ends_with(".log"))
        .collect();
    logs.sort_by(|a, b| b.cmp(a));
    Ok(logs)
}

#[tauri::command]
fn read_vm_log(id: String, log_name: String) -> Result<String, String> {
    let vm_dir = vm::vm_dir_for(&id);
    let log_path = vm_dir.join("logs").join(&log_name);
    std::fs::read_to_string(&log_path).map_err(|e| format!("Cannot read log: {}", e))
}

fn find_capture_script() -> Option<std::path::PathBuf> {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(exe_dir) = exe.parent() {
            // Dev mode
            let p = exe_dir.join("../../../../SiliconSheep/src-tauri/scripts/vnc_capture.py");
            if p.exists() { return Some(p); }
            // Also try relative to repo root
            let p = exe_dir.join("../scripts/vnc_capture.py");
            if p.exists() { return Some(p); }
        }
    }
    let p = std::path::Path::new("SiliconSheep/src-tauri/scripts/vnc_capture.py");
    if p.exists() { return Some(p.to_path_buf()); }
    None
}

fn find_e2e_python() -> Option<String> {
    let candidates = [
        "../SheepShaver/e2e/.venv/bin/python3",
    ];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(exe_dir) = exe.parent() {
            let p = exe_dir.join("../../../../SheepShaver/e2e/.venv/bin/python3");
            if p.exists() { return Some(p.to_string_lossy().to_string()); }
        }
    }
    for path in &candidates {
        if std::path::Path::new(path).exists() {
            return Some(path.to_string());
        }
    }
    None
}

fn capture_vnc_screenshot(vncport: u16, output_path: &std::path::Path) -> Result<(), String> {
    let script = find_capture_script()
        .ok_or("vnc_capture.py not found")?;
    let python = find_e2e_python()
        .ok_or("E2E Python venv not found (run: cd SheepShaver/e2e && python3 -m venv .venv && pip install vncdotool Pillow)")?;

    let server = format!("localhost::{}", vncport);
    let result = std::process::Command::new(&python)
        .args([script.to_str().unwrap(), &server, output_path.to_str().unwrap_or("screenshot.png")])
        .output()
        .map_err(|e| format!("Failed to run capture script: {}", e))?;

    if result.status.success() {
        Ok(())
    } else {
        let stderr = String::from_utf8_lossy(&result.stderr);
        Err(format!("Screenshot capture failed: {}", stderr.trim()))
    }
}

#[tauri::command]
fn capture_vm_screenshot(id: String, state: State<AppState>) -> Result<(), String> {
    let running = state.running.lock().map_err(|e| e.to_string())?;
    let r = running.get(&id).ok_or("That VM is not running")?;
    let vm_dir = vm::vm_dir_for(&id);
    let screenshot_path = vm_dir.join("screenshot.png");
    capture_vnc_screenshot(r.vncport, &screenshot_path)
}

#[tauri::command]
fn stop_vm(id: String, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    let r = running.get(&id).ok_or("That VM is not running")?;

    let vm_dir = vm::vm_dir_for(&id);
    let screenshot_path = vm_dir.join("screenshot.png");
    capture_vnc_screenshot(r.vncport, &screenshot_path).ok();

    #[cfg(unix)]
    unsafe {
        libc::kill(r.child.id() as i32, libc::SIGUSR1);
    }
    running.remove(&id);
    Ok(())
}

#[tauri::command]
fn get_running_vms(state: State<AppState>) -> Vec<String> {
    let mut running = state.running.lock().unwrap_or_else(|e| e.into_inner());
    let mut dead: Vec<String> = Vec::new();
    for (id, r) in running.iter_mut() {
        match r.child.try_wait() {
            Ok(Some(_)) | Err(_) => dead.push(id.clone()),
            Ok(None) => {}
        }
    }
    for id in &dead {
        running.remove(id);
    }
    running.keys().cloned().collect()
}

#[tauri::command]
fn import_from_prefs(prefs_path: String, name: String) -> Result<vm::VmProfile, String> {
    vm::import_from_prefs_file(&prefs_path, &name)
}

fn find_emulator_binary() -> Option<String> {
    use std::path::PathBuf;

    let mut candidates: Vec<PathBuf> = Vec::new();

    // Relative to the Tauri executable (works in dev and production)
    if let Ok(exe) = std::env::current_exe() {
        if let Some(exe_dir) = exe.parent() {
            // Dev mode: exe is in SiliconSheep/src-tauri/target/{debug,release}/
            // → repo root is 4 levels up
            candidates.push(exe_dir.join("../../../../SheepShaver/src/Unix/SheepShaver"));
            // Production .app bundle: exe is in SiliconSheep.app/Contents/MacOS/
            // → sibling binary in the same dir or repo checkout nearby
            candidates.push(exe_dir.join("SheepShaver"));
        }
    }

    // Relative to cwd (works when launched from repo root)
    candidates.push(PathBuf::from("SheepShaver/src/Unix/SheepShaver"));
    candidates.push(PathBuf::from("../SheepShaver/src/Unix/SheepShaver"));

    // System-wide installs
    candidates.push(PathBuf::from("/usr/local/bin/SheepShaver"));
    candidates.push(PathBuf::from("/opt/homebrew/bin/SheepShaver"));

    // Check PATH via `which`
    if let Ok(output) = std::process::Command::new("which")
        .arg("SheepShaver")
        .output()
    {
        if output.status.success() {
            let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
            if !path.is_empty() {
                candidates.push(PathBuf::from(path));
            }
        }
    }

    for p in &candidates {
        if p.exists() {
            return Some(
                p.canonicalize()
                    .unwrap_or_else(|_| p.to_path_buf())
                    .to_string_lossy()
                    .to_string(),
            );
        }
    }
    None
}

#[tauri::command]
fn check_emulator_status() -> Result<EmulatorStatus, String> {
    match find_emulator_binary() {
        Some(path) => Ok(EmulatorStatus { found: true, path }),
        None => Ok(EmulatorStatus {
            found: false,
            path: String::new(),
        }),
    }
}

#[derive(serde::Serialize)]
struct EmulatorStatus {
    found: bool,
    path: String,
}

#[tauri::command]
fn reveal_vm_in_finder(id: String) -> Result<(), String> {
    let vm_dir = vm::vm_dir_for(&id);
    if !vm_dir.exists() {
        return Err("VM directory not found".to_string());
    }
    std::process::Command::new("open")
        .arg(&vm_dir)
        .spawn()
        .map_err(|e| format!("Failed to open Finder: {}", e))?;
    Ok(())
}

#[tauri::command]
fn backup_vm_disk(id: String) -> Result<String, String> {
    vm::backup_disk(&id)
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState {
            running: Mutex::new(HashMap::new()),
        })
        .invoke_handler(tauri::generate_handler![
            list_vms,
            create_vm,
            delete_vm,
            duplicate_vm,
            get_vm,
            verify_rom,
            get_vm_prefs,
            save_vm_prefs,
            update_vm_setting,
            add_vm_disk,
            launch_vm,
            stop_vm,
            get_running_vms,
            check_emulator_status,
            import_from_prefs,
            get_vm_screenshot,
            capture_vm_screenshot,
            list_vm_logs,
            read_vm_log,
            reveal_vm_in_finder,
            backup_vm_disk,
        ])
        .run(tauri::generate_context!())
        .expect("error while running SiliconSheep");
}
