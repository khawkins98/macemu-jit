import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";

interface VmProfile {
  id: string;
  name: string;
  rom_path: string;
  ram_mb: number;
  disk_paths: string[];
  cd_path: string;
  screen: string;
}

interface RomInfo {
  valid: boolean;
  name: string;
  sha256: string;
  status: string;
}

type View = "library" | "wizard" | "settings";

let currentView: View = "library";
let vms: VmProfile[] = [];
let selectedVmId: string | null = null;
let runningVmId: string | null = null;
let wizardStep = 0;
let wizardState = {
  romPath: "",
  romStatus: "" as "" | "verified" | "accepted" | "error",
  romName: "",
  diskPath: "",
  diskMode: "create" as "create" | "existing",
  diskSizeGb: 2,
  cdPath: "",
  vmName: "My Mac",
  ramMb: 256,
  screen: "win/1024/768",
};

async function loadVms(): Promise<VmProfile[]> {
  try {
    return (await invoke("list_vms")) as VmProfile[];
  } catch {
    return [];
  }
}

async function checkRunning(): Promise<string | null> {
  try {
    return (await invoke("is_vm_running")) as string | null;
  } catch {
    return null;
  }
}

function escapeHtml(str: string): string {
  const div = document.createElement("div");
  div.textContent = str;
  return div.innerHTML;
}

function escapeAttr(str: string): string {
  return str
    .replace(/&/g, "&amp;")
    .replace(/"/g, "&quot;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

function fileName(path: string): string {
  return path.split("/").pop() || path;
}

function renderTitlebar(): string {
  return `<div class="titlebar">Silicon Sheep</div>`;
}

function renderVmCard(vm: VmProfile): string {
  const isRunning = vm.id === runningVmId;
  return `
    <div class="vm-card" data-id="${escapeAttr(vm.id)}">
      <div class="screenshot">
        <span class="screenshot-icon">🖥</span>
      </div>
      <div class="card-body">
        <div class="name">${escapeHtml(vm.name)}</div>
        <div class="meta">${vm.ram_mb} MB RAM · ${vm.disk_paths.length} disk(s)</div>
        <div class="card-actions">
          ${isRunning
            ? `<button class="btn btn-secondary btn-sm" data-action="stop" data-id="${escapeAttr(vm.id)}">◼ Stop</button>`
            : `<button class="btn btn-primary btn-sm" data-action="launch" data-id="${escapeAttr(vm.id)}">▶ Start</button>`
          }
          <button class="btn btn-secondary btn-sm" data-action="settings" data-id="${escapeAttr(vm.id)}">⚙</button>
          <button class="btn btn-secondary btn-sm" data-action="duplicate" data-id="${escapeAttr(vm.id)}" data-name="${escapeAttr(vm.name)}">⎘</button>
          <button class="btn btn-secondary btn-sm btn-danger-hover" data-action="delete" data-id="${escapeAttr(vm.id)}">✕</button>
        </div>
        ${isRunning ? '<span class="status running">Running</span>' : ""}
      </div>
    </div>
  `;
}

function renderEmptyState(): string {
  return `
    <div class="empty-state">
      <div class="empty-icon">🐑</div>
      <h2>No Virtual Machines</h2>
      <p>Create your first classic Mac virtual machine to get started with Mac OS 8 or 9 on Apple Silicon.</p>
      <button class="btn btn-primary btn-lg" data-action="wizard">Create Virtual Machine</button>
    </div>
  `;
}

function renderErrorBanner(): string {
  if (!errorBanner) return "";
  return `<div class="error-banner">${escapeHtml(errorBanner)}</div>`;
}

function renderLibrary(): string {
  if (vms.length === 0) {
    return `${renderTitlebar()}${renderErrorBanner()}${renderEmptyState()}`;
  }
  return `
    ${renderTitlebar()}
    ${renderErrorBanner()}
    <div class="header">
      <h1>Virtual Machines</h1>
      <button class="btn btn-primary" data-action="wizard">+ New VM</button>
    </div>
    <div class="vm-grid">
      ${vms.map(renderVmCard).join("")}
    </div>
  `;
}

function renderWizardStep(): string {
  const steps = ["Welcome", "ROM File", "Disk Image", "Review"];
  const progress = steps
    .map(
      (s, i) => `
    <div class="wizard-step-indicator ${i === wizardStep ? "active" : ""} ${i < wizardStep ? "done" : ""}">
      <span class="step-num">${i < wizardStep ? "✓" : i + 1}</span>
      <span class="step-label">${s}</span>
    </div>
  `
    )
    .join('<div class="step-connector"></div>');

  let content = "";

  switch (wizardStep) {
    case 0:
      content = `
        <div class="wizard-welcome">
          <div class="welcome-icon">🐑</div>
          <h2>Silicon Sheep</h2>
          <p class="subtitle">Classic Mac OS on Apple Silicon</p>
          <p class="welcome-desc">Set up a virtual Power Macintosh running Mac OS 8 or 9. You'll need a ROM file — everything else can be created for you.</p>
          <button class="btn btn-primary btn-lg" data-action="wizard-next">Get Started</button>
        </div>
      `;
      break;

    case 1:
      content = `
        <div class="wizard-section">
          <h2>Macintosh ROM File</h2>
          <p>SheepShaver needs a ROM image from a real Power Macintosh to boot. This is a 4 MB file you'll need to provide.</p>
          <button class="drop-zone" data-action="pick-rom" role="button" tabindex="0">
            <div class="drop-icon">📁</div>
            <div class="drop-text">
              ${wizardState.romPath
                ? `<span class="file-chosen">${escapeHtml(fileName(wizardState.romPath))}</span>`
                : "Click to browse for your ROM file"
              }
            </div>
            ${wizardState.romStatus === "verified" ? `<div class="rom-badge verified">✓ ${escapeHtml(wizardState.romName)} — compatible</div>` : ""}
            ${wizardState.romStatus === "accepted" ? '<div class="rom-badge accepted">⚠ ROM file accepted (unverified)</div>' : ""}
            ${wizardState.romStatus === "error" ? '<div class="rom-badge error">✕ Invalid ROM — must be exactly 4 MB</div>' : ""}
          </button>
          <details class="help-disclosure">
            <summary>Where do I find a ROM file?</summary>
            <p>ROM files can be extracted from a real Power Macintosh, or found through retro computing communities:</p>
            <ul>
              <li>E-Maculation Setup Guide (emaculation.com)</li>
              <li>E-Maculation Forum</li>
              <li>68k MLA Community (68kmla.org)</li>
            </ul>
          </details>
          <div class="wizard-nav">
            <button class="btn btn-secondary" data-action="wizard-back">Back</button>
            <button class="btn btn-primary" data-action="wizard-next" ${!wizardState.romPath ? "disabled" : ""}>Next</button>
          </div>
        </div>
      `;
      break;

    case 2:
      content = `
        <div class="wizard-section">
          <h2>Disk Image</h2>
          <div class="disk-mode-toggle">
            <button class="btn ${wizardState.diskMode === "create" ? "btn-primary" : "btn-secondary"}"
                    data-action="disk-mode" data-mode="create">Create New Disk</button>
            <button class="btn ${wizardState.diskMode === "existing" ? "btn-primary" : "btn-secondary"}"
                    data-action="disk-mode" data-mode="existing">Use Existing</button>
          </div>
          ${wizardState.diskMode === "create" ? `
            <div class="form-group">
              <label>Disk Size</label>
              <div class="size-picker">
                ${[0.5, 1, 2, 4]
                  .map(
                    (s) => `
                  <button class="size-btn ${wizardState.diskSizeGb === s ? "active" : ""}"
                          data-action="disk-size" data-size="${s}">${s < 1 ? "500 MB" : s + " GB"}</button>
                `
                  )
                  .join("")}
              </div>
            </div>
          ` : `
            <button class="drop-zone" data-action="pick-disk" role="button" tabindex="0">
              <div class="drop-icon">💾</div>
              <div class="drop-text">
                ${wizardState.diskPath
                  ? `<span class="file-chosen">${escapeHtml(fileName(wizardState.diskPath))}</span>`
                  : "Click to browse for a disk image"
                }
              </div>
            </button>
          `}
          <div class="form-group">
            <label>Mac OS Install CD <span class="optional">(optional)</span></label>
            <button class="drop-zone small" data-action="pick-cd" role="button" tabindex="0">
              <div class="drop-text">
                ${wizardState.cdPath
                  ? `<span class="file-chosen">${escapeHtml(fileName(wizardState.cdPath))}</span>`
                  : "Click to attach a CD image (ISO/toast)"
                }
              </div>
            </button>
          </div>
          <div class="wizard-nav">
            <button class="btn btn-secondary" data-action="wizard-back">Back</button>
            <button class="btn btn-primary" data-action="wizard-next">Next</button>
          </div>
        </div>
      `;
      break;

    case 3:
      content = `
        <div class="wizard-section">
          <h2>Review & Boot</h2>
          <div class="review-card">
            <div class="form-group">
              <label>VM Name</label>
              <input type="text" class="input" id="vm-name" value="${escapeAttr(wizardState.vmName)}" />
            </div>
            <div class="review-grid">
              <div class="review-item">
                <span class="review-label">ROM</span>
                <span class="review-value">${escapeHtml(fileName(wizardState.romPath) || "—")}</span>
              </div>
              <div class="review-item">
                <span class="review-label">Disk</span>
                <span class="review-value">${
                  wizardState.diskMode === "create"
                    ? `New ${wizardState.diskSizeGb < 1 ? "500 MB" : wizardState.diskSizeGb + " GB"} disk`
                    : escapeHtml(fileName(wizardState.diskPath) || "—")
                }</span>
              </div>
              <div class="review-item">
                <span class="review-label">RAM</span>
                <span class="review-value">
                  <select class="input-inline" id="ram-select">
                    ${[64, 128, 256, 512]
                      .map((m) => `<option value="${m}" ${wizardState.ramMb === m ? "selected" : ""}>${m} MB</option>`)
                      .join("")}
                  </select>
                </span>
              </div>
              <div class="review-item">
                <span class="review-label">Display</span>
                <span class="review-value">Windowed 1024×768</span>
              </div>
              <div class="review-item">
                <span class="review-label">Network</span>
                <span class="review-value">slirp (NAT)</span>
              </div>
              ${wizardState.cdPath ? `
              <div class="review-item">
                <span class="review-label">CD</span>
                <span class="review-value">${escapeHtml(fileName(wizardState.cdPath))}</span>
              </div>` : ""}
            </div>
          </div>
          <div class="wizard-nav">
            <button class="btn btn-secondary" data-action="wizard-back">Back</button>
            <button class="btn btn-primary btn-lg" data-action="wizard-create">Create & Start</button>
          </div>
        </div>
      `;
      break;
  }

  return `
    ${renderTitlebar()}
    <div class="wizard">
      <div class="wizard-progress">${progress}</div>
      <div class="wizard-content">${content}</div>
    </div>
  `;
}

let settingsSection = "general";
let pendingSettings: Record<string, string> = {};

function renderSettings(): string {
  const vm = vms.find((v) => v.id === selectedVmId);
  if (!vm) return renderLibrary();
  const isRunning = vm.id === runningVmId;

  const sections: Record<string, string> = {
    general: `
      <div class="form-group">
        <label>VM Name</label>
        <input type="text" class="input" id="setting-name" value="${escapeAttr(vm.name)}" />
      </div>
      <div class="form-group">
        <label>ROM File</label>
        <div class="file-input">
          <span class="file-path">${escapeHtml(vm.rom_path || "Not set")}</span>
          <button class="btn btn-secondary btn-sm" data-action="pick-setting-rom">Browse</button>
        </div>
      </div>
      <div class="form-group">
        <label>RAM <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-ram" ${isRunning ? "disabled" : ""}>
          ${[64, 128, 256, 512]
            .map((m) => `<option value="${m}" ${vm.ram_mb === m ? "selected" : ""}>${m} MB</option>`)
            .join("")}
        </select>
      </div>
    `,
    display: `
      <div class="form-group">
        <label>Window Size <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-screen" ${isRunning ? "disabled" : ""}>
          ${["win/640/480", "win/800/600", "win/1024/768", "win/1280/1024"]
            .map((s) => `<option value="${s}" ${vm.screen === s ? "selected" : ""}>${s.replace("win/", "").replace("/", "×")}</option>`)
            .join("")}
        </select>
      </div>
    `,
    storage: `
      <div class="form-group">
        <label>Disk Images</label>
        ${vm.disk_paths.length === 0
          ? '<p class="text-muted">No disks attached.</p>'
          : vm.disk_paths.map((d) => `
            <div class="file-input" style="margin-bottom: 8px;">
              <span class="file-path">${escapeHtml(d)}</span>
            </div>
          `).join("")
        }
        <button class="btn btn-secondary btn-sm" data-action="pick-setting-disk" style="margin-top: 8px;">+ Add Disk</button>
      </div>
      <div class="form-group">
        <label>CD-ROM</label>
        <div class="file-input">
          <span class="file-path">${escapeHtml(vm.cd_path || "None")}</span>
          <button class="btn btn-secondary btn-sm" data-action="pick-setting-cd">Browse</button>
        </div>
      </div>
    `,
    network: `
      <div class="form-group">
        <label>Networking</label>
        <select class="input" id="setting-ether">
          <option value="slirp" selected>slirp (NAT — outbound only)</option>
          <option value="">None</option>
        </select>
        <p class="text-muted" style="margin-top: 8px;">In the guest, open TCP/IP in Control Panels and set Configure to "Using DHCP Server".</p>
      </div>
    `,
    advanced: `
      <div class="form-group">
        <label>Sound</label>
        <select class="input" id="setting-nosound">
          <option value="true">Disabled</option>
          <option value="false">Enabled</option>
        </select>
      </div>
      <div class="form-group">
        <label>JIT Cache Size <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-jitcache" ${isRunning ? "disabled" : ""}>
          <option value="64M">64 MB</option>
          <option value="128M">128 MB</option>
          <option value="256M" selected>256 MB</option>
          <option value="512M">512 MB</option>
        </select>
      </div>
    `,
  };

  return `
    ${renderTitlebar()}
    <div class="settings">
      <div class="settings-header">
        <button class="btn btn-secondary" data-action="back-to-library">← Back</button>
        <h2>${escapeHtml(vm.name)}</h2>
        <div style="flex:1"></div>
        <button class="btn btn-primary" data-action="save-settings">Save</button>
      </div>
      <div class="settings-body">
        <div class="settings-sidebar">
          ${Object.keys(sections).map((s) => `
            <button class="settings-nav-item ${s === settingsSection ? "active" : ""}"
                    data-action="switch-section" data-section="${s}">
              ${s.charAt(0).toUpperCase() + s.slice(1)}
            </button>
          `).join("")}
        </div>
        <div class="settings-content">
          ${sections[settingsSection] || ""}
        </div>
      </div>
    </div>
  `;
}

function render() {
  const app = document.getElementById("app")!;
  switch (currentView) {
    case "library":
      app.innerHTML = renderLibrary();
      break;
    case "wizard":
      app.innerHTML = renderWizardStep();
      break;
    case "settings":
      app.innerHTML = renderSettings();
      break;
  }
  bindEvents();
}

function bindEvents() {
  document.querySelectorAll("[data-action]").forEach((el) => {
    el.addEventListener("click", handleAction);
  });
}

async function pickFile(
  title: string,
  filters?: { name: string; extensions: string[] }[]
): Promise<string | null> {
  const result = await open({ title, filters, multiple: false, directory: false });
  if (typeof result === "string") return result;
  if (result && "path" in result) return (result as { path: string }).path;
  return null;
}

function captureCurrentSectionSettings() {
  const fields: [string, string, (v: string) => string][] = [
    ["setting-name", "name", (v) => v],
    ["setting-ram", "ramsize", (v) => v + "M"],
    ["setting-screen", "screen", (v) => v],
    ["setting-ether", "ether", (v) => v],
    ["setting-nosound", "nosound", (v) => v],
    ["setting-jitcache", "jitcachesize", (v) => v],
  ];
  for (const [elId, key, transform] of fields) {
    const el = document.getElementById(elId) as HTMLInputElement | HTMLSelectElement | null;
    if (el) {
      pendingSettings[key] = transform(el.value);
    }
  }
}

async function handleAction(e: Event) {
  const target = (e.target as HTMLElement).closest("[data-action]") as HTMLElement;
  if (!target) return;

  const action = target.dataset.action;
  const id = target.dataset.id;

  switch (action) {
    case "wizard":
      currentView = "wizard";
      wizardStep = 0;
      wizardState = {
        romPath: "",
        romStatus: "",
        romName: "",
        diskPath: "",
        diskMode: "create",
        diskSizeGb: 2,
        cdPath: "",
        vmName: "My Mac",
        ramMb: 256,
        screen: "win/1024/768",
      };
      render();
      break;

    case "wizard-next":
      if (wizardStep < 3) {
        wizardStep++;
        render();
      }
      break;

    case "wizard-back":
      if (wizardStep > 0) {
        wizardStep--;
        render();
      } else {
        currentView = "library";
        render();
      }
      break;

    case "pick-rom": {
      const path = await pickFile("Select ROM File");
      if (path) {
        wizardState.romPath = path;
        try {
          const info = (await invoke("verify_rom", { path })) as RomInfo;
          wizardState.romStatus = info.status as "" | "verified" | "accepted" | "error";
          wizardState.romName = info.name;
        } catch {
          wizardState.romStatus = "accepted";
        }
        render();
      }
      break;
    }

    case "pick-disk": {
      const path = await pickFile("Select Disk Image", [
        { name: "Disk Images", extensions: ["dsk", "img", "hfv"] },
      ]);
      if (path) {
        wizardState.diskPath = path;
        render();
      }
      break;
    }

    case "pick-cd": {
      const path = await pickFile("Select CD Image", [
        { name: "CD Images", extensions: ["iso", "toast", "cdr", "dmg"] },
      ]);
      if (path) {
        wizardState.cdPath = path;
        render();
      }
      break;
    }

    case "disk-mode":
      wizardState.diskMode = target.dataset.mode as "create" | "existing";
      render();
      break;

    case "disk-size":
      wizardState.diskSizeGb = parseFloat(target.dataset.size || "2");
      render();
      break;

    case "wizard-create": {
      const nameInput = document.getElementById("vm-name") as HTMLInputElement;
      const ramSelect = document.getElementById("ram-select") as HTMLSelectElement;
      if (nameInput) wizardState.vmName = nameInput.value;
      if (ramSelect) wizardState.ramMb = parseInt(ramSelect.value);

      try {
        const vm = (await invoke("create_vm", {
          request: {
            name: wizardState.vmName,
            romPath: wizardState.romPath,
            ramMb: wizardState.ramMb,
            diskMode: wizardState.diskMode,
            diskSizeGb: wizardState.diskSizeGb,
            diskPath: wizardState.diskPath,
            cdPath: wizardState.cdPath,
            screen: wizardState.screen,
          },
        })) as VmProfile;
        vms = await loadVms();

        try {
          await invoke("launch_vm", { id: vm.id });
          runningVmId = vm.id;
        } catch (err) {
          console.error("Created VM but failed to launch:", err);
        }

        currentView = "library";
        render();
      } catch (err) {
        console.error("Failed to create VM:", err);
        alert(`Failed to create VM: ${err}`);
      }
      break;
    }

    case "settings":
      if (id) {
        selectedVmId = id;
        settingsSection = "general";
        pendingSettings = {};
        currentView = "settings";
        render();
      }
      break;

    case "back-to-library":
      currentView = "library";
      render();
      break;

    case "launch":
      if (id) {
        try {
          await invoke("launch_vm", { id });
          runningVmId = id;
          showToast("Virtual machine started. Click inside the classic desktop to capture the mouse. Press Ctrl-F5 to release.", "info", 8000);
          render();
        } catch (err) {
          showToast(`Failed to launch: ${err}`, "error");
        }
      }
      break;

    case "stop":
      try {
        await invoke("stop_vm");
        runningVmId = null;
        render();
      } catch (err) {
        console.error("Failed to stop VM:", err);
      }
      break;

    case "duplicate":
      if (id) {
        const vmName = target.dataset.name || "Copy";
        try {
          await invoke("duplicate_vm", { id, newName: `${vmName} (Copy)` });
          vms = await loadVms();
          showToast("VM duplicated", "success");
          render();
        } catch (err) {
          showToast(`Failed to duplicate: ${err}`, "error");
        }
      }
      break;

    case "delete":
      if (id && confirm("Remove this virtual machine and its files?")) {
        try {
          await invoke("delete_vm", { id });
          vms = await loadVms();
          render();
        } catch (err) {
          console.error("Failed to delete VM:", err);
        }
      }
      break;

    case "switch-section":
      captureCurrentSectionSettings();
      settingsSection = target.dataset.section || "general";
      render();
      break;

    case "save-settings":
      if (selectedVmId) {
        captureCurrentSectionSettings();
        try {
          for (const [key, value] of Object.entries(pendingSettings)) {
            await invoke("update_vm_setting", { id: selectedVmId, key, value });
          }
          pendingSettings = {};
          vms = await loadVms();
          showToast("Settings saved", "success");
          render();
        } catch (err) {
          showToast(`Failed to save: ${err}`, "error");
        }
      }
      break;

    case "pick-setting-rom": {
      const path = await pickFile("Select ROM File");
      if (path && selectedVmId) {
        await invoke("update_vm_setting", { id: selectedVmId, key: "rom", value: path });
        vms = await loadVms();
        render();
      }
      break;
    }

    case "pick-setting-disk": {
      const path = await pickFile("Select Disk Image", [
        { name: "Disk Images", extensions: ["dsk", "img", "hfv"] },
      ]);
      if (path && selectedVmId) {
        const vm_dir = (await invoke("get_vm", { id: selectedVmId }) as VmProfile);
        const currentDisks = vm_dir.disk_paths;
        if (!currentDisks.includes(path)) {
          // For now, add via a direct prefs append — TODO: proper multi-value support
          console.log("Would add disk:", path);
        }
        render();
      }
      break;
    }

    case "pick-setting-cd": {
      const path = await pickFile("Select CD Image", [
        { name: "CD Images", extensions: ["iso", "toast", "cdr", "dmg"] },
      ]);
      if (path && selectedVmId) {
        await invoke("update_vm_setting", { id: selectedVmId, key: "cdrom", value: path });
        vms = await loadVms();
        render();
      }
      break;
    }
  }
}

// Error banner state
let errorBanner: string | null = null;

// Toast notifications
let toasts: { id: number; message: string; type: "info" | "success" | "error" }[] = [];
let toastCounter = 0;

function showToast(message: string, type: "info" | "success" | "error" = "info", durationMs = 5000) {
  const id = ++toastCounter;
  toasts.push({ id, message, type });
  renderToasts();
  setTimeout(() => {
    toasts = toasts.filter((t) => t.id !== id);
    renderToasts();
  }, durationMs);
}

function renderToasts() {
  let container = document.getElementById("toast-container");
  if (!container) {
    container = document.createElement("div");
    container.id = "toast-container";
    document.body.appendChild(container);
  }
  container.innerHTML = toasts
    .map((t) => `<div class="toast toast-${t.type}">${escapeHtml(t.message)}</div>`)
    .join("");
}

let statusPollInterval: ReturnType<typeof setInterval> | null = null;

async function pollRunningStatus() {
  const newRunningId = await checkRunning();
  if (newRunningId !== runningVmId) {
    runningVmId = newRunningId;
    if (currentView === "library") render();
  }
}

async function init() {
  vms = await loadVms();
  runningVmId = await checkRunning();

  try {
    const status = (await invoke("check_emulator_status")) as { found: boolean; path: string };
    if (!status.found) {
      errorBanner = "SheepShaver binary not found. Build it first: cd SheepShaver && make build-ss";
    }
  } catch {
    // Not fatal — may be running in browser preview
  }

  render();
  statusPollInterval = setInterval(pollRunningStatus, 2000);
}

init();
