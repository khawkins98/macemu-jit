#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod prefs;
mod rpc_client;
mod vm;

use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::process::{Child, Stdio};
use std::sync::{Arc, Mutex};
use tauri::State;
use vm::{CreateVmRequest, VmProfile};

#[derive(Default, Clone, serde::Serialize)]
pub struct VmLiveStats {
    pub timestamp: String,
    pub blocks: String,
    pub rate: String,
    pub compiled: String,
    pub jnk: String,
    pub jdr: String,
    pub jram: String,
    pub j2i: String,
    pub rss: String,
    pub cpu: String,
    pub warnings: String,
}

#[derive(Default, Clone, serde::Serialize)]
pub struct VmSignal {
    pub kind: String,
    pub payload: String,
}

#[derive(Default, Clone, serde::Serialize)]
pub struct VmInspectorState {
    pub stats: VmLiveStats,
    pub signals: Vec<VmSignal>,
    pub log_tail: Vec<String>,
}

struct RunningVm {
    child: Child,
    vncport: u16,
    rpc: Option<rpc_client::RpcClient>,
}

struct AppState {
    running: Mutex<HashMap<String, RunningVm>>,
    inspector: Arc<Mutex<HashMap<String, VmInspectorState>>>,
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
fn resize_disk(path: String, size_gb: f64) -> Result<(), String> {
    let p = std::path::Path::new(&path);
    if !p.exists() {
        return Err(format!("Disk image not found: {}", path));
    }
    let current_size = std::fs::metadata(p).map_err(|e| e.to_string())?.len();
    let new_size = (size_gb * 1024.0 * 1024.0 * 1024.0) as u64;
    if new_size < current_size {
        return Err(format!(
            "Cannot shrink: current size is {:.1} GB. Only growing is supported.",
            current_size as f64 / (1024.0 * 1024.0 * 1024.0)
        ));
    }
    let f = std::fs::OpenOptions::new()
        .write(true)
        .open(p)
        .map_err(|e| format!("Cannot open disk: {}", e))?;
    f.set_len(new_size)
        .map_err(|e| format!("Resize failed: {}", e))?;
    Ok(())
}

#[tauri::command]
fn remove_vm_disk(id: String, index: usize) -> Result<(), String> {
    let vm_dir = vm::vm_dir_for(&id);
    let prefs_path = vm_dir.join("prefs");
    let mut pf = prefs::load_prefs(&prefs_path)?;
    let disks: Vec<_> = pf.entries.iter().enumerate()
        .filter(|(_, e)| e.key == "disk")
        .map(|(i, _)| i)
        .collect();
    if index < disks.len() {
        pf.entries.remove(disks[index]);
        prefs::save_prefs(&prefs_path, &pf)?;
        vm::sync_profile_from_prefs(&id)?;
    }
    Ok(())
}

#[tauri::command]
fn update_vm_setting(id: String, key: String, value: String) -> Result<(), String> {
    if key == "name" {
        return vm::rename_profile(&id, &value);
    }
    if key == "description" {
        return vm::update_metadata(&id, |vm| vm.description = Some(value.clone()));
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

    // Spawn a thread to parse stderr for boot signals, OS version, and inspector stats
    let stderr = child.stderr.take();
    let vm_id = id.clone();
    let vm_dir_clone = vm_dir.clone();
    let inspector = Arc::clone(&state.inspector);
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
                    if line.contains("[BOOT]") && line.contains("frontApp='Finder'") {
                        let _ = vm::update_last_booted(&vm_id);
                    }

                    // Inspector: parse heartbeat + signals into live state
                    if let Ok(mut map) = inspector.lock() {
                        let state = map.entry(vm_id.clone()).or_default();

                        // Keep last 200 log lines
                        state.log_tail.push(line.clone());
                        if state.log_tail.len() > 200 {
                            state.log_tail.remove(0);
                        }

                        // Parse [HB] heartbeat
                        if line.starts_with("[HB ") {
                            let mut stats = VmLiveStats::default();
                            // [HB 10.0s] blocks=1.2M (0.5M/s) comp=847 | jNK=... | rss=... cpu=...
                            if let Some(ts) = line.get(4..line.find(']').unwrap_or(4)) {
                                stats.timestamp = ts.trim().to_string();
                            }
                            for part in line.split_whitespace() {
                                if let Some(v) = part.strip_prefix("blocks=") { stats.blocks = v.to_string(); }
                                if let Some(v) = part.strip_prefix("comp=") { stats.compiled = v.to_string(); }
                                if let Some(v) = part.strip_prefix("jNK=") { stats.jnk = v.to_string(); }
                                if let Some(v) = part.strip_prefix("jDR=") { stats.jdr = v.to_string(); }
                                if let Some(v) = part.strip_prefix("jRAM=") { stats.jram = v.to_string(); }
                                if let Some(v) = part.strip_prefix("j2i=") { stats.j2i = v.to_string(); }
                                if let Some(v) = part.strip_prefix("rss=") { stats.rss = v.to_string(); }
                                if let Some(v) = part.strip_prefix("cpu=") { stats.cpu = v.to_string(); }
                            }
                            if let Some(r) = line.find("M/s)") {
                                if let Some(s) = line[..r].rfind('(') {
                                    stats.rate = line[s+1..r].to_string() + "M/s";
                                }
                            }
                            if let Some(w) = line.find("[WARN:").or(line.find("[SUSPECT:")) {
                                stats.warnings = line[w..].to_string();
                            }
                            state.stats = stats;
                        }

                        // Parse signals: [BOOT], [SYSV], [APP], [READY], [STALL], [WARN]
                        for tag in &["[BOOT]", "[SYSV]", "[APP]", "[READY]", "[STALL]", "[JIT"] {
                            if line.contains(tag) {
                                state.signals.push(VmSignal {
                                    kind: tag.trim_matches(&['[', ']'] as &[char]).to_string(),
                                    payload: line.clone(),
                                });
                                if state.signals.len() > 100 {
                                    state.signals.remove(0);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        });
    }

    running.insert(id, RunningVm { child, vncport, rpc: None });

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

/// Ensure the RPC client is connected for a running VM (lazy connect)
fn ensure_rpc(running: &mut HashMap<String, RunningVm>, id: &str) -> Result<(), String> {
    let vm = running.get_mut(id).ok_or("VM not running")?;
    if vm.rpc.is_some() {
        return Ok(());
    }
    let vm_dir = vm::vm_dir_for(id);
    match rpc_client::RpcClient::connect_from_vm(&vm_dir) {
        Ok(client) => {
            vm.rpc = Some(client);
            Ok(())
        }
        Err(e) => Err(format!("RPC not available yet: {}", e)),
    }
}

#[tauri::command]
fn rpc_set_input_lockout(id: String, enabled: bool, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    vm.rpc.as_mut().unwrap()
        .invoke_int32(rpc_client::METHOD_INPUT_LOCKOUT, if enabled { 1 } else { 0 })
}

#[tauri::command]
fn rpc_set_frameskip(id: String, value: i32, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    vm.rpc.as_mut().unwrap()
        .invoke_int32(rpc_client::METHOD_FRAMESKIP, value)
}

#[tauri::command]
fn rpc_get_stats(id: String, state: State<AppState>) -> Result<String, String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    vm.rpc.as_mut().unwrap()
        .invoke_get_string(rpc_client::METHOD_GET_STATS)
}

#[tauri::command]
fn rpc_dump_registers(id: String, state: State<AppState>) -> Result<String, String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    vm.rpc.as_mut().unwrap()
        .invoke_get_string(rpc_client::METHOD_DUMP_REGISTERS)
}

#[tauri::command]
fn rpc_read_memory(id: String, addr: u32, len: u32, state: State<AppState>) -> Result<String, String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    let bytes = vm.rpc.as_mut().unwrap()
        .invoke_read_memory(addr, len.min(4096))?;

    // Format as hex dump with ASCII sidebar (classic debugger layout)
    let mut result = String::new();
    for (i, chunk) in bytes.chunks(16).enumerate() {
        let offset = i * 16;
        result.push_str(&format!("{:08x}  ", addr as usize + offset));
        for (j, b) in chunk.iter().enumerate() {
            result.push_str(&format!("{:02x} ", b));
            if j == 7 { result.push(' '); }
        }
        for _ in chunk.len()..16 {
            result.push_str("   ");
        }
        result.push_str(" |");
        for b in chunk {
            result.push(if *b >= 0x20 && *b < 0x7f { *b as char } else { '.' });
        }
        result.push_str("|\n");
    }
    Ok(result)
}

#[tauri::command]
fn read_file_contents(path: String) -> Result<String, String> {
    std::fs::read_to_string(&path).map_err(|e| format!("Cannot read file: {}", e))
}

#[tauri::command]
fn save_profile_session(session: String) -> Result<String, String> {
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let filename = format!("session-{}.sheepshaver-profile", ts);
    let path = std::env::temp_dir().join(&filename);
    std::fs::write(&path, &session).map_err(|e| format!("Save failed: {}", e))?;
    Ok(path.to_string_lossy().to_string())
}

#[tauri::command]
fn rpc_ui_snapshot(id: String, state: State<AppState>) -> Result<String, String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    ensure_rpc(&mut running, &id)?;
    let vm = running.get_mut(&id).unwrap();
    vm.rpc.as_mut().unwrap()
        .invoke_get_string(rpc_client::METHOD_UI_SNAPSHOT)
}

#[tauri::command]
fn get_vm_inspector(id: String, state: State<AppState>) -> Result<VmInspectorState, String> {
    let map = state.inspector.lock().map_err(|e| e.to_string())?;
    Ok(map.get(&id).cloned().unwrap_or_default())
}

#[tauri::command]
fn generate_bug_report(id: String, ui_screenshot_b64: Option<String>, state: State<AppState>) -> Result<String, String> {
    let vm_dir = vm::vm_dir_for(&id);
    let profile = vm::get_profile(&id)?;

    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let report_dir = std::env::temp_dir().join(format!("siliconsheep-report-{}", ts));
    std::fs::create_dir_all(&report_dir).map_err(|e| e.to_string())?;

    // 1. VM profile (sanitized — remove absolute paths for privacy)
    let profile_json = serde_json::to_string_pretty(&profile).unwrap_or_default();
    std::fs::write(report_dir.join("vm-profile.json"), &profile_json).ok();

    // 2. Prefs file
    let prefs_path = vm_dir.join("prefs");
    if prefs_path.exists() {
        std::fs::copy(&prefs_path, report_dir.join("prefs.txt")).ok();
    }

    // 3. Last run log
    let last_log = vm_dir.join("last_run.log");
    if last_log.exists() {
        if let Ok(target) = std::fs::read_link(&last_log) {
            std::fs::copy(&target, report_dir.join("last_run.log")).ok();
        } else {
            std::fs::copy(&last_log, report_dir.join("last_run.log")).ok();
        }
    }

    // 4. Guest screenshot (from VNC if available)
    let guest_screenshot = vm_dir.join("screenshot.png");
    if guest_screenshot.exists() {
        std::fs::copy(&guest_screenshot, report_dir.join("guest-screen.png")).ok();
    }

    // 5. UI screenshot (from frontend, base64 PNG)
    if let Some(ref b64) = ui_screenshot_b64 {
        if let Some(data) = b64.strip_prefix("data:image/png;base64,") {
            if let Ok(bytes) = base64_decode(data) {
                std::fs::write(report_dir.join("ui-screenshot.png"), &bytes).ok();
            }
        }
    }

    // 6. Inspector stats snapshot
    if let Ok(map) = state.inspector.lock() {
        if let Some(inspector) = map.get(&id) {
            let json = serde_json::to_string_pretty(inspector).unwrap_or_default();
            std::fs::write(report_dir.join("inspector-stats.json"), &json).ok();
        }
    }

    // 7. Host environment info
    let mut env_info = String::new();
    env_info.push_str(&format!("SiliconSheep version: {}\n", env!("CARGO_PKG_VERSION")));
    env_info.push_str(&format!("OS: {}\n", std::env::consts::OS));
    env_info.push_str(&format!("Arch: {}\n", std::env::consts::ARCH));
    if let Ok(output) = std::process::Command::new("sw_vers").output() {
        env_info.push_str(&format!("macOS: {}", String::from_utf8_lossy(&output.stdout)));
    }
    if let Some(emu) = find_emulator_binary() {
        env_info.push_str(&format!("Emulator: {}\n", emu));
    }
    std::fs::write(report_dir.join("environment.txt"), &env_info).ok();

    // 8. Create zip
    let zip_path = std::env::temp_dir().join(format!("siliconsheep-report-{}.zip", ts));
    let zip_file = std::fs::File::create(&zip_path).map_err(|e| e.to_string())?;
    let mut zip = zip::ZipWriter::new(zip_file);
    let options = zip::write::SimpleFileOptions::default();

    if let Ok(entries) = std::fs::read_dir(&report_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if let Ok(data) = std::fs::read(entry.path()) {
                zip.start_file(&name, options).ok();
                use std::io::Write;
                zip.write_all(&data).ok();
            }
        }
    }
    zip.finish().map_err(|e| e.to_string())?;

    // Cleanup temp dir
    std::fs::remove_dir_all(&report_dir).ok();

    Ok(zip_path.to_string_lossy().to_string())
}

fn base64_decode(input: &str) -> Result<Vec<u8>, String> {
    const TABLE: [u8; 128] = {
        let mut t = [255u8; 128];
        let chars = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut i = 0;
        while i < 64 {
            t[chars[i] as usize] = i as u8;
            i += 1;
        }
        t
    };
    let mut out = Vec::with_capacity(input.len() * 3 / 4);
    let bytes: Vec<u8> = input.bytes().filter(|&b| b != b'\n' && b != b'\r' && b != b' ').collect();
    for chunk in bytes.chunks(4) {
        if chunk.len() < 2 { break; }
        let a = TABLE.get(chunk[0] as usize).copied().unwrap_or(0);
        let b = TABLE.get(chunk[1] as usize).copied().unwrap_or(0);
        out.push((a << 2) | (b >> 4));
        if chunk.len() > 2 && chunk[2] != b'=' {
            let c = TABLE.get(chunk[2] as usize).copied().unwrap_or(0);
            out.push((b << 4) | (c >> 2));
            if chunk.len() > 3 && chunk[3] != b'=' {
                let d = TABLE.get(chunk[3] as usize).copied().unwrap_or(0);
                out.push((c << 6) | d);
            }
        }
    }
    Ok(out)
}

#[tauri::command]
fn set_runtime_control(id: String, key: String, value: String) -> Result<(), String> {
    let vm_dir = vm::vm_dir_for(&id);
    let control_path = vm_dir.join("runtime_control");

    // Read existing controls, update/add the key, write back
    let mut controls: std::collections::HashMap<String, String> = HashMap::new();
    if let Ok(content) = std::fs::read_to_string(&control_path) {
        for line in content.lines() {
            let parts: Vec<&str> = line.splitn(2, ' ').collect();
            if parts.len() == 2 {
                controls.insert(parts[0].to_string(), parts[1].to_string());
            }
        }
    }
    controls.insert(key, value);

    let content: String = controls.iter()
        .map(|(k, v)| format!("{} {}\n", k, v))
        .collect();
    std::fs::write(&control_path, content).map_err(|e| e.to_string())
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
            inspector: Arc::new(Mutex::new(HashMap::new())),
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
            resize_disk,
            remove_vm_disk,
            launch_vm,
            stop_vm,
            get_running_vms,
            check_emulator_status,
            import_from_prefs,
            get_vm_screenshot,
            get_vm_inspector,
            rpc_set_input_lockout,
            rpc_set_frameskip,
            rpc_get_stats,
            rpc_dump_registers,
            rpc_read_memory,
            rpc_ui_snapshot,
            read_file_contents,
            save_profile_session,
            generate_bug_report,
            set_runtime_control,
            capture_vm_screenshot,
            list_vm_logs,
            read_vm_log,
            reveal_vm_in_finder,
            backup_vm_disk,
        ])
        .run(tauri::generate_context!())
        .expect("error while running SiliconSheep");
}
