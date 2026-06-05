#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod prefs;
mod vm;

use std::process::Child;
use std::sync::Mutex;
use tauri::State;
use vm::{CreateVmRequest, VmProfile};

struct RunningVm {
    id: String,
    child: Child,
}

struct AppState {
    running: Mutex<Option<RunningVm>>,
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
fn launch_vm(id: String, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if let Some(ref r) = *running {
        return Err(format!("VM '{}' is already running", r.id));
    }

    let _profile = vm::get_profile(&id)?;
    let vm_dir = vm::vm_dir_for(&id);

    let emu_path = find_emulator_binary()
        .ok_or("SheepShaver binary not found. Build it first: cd SheepShaver && make build-ss")?;

    let child = std::process::Command::new(&emu_path)
        .arg(vm_dir.to_str().unwrap_or("."))
        .current_dir(&vm_dir)
        .spawn()
        .map_err(|e| format!("Failed to launch SheepShaver: {}", e))?;

    *running = Some(RunningVm { id, child });

    Ok(())
}

#[tauri::command]
fn stop_vm(state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if let Some(ref r) = *running {
        #[cfg(unix)]
        unsafe {
            libc::kill(r.child.id() as i32, libc::SIGUSR1);
        }
        *running = None;
        Ok(())
    } else {
        Err("No VM is running".to_string())
    }
}

#[tauri::command]
fn is_vm_running(state: State<AppState>) -> Option<String> {
    let mut running = state.running.lock().ok()?;
    if let Some(ref mut r) = *running {
        match r.child.try_wait() {
            Ok(Some(_)) => {
                *running = None;
                None
            }
            Ok(None) => Some(r.id.clone()),
            Err(_) => {
                *running = None;
                None
            }
        }
    } else {
        None
    }
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
            // Production .app bundle: exe is in Silicon Sheep.app/Contents/MacOS/
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
            running: Mutex::new(None),
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
            is_vm_running,
            check_emulator_status,
            reveal_vm_in_finder,
            backup_vm_disk,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Silicon Sheep");
}
