use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Write;
use std::path::PathBuf;

#[cfg(target_os = "macos")]
extern "C" {
    fn clonefile(
        src: *const std::os::raw::c_char,
        dst: *const std::os::raw::c_char,
        flags: u32,
    ) -> std::os::raw::c_int;
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VmProfile {
    pub id: String,
    pub name: String,
    pub rom_path: String,
    pub ram_mb: u32,
    pub disk_paths: Vec<String>,
    pub cd_path: String,
    pub screen: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub shared_disk_warning: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub os_version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_booted: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateVmRequest {
    pub name: String,
    pub rom_path: String,
    pub ram_mb: u32,
    pub disk_mode: String,
    pub disk_size_gb: f64,
    pub disk_path: String,
    pub cd_path: String,
    pub screen: String,
}

#[derive(Debug, Serialize)]
pub struct RomInfo {
    pub valid: bool,
    pub name: String,
    pub sha256: String,
    pub status: String,
}

struct KnownRom {
    sha256: &'static str,
    name: &'static str,
}

const KNOWN_ROMS: &[KnownRom] = &[
    KnownRom {
        sha256: "d439f412c5dda7e2a498bb7b40a9ffd7bb542cf003586c45a7b5d7e24304cb09",
        name: "Mac OS ROM 1.1 (1998-07-21)",
    },
];

fn make_vm_id(name: &str) -> String {
    let slug: String = name
        .to_lowercase()
        .chars()
        .map(|c| if c.is_alphanumeric() { c } else { '-' })
        .collect();
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() as u32;
    format!("{}-{:08x}.sheepvm", slug, ts)
}

// Tests override this via VM_LIBRARY_DIR_OVERRIDE to use a temp directory
#[cfg(test)]
use std::sync::Mutex;
#[cfg(test)]
static TEST_DIR_OVERRIDE: Mutex<Option<PathBuf>> = Mutex::new(None);

fn vm_library_dir() -> PathBuf {
    #[cfg(test)]
    if let Some(ref dir) = *TEST_DIR_OVERRIDE.lock().unwrap() {
        fs::create_dir_all(dir).ok();
        return dir.clone();
    }

    let dir = dirs::data_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("SiliconSheep")
        .join("VMs");
    fs::create_dir_all(&dir).ok();
    dir
}

pub fn vm_dir_for(id: &str) -> PathBuf {
    vm_library_dir().join(id)
}

fn manifest_path() -> PathBuf {
    vm_library_dir().join("vms.json")
}

fn load_manifest() -> Vec<VmProfile> {
    match fs::read_to_string(manifest_path()) {
        Ok(data) => serde_json::from_str(&data).unwrap_or_default(),
        Err(_) => Vec::new(),
    }
}

fn save_manifest(vms: &[VmProfile]) {
    if let Ok(data) = serde_json::to_string_pretty(vms) {
        fs::write(manifest_path(), data).ok();
    }
}

pub fn list_profiles() -> Vec<VmProfile> {
    load_manifest()
}

pub fn get_profile(id: &str) -> Result<VmProfile, String> {
    load_manifest()
        .into_iter()
        .find(|vm| vm.id == id)
        .ok_or_else(|| format!("VM '{}' not found", id))
}

pub fn verify_rom(path: &str) -> Result<RomInfo, String> {
    let data = fs::read(path).map_err(|e| format!("Cannot read ROM: {}", e))?;
    let size = data.len();

    // SheepShaver accepts multiple ROM formats: raw 4 MB, CHRP 3 MB, compressed/
    // trimmed OldWorld (~1.8 MB), and parcels-format (variable). Only reject files
    // that are clearly not ROMs (too small to be useful, or too large).
    if size < 512 * 1024 || size > 8 * 1024 * 1024 {
        return Ok(RomInfo {
            valid: false,
            name: String::new(),
            sha256: String::new(),
            status: "error".to_string(),
        });
    }

    let hash = format!("{:x}", Sha256::digest(&data));

    let known = KNOWN_ROMS.iter().find(|r| r.sha256 == hash);

    Ok(RomInfo {
        valid: true,
        name: known.map_or_else(String::new, |r| r.name.to_string()),
        sha256: hash,
        status: if known.is_some() {
            "verified".to_string()
        } else {
            "accepted".to_string()
        },
    })
}

pub fn create_profile(req: &CreateVmRequest) -> Result<VmProfile, String> {
    let mut vms = load_manifest();
    let id = make_vm_id(&req.name);

    let vm_dir = vm_library_dir().join(&id);
    fs::create_dir_all(&vm_dir).map_err(|e| format!("Failed to create VM directory: {}", e))?;

    let mut disk_paths = Vec::new();

    if req.disk_mode == "create" {
        let size_bytes = (req.disk_size_gb * 1024.0 * 1024.0 * 1024.0) as u64;
        let disk_path = vm_dir.join("disk.dsk");
        let file =
            fs::File::create(&disk_path).map_err(|e| format!("Failed to create disk: {}", e))?;
        file.set_len(size_bytes)
            .map_err(|e| format!("Failed to set disk size: {}", e))?;
        disk_paths.push(disk_path.to_string_lossy().to_string());
    } else if !req.disk_path.is_empty() {
        disk_paths.push(req.disk_path.clone());
    }

    let mut prefs = String::new();
    prefs.push_str(&format!("rom {}\n", req.rom_path));
    prefs.push_str(&format!("ramsize {}M\n", req.ram_mb));
    prefs.push_str(&format!("screen {}\n", req.screen));
    for disk in &disk_paths {
        prefs.push_str(&format!("disk {}\n", disk));
    }
    if !req.cd_path.is_empty() {
        prefs.push_str(&format!("cdrom {}\n", req.cd_path));
        prefs.push_str("bootdriver -62\n");
    }
    prefs.push_str("ether slirp\n");
    prefs.push_str("nosound true\n");

    // VNC enabled by default for screenshot capture; random port in 5900-5999 to avoid collisions
    let vncport = 5900 + (std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() % 100) as u16;
    prefs.push_str("vncserver true\n");
    prefs.push_str(&format!("vncport {}\n", vncport));

    let prefs_path = vm_dir.join("prefs");
    let mut f =
        fs::File::create(&prefs_path).map_err(|e| format!("Failed to write prefs: {}", e))?;
    f.write_all(prefs.as_bytes())
        .map_err(|e| format!("Failed to write prefs: {}", e))?;

    let profile = VmProfile {
        id,
        name: req.name.clone(),
        rom_path: req.rom_path.clone(),
        ram_mb: req.ram_mb,
        disk_paths,
        cd_path: req.cd_path.clone(),
        screen: req.screen.clone(),
        shared_disk_warning: None,
        os_version: None,
        last_booted: None,
    };

    vms.push(profile.clone());
    save_manifest(&vms);

    Ok(profile)
}

pub fn import_from_prefs_file(prefs_path: &str, name: &str) -> Result<VmProfile, String> {
    let pf = crate::prefs::load_prefs(std::path::Path::new(prefs_path))?;

    let rom_path = pf.get("rom").unwrap_or("").to_string();
    let ram_mb = pf.get_int("ramsize").map(|v| (v / (1024 * 1024)) as u32).unwrap_or(256);
    let screen = pf.get("screen").unwrap_or("win/800/600").to_string();
    let disk_paths: Vec<String> = pf.get_all("disk").iter().map(|s| s.to_string()).collect();
    let cd_path = pf.get("cdrom").unwrap_or("").to_string();

    let mut vms = load_manifest();
    let id = make_vm_id(name);
    let vm_dir = vm_library_dir().join(&id);
    fs::create_dir_all(&vm_dir).map_err(|e| format!("Failed to create VM directory: {}", e))?;

    // Copy the prefs file into the bundle
    let dest_prefs = vm_dir.join("prefs");
    fs::copy(prefs_path, &dest_prefs)
        .map_err(|e| format!("Failed to copy prefs: {}", e))?;

    let profile = VmProfile {
        id,
        name: name.to_string(),
        rom_path,
        ram_mb,
        disk_paths,
        cd_path,
        screen,
        shared_disk_warning: None,
        os_version: None,
        last_booted: None,
    };

    vms.push(profile.clone());
    save_manifest(&vms);
    Ok(profile)
}

pub fn update_last_booted(id: &str) -> Result<(), String> {
    let mut vms = load_manifest();
    if let Some(vm) = vms.iter_mut().find(|v| v.id == id) {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        vm.last_booted = Some(format!("{}", now));

        // Try to detect OS version from disk image names if not already set
        if vm.os_version.is_none() {
            vm.os_version = detect_os_version_from_disks(&vm.disk_paths);
        }
        save_manifest(&vms);
    }
    Ok(())
}

fn detect_os_version_from_disks(disk_paths: &[String]) -> Option<String> {
    for path in disk_paths {
        let lower = path.to_lowercase();
        if lower.contains("macos86") || lower.contains("mac os 8.6") || lower.contains("8.6") {
            return Some("Mac OS 8.6".to_string());
        }
        if lower.contains("macos9") || lower.contains("mac os 9") || lower.contains("9.0") {
            return Some("Mac OS 9".to_string());
        }
        if lower.contains("macos8") || lower.contains("mac os 8") || lower.contains("8.1") || lower.contains("8.5") {
            return Some("Mac OS 8".to_string());
        }
        if lower.contains("os 7") || lower.contains("system 7") {
            return Some("System 7".to_string());
        }
    }
    None
}

pub fn rename_profile(id: &str, new_name: &str) -> Result<(), String> {
    let mut vms = load_manifest();
    if let Some(vm) = vms.iter_mut().find(|v| v.id == id) {
        vm.name = new_name.to_string();
        save_manifest(&vms);
        Ok(())
    } else {
        Err(format!("VM '{}' not found", id))
    }
}

pub fn duplicate_profile(id: &str, new_name: &str) -> Result<VmProfile, String> {
    let source = get_profile(id)?;
    let mut vms = load_manifest();
    let new_id = make_vm_id(new_name);

    let source_dir = vm_library_dir().join(id);
    let dest_dir = vm_library_dir().join(&new_id);

    copy_dir_recursive(&source_dir, &dest_dir)
        .map_err(|e| format!("Failed to copy VM: {}", e))?;

    let source_dir_str = source_dir.to_string_lossy().to_string();
    let dest_dir_str = dest_dir.to_string_lossy().to_string();

    let mut shared_external_disks = Vec::new();
    let new_disk_paths: Vec<String> = source
        .disk_paths
        .iter()
        .map(|p| {
            let rewritten = p.replace(&source_dir_str, &dest_dir_str);
            if rewritten == *p {
                // Path wasn't inside the source bundle — both VMs will point at the same file.
                // This is a data-corruption risk if both run simultaneously.
                shared_external_disks.push(p.clone());
            }
            rewritten
        })
        .collect();

    let new_cd_path = source.cd_path.replace(&source_dir_str, &dest_dir_str);

    let prefs_path = dest_dir.join("prefs");
    if let Ok(content) = fs::read_to_string(&prefs_path) {
        let updated = content.replace(&source_dir_str, &dest_dir_str);
        fs::write(&prefs_path, updated).ok();
    }

    let new_profile = VmProfile {
        id: new_id,
        name: new_name.to_string(),
        rom_path: source.rom_path,
        ram_mb: source.ram_mb,
        disk_paths: new_disk_paths,
        cd_path: new_cd_path,
        screen: source.screen,
        shared_disk_warning: if shared_external_disks.is_empty() {
            None
        } else {
            Some(format!(
                "Warning: {} disk(s) live outside the VM bundle and are shared with the original. Running both VMs simultaneously risks data corruption: {}",
                shared_external_disks.len(),
                shared_external_disks.join(", ")
            ))
        },
        os_version: source.os_version,
        last_booted: source.last_booted,
    };

    vms.push(new_profile.clone());
    save_manifest(&vms);
    Ok(new_profile)
}

pub fn sync_profile_from_prefs(id: &str) -> Result<(), String> {
    let vm_dir = vm_dir_for(id);
    let prefs_path = vm_dir.join("prefs");
    let pf = crate::prefs::load_prefs(&prefs_path)?;

    let mut vms = load_manifest();
    if let Some(vm) = vms.iter_mut().find(|v| v.id == id) {
        if let Some(v) = pf.get("rom") {
            vm.rom_path = v.to_string();
        }
        if let Some(v) = pf.get_int("ramsize") {
            vm.ram_mb = (v / (1024 * 1024)) as u32;
        }
        if let Some(v) = pf.get("screen") {
            vm.screen = v.to_string();
        }
        vm.disk_paths = pf.get_all("disk").iter().map(|s| s.to_string()).collect();
        vm.cd_path = pf.get("cdrom").unwrap_or("").to_string();
        save_manifest(&vms);
    }
    Ok(())
}

fn copy_dir_recursive(src: &std::path::Path, dst: &std::path::Path) -> std::io::Result<()> {
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        if src_path.is_dir() {
            copy_dir_recursive(&src_path, &dst_path)?;
        } else {
            #[cfg(target_os = "macos")]
            {
                use std::ffi::CString;
                let src_c = CString::new(src_path.to_str().unwrap()).unwrap();
                let dst_c = CString::new(dst_path.to_str().unwrap()).unwrap();
                let ret = unsafe {
                    clonefile(src_c.as_ptr(), dst_c.as_ptr(), 0)
                };
                if ret != 0 {
                    fs::copy(&src_path, &dst_path)?;
                }
            }
            #[cfg(not(target_os = "macos"))]
            {
                fs::copy(&src_path, &dst_path)?;
            }
        }
    }
    Ok(())
}

pub fn backup_disk(id: &str) -> Result<String, String> {
    let profile = get_profile(id)?;
    if profile.disk_paths.is_empty() {
        return Err("No disks to back up".to_string());
    }

    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();

    let mut backed_up = Vec::new();
    for disk_path in &profile.disk_paths {
        let src = std::path::Path::new(disk_path);
        if !src.exists() {
            continue;
        }
        let stem = src.file_stem().unwrap_or_default().to_string_lossy();
        let ext = src.extension().unwrap_or_default().to_string_lossy();
        let backup_name = format!("{}-backup-{}.{}", stem, timestamp, ext);
        let dst = src.with_file_name(&backup_name);

        #[cfg(target_os = "macos")]
        {
            use std::ffi::CString;
            let src_c = CString::new(src.to_str().unwrap()).map_err(|e| e.to_string())?;
            let dst_c = CString::new(dst.to_str().unwrap()).map_err(|e| e.to_string())?;
            let ret = unsafe { clonefile(src_c.as_ptr(), dst_c.as_ptr(), 0) };
            if ret != 0 {
                fs::copy(src, &dst).map_err(|e| format!("Backup failed: {}", e))?;
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            fs::copy(src, &dst).map_err(|e| format!("Backup failed: {}", e))?;
        }
        backed_up.push(backup_name);
    }

    if backed_up.is_empty() {
        return Err("No disk files found to back up".to_string());
    }

    Ok(format!("Backed up: {}", backed_up.join(", ")))
}

pub fn delete_profile(id: &str) -> Result<(), String> {
    let mut vms = load_manifest();
    let initial_len = vms.len();
    vms.retain(|vm| vm.id != id);

    if vms.len() == initial_len {
        return Err(format!("VM '{}' not found", id));
    }

    save_manifest(&vms);

    let vm_dir = vm_library_dir().join(id);
    if vm_dir.exists() {
        fs::remove_dir_all(&vm_dir).ok();
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn with_temp_dir<F: FnOnce(&Path)>(f: F) {
        let dir = std::env::temp_dir().join(format!("silicon-sheep-test-{:x}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos() as u64));
        fs::create_dir_all(&dir).unwrap();
        *TEST_DIR_OVERRIDE.lock().unwrap() = Some(dir.clone());
        f(&dir);
        *TEST_DIR_OVERRIDE.lock().unwrap() = None;
        fs::remove_dir_all(&dir).ok();
    }

    fn sample_request() -> CreateVmRequest {
        CreateVmRequest {
            name: "Test Mac".to_string(),
            rom_path: "/Users/Shared/macemu/test.rom".to_string(),
            ram_mb: 256,
            disk_mode: "create".to_string(),
            disk_size_gb: 2.0,
            disk_path: String::new(),
            cd_path: String::new(),
            screen: "win/1024/768".to_string(),
        }
    }

    #[test]
    fn test_create_profile_produces_bootable_prefs() {
        with_temp_dir(|_| {
            let profile = create_profile(&sample_request()).unwrap();

            assert!(profile.id.ends_with(".sheepvm"), "VM dir must end with .sheepvm");

            let prefs_path = vm_library_dir().join(&profile.id).join("prefs");
            assert!(prefs_path.exists(), "prefs file must exist");

            let content = fs::read_to_string(&prefs_path).unwrap();
            let pf = crate::prefs::PrefsFile::parse(&content);

            assert_eq!(pf.get("rom"), Some("/Users/Shared/macemu/test.rom"));
            assert_eq!(pf.get("ramsize"), Some("256M"));
            assert_eq!(pf.get("screen"), Some("win/1024/768"));
            assert!(pf.get("ether").is_some(), "networking should be configured");
            assert!(!pf.get_all("disk").is_empty(), "at least one disk must be attached");
        });
    }

    #[test]
    fn test_create_profile_with_cd_sets_bootdriver() {
        with_temp_dir(|_| {
            let mut req = sample_request();
            req.cd_path = "/path/to/install.iso".to_string();
            req.disk_mode = "existing".to_string();
            req.disk_path = String::new();

            let profile = create_profile(&req).unwrap();
            let prefs_path = vm_library_dir().join(&profile.id).join("prefs");
            let content = fs::read_to_string(&prefs_path).unwrap();
            let pf = crate::prefs::PrefsFile::parse(&content);

            assert_eq!(pf.get("cdrom"), Some("/path/to/install.iso"));
            assert_eq!(pf.get("bootdriver"), Some("-62"), "CD boot must set bootdriver -62");
        });
    }

    #[test]
    fn test_create_disk_image() {
        with_temp_dir(|_| {
            let profile = create_profile(&sample_request()).unwrap();
            assert_eq!(profile.disk_paths.len(), 1);
            let disk = Path::new(&profile.disk_paths[0]);
            assert!(disk.exists(), "disk image must be created");
            let meta = fs::metadata(disk).unwrap();
            assert_eq!(meta.len(), 2 * 1024 * 1024 * 1024, "disk should be 2 GB");
        });
    }

    #[test]
    fn test_create_delete_roundtrip() {
        with_temp_dir(|_| {
            let profile = create_profile(&sample_request()).unwrap();
            let id = profile.id.clone();

            assert_eq!(list_profiles().len(), 1);
            assert!(get_profile(&id).is_ok());

            delete_profile(&id).unwrap();
            assert_eq!(list_profiles().len(), 0);
            assert!(get_profile(&id).is_err());
            assert!(!vm_library_dir().join(&id).exists(), "VM dir should be removed");
        });
    }

    #[test]
    fn test_rename_profile() {
        with_temp_dir(|_| {
            let profile = create_profile(&sample_request()).unwrap();
            rename_profile(&profile.id, "Renamed Mac").unwrap();
            let updated = get_profile(&profile.id).unwrap();
            assert_eq!(updated.name, "Renamed Mac");

            delete_profile(&profile.id).unwrap();
        });
    }

    #[test]
    fn test_duplicate_profile() {
        with_temp_dir(|_| {
            let original = create_profile(&sample_request()).unwrap();
            let copy = duplicate_profile(&original.id, "Copy of Test").unwrap();

            assert_ne!(original.id, copy.id);
            assert!(copy.id.ends_with(".sheepvm"));
            assert_eq!(copy.name, "Copy of Test");
            assert_eq!(list_profiles().len(), 2);

            for disk in &copy.disk_paths {
                assert!(disk.contains(&copy.id), "disk path should reference the new VM dir");
                assert!(!disk.contains(&original.id), "disk path must not reference the original");
            }

            let copy_prefs_path = vm_library_dir().join(&copy.id).join("prefs");
            assert!(copy_prefs_path.exists());

            delete_profile(&original.id).unwrap();
            delete_profile(&copy.id).unwrap();
        });
    }

    #[test]
    fn test_sync_profile_from_prefs() {
        with_temp_dir(|_| {
            let profile = create_profile(&sample_request()).unwrap();

            let prefs_path = vm_library_dir().join(&profile.id).join("prefs");
            let mut pf = crate::prefs::load_prefs(&prefs_path).unwrap();
            pf.set("ramsize", "512M");
            pf.set("screen", "win/800/600");
            crate::prefs::save_prefs(&prefs_path, &pf).unwrap();

            sync_profile_from_prefs(&profile.id).unwrap();
            let updated = get_profile(&profile.id).unwrap();
            assert_eq!(updated.ram_mb, 512);
            assert_eq!(updated.screen, "win/800/600");

            delete_profile(&profile.id).unwrap();
        });
    }

    #[test]
    fn test_duplicate_warns_on_external_disks() {
        with_temp_dir(|_| {
            let mut req = sample_request();
            req.disk_mode = "existing".to_string();
            req.disk_path = "/Users/Shared/macemu/external.dsk".to_string();

            let original = create_profile(&req).unwrap();
            let copy = duplicate_profile(&original.id, "Copy").unwrap();

            assert!(copy.shared_disk_warning.is_some(),
                "should warn about shared external disk");
            assert!(copy.shared_disk_warning.as_ref().unwrap().contains("external.dsk"));

            delete_profile(&original.id).unwrap();
            delete_profile(&copy.id).unwrap();
        });
    }

    #[test]
    fn test_vm_id_has_sheepvm_suffix() {
        let id = make_vm_id("My Cool Mac");
        assert!(id.ends_with(".sheepvm"), "ID must end with .sheepvm, got: {}", id);
        assert!(id.starts_with("my-cool-mac-"), "ID should start with slugified name");
    }
}
