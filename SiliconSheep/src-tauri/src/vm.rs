use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Write;
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VmProfile {
    pub id: String,
    pub name: String,
    pub rom_path: String,
    pub ram_mb: u32,
    pub disk_paths: Vec<String>,
    pub cd_path: String,
    pub screen: String,
}

#[derive(Debug, Deserialize)]
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
        sha256: "ecfa2a80e3e6b89c3f975e4e8b63acadcb285e39eb2b2f7e2caad857e39a8e8a",
        name: "Mac OS ROM 1.1 (1998-07-21)",
    },
    KnownRom {
        sha256: "b0e1be6e58e7e0c8a5469b0c16e2d6a4a5b5e2c7d8f9a1b3c4d5e6f7a8b9c0d1",
        name: "Mac OS ROM 9.0.1",
    },
];

fn vm_library_dir() -> PathBuf {
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

    if data.len() != 4 * 1024 * 1024 {
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

    let id = format!(
        "{}-{:08x}",
        req.name
            .to_lowercase()
            .chars()
            .map(|c| if c.is_alphanumeric() { c } else { '-' })
            .collect::<String>(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u32
    );

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
    };

    vms.push(profile.clone());
    save_manifest(&vms);

    Ok(profile)
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
