#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod prefs;
mod vm;

use std::sync::Mutex;
use tauri::State;
use vm::{CreateVmRequest, VmProfile};

struct RunningVm {
    id: String,
    #[allow(dead_code)]
    pid: u32,
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
fn verify_rom(path: String) -> Result<vm::RomInfo, String> {
    vm::verify_rom(&path)
}

#[tauri::command]
fn launch_vm(id: String, state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if let Some(ref r) = *running {
        return Err(format!("VM '{}' is already running", r.id));
    }

    let profile = vm::get_profile(&id)?;
    let vm_dir = vm::vm_dir_for(&id);

    let child = std::process::Command::new("../SheepShaver/src/Unix/SheepShaver")
        .arg(vm_dir.to_str().unwrap_or("."))
        .current_dir(&vm_dir)
        .spawn()
        .map_err(|e| format!("Failed to launch SheepShaver: {}", e))?;

    let pid = child.id();
    *running = Some(RunningVm { id: profile.id, pid });

    Ok(())
}

#[tauri::command]
fn stop_vm(state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if let Some(ref r) = *running {
        #[cfg(unix)]
        {
            use std::process::Command;
            Command::new("kill")
                .args(["-SIGUSR1", &r.pid.to_string()])
                .output()
                .ok();
        }
        *running = None;
        Ok(())
    } else {
        Err("No VM is running".to_string())
    }
}

#[tauri::command]
fn is_vm_running(state: State<AppState>) -> Option<String> {
    let running = state.running.lock().ok()?;
    running.as_ref().map(|r| r.id.clone())
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
            get_vm,
            verify_rom,
            launch_vm,
            stop_vm,
            is_vm_running,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Silicon Sheep");
}
