import { invoke } from "@tauri-apps/api/core";
import { listen, emit } from "@tauri-apps/api/event";
import { open } from "@tauri-apps/plugin-dialog";
import { WebviewWindow } from "@tauri-apps/api/webviewWindow";
import { getCurrentWindow } from "@tauri-apps/api/window";

import iconDisplayOn from "./icons/display_on.png";
import iconDisplayOff from "./icons/display_off.png";
import iconToolbox from "./icons/toolbox.png";
import iconTrash from "./icons/trash.png";
import iconFolder from "./icons/folder_apple.png";

// App preference: show text labels next to icons (stored in localStorage)
let showIconLabels = localStorage.getItem("ss-icon-labels") !== "false";

function toggleIconLabels() {
  showIconLabels = !showIconLabels;
  localStorage.setItem("ss-icon-labels", String(showIconLabels));
  render();
}

const icon = (src: string, alt: string, size = 18) =>
  `<img src="${src}" alt="${alt}" width="${size}" height="${size}" style="image-rendering: pixelated; vertical-align: middle;" />`;

const iconLabel = (src: string, alt: string, label: string, size = 18) =>
  showIconLabels
    ? `${icon(src, alt, size)} <span class="icon-label">${label}</span>`
    : icon(src, alt, size);

// Placeholders for icons we haven't sourced yet (TODO: find pixel art versions)
const pLabel = (sym: string, label: string) => showIconLabels ? `${sym} <span class="icon-label">${label}</span>` : sym;

const ICON_PLUS = () => pLabel("＋", "New");
const ICON_IMPORT = () => pLabel("⤓", "Import");
const ICON_HELP = () => pLabel("?", "Help");
const ICON_DUPLICATE = () => pLabel("⎘", "Duplicate");
const ICON_SETTINGS = () => iconLabel(iconToolbox, "Configure", "Configure");
const ICON_POWER_ON = () => iconLabel(iconDisplayOn, "Running", "Shut Down");
const ICON_POWER_OFF = () => iconLabel(iconDisplayOff, "Start", "Start");
const ICON_TRASH = () => iconLabel(iconTrash, "Delete", "Delete");
const ICON_FOLDER = () => iconLabel(iconFolder, "Reveal", "Finder");

interface VmProfile {
  id: string;
  name: string;
  rom_path: string;
  ram_mb: number;
  disk_paths: string[];
  cd_path: string;
  screen: string;
  shared_disk_warning?: string | null;
  os_version?: string | null;
  last_booted?: string | null;
}

const vmScreenshots: Map<string, string> = new Map();

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
let runningVmIds: Set<string> = new Set();
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

async function checkRunning(): Promise<Set<string>> {
  try {
    const ids = (await invoke("get_running_vms")) as string[];
    return new Set(ids);
  } catch {
    return new Set();
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
  return "";
}

function formatLastBooted(ts: string | null | undefined): string {
  if (!ts) return "";
  const secs = parseInt(ts);
  if (isNaN(secs)) return "";
  const d = new Date(secs * 1000);
  return d.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" });
}

function formatElapsed(launchTs: number): string {
  const secs = Math.floor((Date.now() - launchTs) / 1000);
  if (secs < 60) return `${secs}s`;
  const mins = Math.floor(secs / 60);
  const remSecs = secs % 60;
  if (mins < 60) return `${mins}m ${remSecs}s`;
  return `${Math.floor(mins / 60)}h ${mins % 60}m`;
}

function renderVmRow(vm: VmProfile): string {
  const isRunning = runningVmIds.has(vm.id);
  const isSelected = vm.id === selectedVmId;
  const screenshotSrc = vmScreenshots.get(vm.id);
  const osLabel = vm.os_version ? escapeHtml(vm.os_version) : `${vm.ram_mb} MB`;
  const launchTs = vmLaunchTimestamps.get(vm.id);
  const elapsed = isRunning && launchTs ? formatElapsed(launchTs) : "";
  return `
    <div class="vm-row ${isRunning ? "vm-row--running" : ""} ${isSelected ? "vm-row--selected" : ""}"
         data-id="${escapeAttr(vm.id)}"
         data-name="${escapeAttr(vm.name)}"
         data-action="select-vm">
      <div class="vm-row__thumb">
        ${screenshotSrc
          ? `<img src="${screenshotSrc}" alt="" class="vm-row__thumb-img" />`
          : `<span class="vm-row__thumb-placeholder">🖥</span>`
        }
        ${isRunning ? '<span class="vm-row__live-dot"></span>' : ""}
      </div>
      <div class="vm-row__info">
        <span class="vm-row__name">${escapeHtml(vm.name)}</span>
        <span class="vm-row__meta">${osLabel}${elapsed ? ` · ${elapsed}` : ""}</span>
      </div>
      <div class="vm-row__actions">
        ${isRunning
          ? `<button class="vm-row__btn vm-row__btn--power-on" data-action="stop" data-id="${escapeAttr(vm.id)}" title="Shut Down">${ICON_POWER_ON()}</button>`
          : `<button class="vm-row__btn" data-action="launch" data-id="${escapeAttr(vm.id)}" title="Start">${ICON_POWER_OFF()}</button>`
        }
        <span class="vm-row__sep"></span>
        <button class="vm-row__btn" data-action="settings" data-id="${escapeAttr(vm.id)}" title="Configure">${ICON_SETTINGS()}</button>
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
      <div class="community-links">
        <p class="ss-text-muted" style="margin-top: 32px;">Find software and resources:</p>
        <div style="display: flex; gap: 12px; flex-wrap: wrap; justify-content: center; margin-top: 8px;">
          <a href="https://infinitemac.org" target="_blank" class="community-link">Infinite Mac — try classic Mac OS in your browser</a>
          <a href="https://macintoshgarden.org" target="_blank" class="community-link">Macintosh Garden — classic Mac software archive</a>
          <a href="https://www.emaculation.com/forum/" target="_blank" class="community-link">E-Maculation — emulation community</a>
          <a href="https://68kmla.org" target="_blank" class="community-link">68k MLA — vintage Mac community</a>
        </div>
      </div>
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

  // Auto-select first VM if none selected (prefs loaded by ensurePrefsLoaded)
  if (!selectedVmId || !vms.find((v) => v.id === selectedVmId)) {
    selectedVmId = vms[0].id;
  }
  ensurePrefsLoaded();

  return `
    ${renderTitlebar()}
    ${renderErrorBanner()}
    <div class="cc-master-detail">
      <div class="cc-sidebar">
        <div class="cc-list">
          ${vms.map(renderVmRow).join("")}
        </div>
        <div class="cc-footer">
          <button class="vm-row__btn" data-action="show-help" title="Help & Resources">${ICON_HELP()}</button>
          <button class="vm-row__btn" data-action="toggle-labels" title="Toggle icon labels">Aa</button>
          <div style="flex:1"></div>
          <button class="vm-row__btn" data-action="import-prefs" title="Import Setup">${ICON_IMPORT()}</button>
          <button class="vm-row__btn" data-action="wizard" title="New VM">${ICON_PLUS()}</button>
        </div>
      </div>
      <div class="cc-detail">
        ${selectedVmId ? renderDetailPane() : '<div class="cc-detail__empty">Select a VM</div>'}
      </div>
    </div>
  `;
}

function renderDetailPane(): string {
  const vm = vms.find((v) => v.id === selectedVmId);
  if (!vm) return '<div class="cc-detail__empty">Select a VM</div>';
  const isRunning = runningVmIds.has(vm.id);
  const screenshotSrc = vmScreenshots.get(vm.id);
  const launchTs = vmLaunchTimestamps.get(vm.id);
  const elapsed = isRunning && launchTs ? formatElapsed(launchTs) : "";

  return `
    <div class="detail-header">
      <div class="detail-header__info">
        <h2>${escapeHtml(vm.name)}</h2>
        <span class="detail-header__meta">${vm.os_version ? escapeHtml(vm.os_version) + " · " : ""}${vm.ram_mb} MB RAM${elapsed ? " · " + elapsed : ""}</span>
      </div>
      <div class="detail-header__actions">
        ${isRunning
          ? `<button class="btn btn-secondary" data-action="stop" data-id="${escapeAttr(vm.id)}">${ICON_POWER_ON()} </button>`
          : `<button class="btn btn-primary" data-action="launch" data-id="${escapeAttr(vm.id)}">${ICON_POWER_OFF()} </button>`
        }
        <button class="btn btn-secondary" data-action="duplicate" data-id="${escapeAttr(vm.id)}" data-name="${escapeAttr(vm.name)}">${ICON_DUPLICATE()}</button>
        <button class="btn btn-secondary" data-action="reveal" data-id="${escapeAttr(vm.id)}">${ICON_FOLDER()}</button>
        <button class="btn btn-secondary btn-danger-hover" data-action="delete" data-id="${escapeAttr(vm.id)}">${ICON_TRASH()}</button>
      </div>
    </div>
    ${isRunning ? '<div class="settings-running-banner">VM is running. Hardware settings apply on next restart.</div>' : ""}
    <div class="detail-screenshot">
      ${screenshotSrc
        ? `<img src="${screenshotSrc}" alt="VM screenshot" class="detail-screenshot__img" />`
        : `<div class="detail-screenshot__placeholder">🖥 ${isRunning ? "Capturing..." : "No screenshot yet"}</div>`
      }
    </div>
    <div class="detail-config">
      <div class="detail-tabs">
        ${["general", "display", "storage", "network", "input", "advanced", "debug"]
          .map((s) => `<button class="detail-tab ${s === settingsSection ? "detail-tab--active" : ""}"
                        data-action="switch-section" data-section="${s}">${s.charAt(0).toUpperCase() + s.slice(1)}</button>`)
          .join("")}
      </div>
      <div class="detail-tab-content">
        ${renderSettingsSection(vm, isRunning)}
      </div>
      <div class="detail-save-bar">
        <button class="btn btn-primary" data-action="save-settings">Save</button>
      </div>
    </div>
  `;
}

function renderSettingsSection(vm: VmProfile, isRunning: boolean): string {
  // Delegates to renderSettings' sections object — shared between
  // the integrated detail pane and the separate settings window
  return renderSettingsSectionContent(vm, isRunning, settingsSection);
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
          <h2>SiliconSheep</h2>
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
            ${wizardState.romStatus === "error" ? '<div class="rom-badge error">⚠ File doesn\'t look like a Mac ROM (unexpected size). <button class="btn-link" data-action="force-accept-rom">Proceed anyway</button></div>' : ""}
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
            <button class="btn btn-secondary" data-action="wizard-cancel">Cancel</button> <button class="btn btn-secondary" data-action="wizard-back">Back</button>
            <button class="btn btn-primary" data-action="wizard-next" ${!wizardState.romPath ? "disabled" : ""}>${wizardState.romStatus === "error" ? "Next (proceed anyway)" : "Next"}</button>
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
            <button class="btn btn-secondary" data-action="wizard-cancel">Cancel</button> <button class="btn btn-secondary" data-action="wizard-back">Back</button>
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
                <span class="review-value">Internet (NAT)</span>
              </div>
              ${wizardState.cdPath ? `
              <div class="review-item">
                <span class="review-label">CD</span>
                <span class="review-value">${escapeHtml(fileName(wizardState.cdPath))}</span>
              </div>` : ""}
            </div>
          </div>
          <div class="wizard-nav">
            <button class="btn btn-secondary" data-action="wizard-cancel">Cancel</button> <button class="btn btn-secondary" data-action="wizard-back">Back</button>
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
let debugEnvVars: Record<string, string> = {};
let settingsWindowOpen = false;
let isSettingsWindow = false;
const vmLaunchTimestamps: Map<string, number> = new Map();

async function openSettingsWindow(vmId: string) {
  // Check by window label — survives Control Center close/reopen
  try {
    const existing = await WebviewWindow.getByLabel("settings");
    if (existing) {
      await existing.setFocus();
      return;
    }
  } catch {
    // Window doesn't exist — proceed to create
  }

  const vm = vms.find((v) => v.id === vmId);
  const title = vm ? `${vm.name} Configuration` : "VM Configuration";

  try {
    const win = new WebviewWindow("settings", {
      url: `index.html?settings=${encodeURIComponent(vmId)}`,
      title,
      width: 750,
      height: 600,
      resizable: true,
      minWidth: 600,
      minHeight: 400,
    });

    settingsWindowOpen = true;

    win.once("tauri://destroyed", () => {
      settingsWindowOpen = false;
    });
  } catch (e) {
    settingsWindowOpen = false;
    showToast(`Failed to open settings: ${e}`, "error");
  }
}

interface PrefEntry {
  key: string;
  value: string;
  comment: string | null;
}

let vmPrefs: PrefEntry[] = [];

async function loadVmPrefs(id: string) {
  try {
    vmPrefs = (await invoke("get_vm_prefs", { id })) as PrefEntry[];
    vmPrefsLoadedFor = id;
  } catch {
    vmPrefs = [];
    vmPrefsLoadedFor = null;
  }
}

function getPref(key: string): string {
  // Pending (unsaved) edits take priority over on-disk prefs
  if (key in pendingSettings) return pendingSettings[key];
  const entry = vmPrefs.find((e) => e.key === key);
  return entry?.value ?? "";
}

let vmPrefsLoadedFor: string | null = null;

function ensurePrefsLoaded() {
  if (selectedVmId && selectedVmId !== vmPrefsLoadedFor) {
    const targetId = selectedVmId;
    loadVmPrefs(targetId).then(() => {
      // Only apply if the selection hasn't changed during the async load (fixes race: bug 3)
      if (selectedVmId === targetId) {
        vmPrefsLoadedFor = targetId;
        render();
      }
    });
  }
}

function getPrefs(key: string): string[] {
  return vmPrefs.filter((e) => e.key === key).map((e) => e.value);
}

function renderSettingsSectionContent(vm: VmProfile, isRunning: boolean, section: string): string {
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
    display: (() => {
      const currentScreen = getPref("screen") || vm.screen;
      const presets = ["win/640/480", "win/800/600", "win/1024/768", "win/1280/1024", "win/1600/1200", "win/1920/1080"];
      const isCustom = !presets.includes(currentScreen);
      const currentW = currentScreen.split("/")[1] || "800";
      const currentH = currentScreen.split("/")[2] || "600";
      const currentFrameskip = getPref("frameskip") || "1";
      return `
      <div class="form-group">
        <label>Window Size <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-screen-preset" ${isRunning ? "disabled" : ""}>
          ${presets
            .map((s) => `<option value="${s}" ${currentScreen === s ? "selected" : ""}>${s.replace("win/", "").replace("/", "×")}</option>`)
            .join("")}
          <option value="custom" ${isCustom ? "selected" : ""}>Custom...</option>
        </select>
      </div>
      <div class="form-group" id="custom-res-group" style="${isCustom ? "" : "display:none"}">
        <label>Custom Resolution</label>
        <div style="display: flex; gap: 8px; align-items: center;">
          <input type="number" class="input" id="setting-screen-w" value="${escapeAttr(currentW)}"
                 min="320" max="3840" style="width: 100px;" ${isRunning ? "disabled" : ""} />
          <span>×</span>
          <input type="number" class="input" id="setting-screen-h" value="${escapeAttr(currentH)}"
                 min="240" max="2160" style="width: 100px;" ${isRunning ? "disabled" : ""} />
        </div>
        <p class="ss-text-muted">Any size works. Classic Mac OS apps assume 72 dpi; 1024×768 or 1280×1024 are the sweet spot. Very large resolutions may be slow.</p>
      </div>
      <div class="form-group">
        <label>Refresh Rate <span class="hot-reload-badge instant">Applies instantly</span></label>
        <select class="input" id="setting-frameskip">
          ${[
            { v: "0", label: "Maximum (60 fps, every tick)" },
            { v: "1", label: "Every frame (60 fps)" },
            { v: "2", label: "Every 2nd frame (30 fps)" },
            { v: "4", label: "Every 4th frame (15 fps)" },
            { v: "8", label: "Every 8th frame (8 fps)" },
            { v: "12", label: "Every 12th frame (5 fps)" },
          ].map((o) => `<option value="${o.v}" ${currentFrameskip === o.v ? "selected" : ""}>${o.label}</option>`)
           .join("")}
        </select>
        <p class="ss-text-muted">The emulator ticks at 60 Hz internally. frameskip=0 and frameskip=1 both render every tick (60 fps). Higher values skip frames to reduce CPU load. The guest runs at full speed regardless — this only affects display updates.</p>
      </div>
      <div class="form-group">
        <label>QuickDraw Acceleration</label>
        <select class="input" id="setting-gfxaccel">
          <option value="true" ${getPref("gfxaccel") !== "false" ? "selected" : ""}>Enabled (recommended)</option>
          <option value="false" ${getPref("gfxaccel") === "false" ? "selected" : ""}>Disabled</option>
        </select>
      </div>`;
    })(),
    storage: (() => {
      const disks = getPrefs("disk");
      const cdroms = getPrefs("cdrom");
      return `
      <div class="form-group">
        <label>Disk Images</label>
        ${disks.length === 0
          ? '<p class="ss-text-muted">No disks attached.</p>'
          : disks.map((d) => `
            <div class="file-input" style="margin-bottom: 8px;">
              <span class="file-path">${escapeHtml(d)}</span>
            </div>
          `).join("")
        }
        <div style="display: flex; gap: 8px; margin-top: 8px;">
          <button class="btn btn-secondary btn-sm" data-action="pick-setting-disk">+ Add Disk</button>
          <button class="btn btn-secondary btn-sm" data-action="backup-disk" ${isRunning ? "disabled" : ""}>Backup Disks</button>
        </div>
      </div>
      <div class="form-group">
        <label>CD-ROM</label>
        ${cdroms.length === 0
          ? `<div class="file-input">
              <span class="file-path">None</span>
              <button class="btn btn-secondary btn-sm" data-action="pick-setting-cd">Browse</button>
            </div>`
          : cdroms.map((c) => `
            <div class="file-input" style="margin-bottom: 8px;">
              <span class="file-path">${escapeHtml(c)}</span>
            </div>
          `).join("") + `
            <button class="btn btn-secondary btn-sm" data-action="pick-setting-cd" style="margin-top: 8px;">+ Add CD</button>`
        }
      </div>
      <div class="form-group">
        <label>No CD-ROM Drive</label>
        <select class="input" id="setting-nocdrom">
          <option value="false" ${getPref("nocdrom") !== "true" ? "selected" : ""}>CD drive enabled</option>
          <option value="true" ${getPref("nocdrom") === "true" ? "selected" : ""}>CD drive disabled</option>
        </select>
      </div>
    `;
    })(),
    network: `
      <div class="form-group">
        <label>Networking</label>
        <select class="input" id="setting-ether">
          <option value="slirp" ${getPref("ether") === "slirp" ? "selected" : ""}>Internet (NAT)</option>
          <option value="" ${!getPref("ether") ? "selected" : ""}>None</option>
        </select>
        <p class="ss-text-muted" style="margin-top: 8px;">In the guest, open TCP/IP in Control Panels and set Configure to "Using DHCP Server".</p>
      </div>
      <div class="form-group">
        <label>VNC Server</label>
        <select class="input" id="setting-vncserver">
          <option value="true" ${getPref("vncserver") === "true" ? "selected" : ""}>Enabled</option>
          <option value="false" ${getPref("vncserver") !== "true" ? "selected" : ""}>Disabled</option>
        </select>
      </div>
      <div class="form-group">
        <label>VNC Port</label>
        <input type="number" class="input" id="setting-vncport" value="${escapeAttr(getPref("vncport") || "5900")}" />
      </div>
    `,
    input: `
      <div class="form-group">
        <label>Mouse Wheel Mode</label>
        <select class="input" id="setting-mousewheelmode">
          <option value="0" ${getPref("mousewheelmode") !== "1" ? "selected" : ""}>Page Up/Down</option>
          <option value="1" ${getPref("mousewheelmode") === "1" ? "selected" : ""}>Cursor Up/Down</option>
        </select>
      </div>
      <div class="form-group">
        <label>Mouse Wheel Lines</label>
        <input type="number" class="input" id="setting-mousewheellines" value="${escapeAttr(getPref("mousewheellines") || "3")}" min="1" max="20" />
      </div>
      <div class="form-group">
        <label>Swap Option/Command Keys</label>
        <select class="input" id="setting-swap_opt_cmd">
          <option value="false" ${getPref("swap_opt_cmd") !== "true" ? "selected" : ""}>No (Option=Option, Command=Command)</option>
          <option value="true" ${getPref("swap_opt_cmd") === "true" ? "selected" : ""}>Yes (swap Option ↔ Command)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Use Raw Keycodes</label>
        <select class="input" id="setting-keycodes">
          <option value="false" ${getPref("keycodes") !== "true" ? "selected" : ""}>No (use keysyms)</option>
          <option value="true" ${getPref("keycodes") === "true" ? "selected" : ""}>Yes (use keycodes)</option>
        </select>
      </div>
    `,
    advanced: `
      <div class="form-group">
        <label>Sound</label>
        <select class="input" id="setting-nosound">
          <option value="true" ${getPref("nosound") === "true" ? "selected" : ""}>Disabled</option>
          <option value="false" ${getPref("nosound") !== "true" ? "selected" : ""}>Enabled</option>
        </select>
      </div>
      <div class="form-group">
        <label>JIT Cache Size <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-jitcache" ${isRunning ? "disabled" : ""}>
          ${["64M", "128M", "256M", "512M"]
            .map((v) => `<option value="${v}" ${getPref("jitcachesize") === v ? "selected" : ""}>${v.replace("M", " MB")}</option>`)
            .join("")}
        </select>
      </div>
      <div class="form-group">
        <label>Shared Folder (ExtFS)</label>
        <div class="file-input">
          <span class="file-path">${escapeHtml(getPref("extfs") || "None")}</span>
          <button class="btn btn-secondary btn-sm" data-action="pick-setting-extfs">Browse</button>
        </div>
      </div>
      <div class="form-group">
        <label>Boot Driver <span class="hot-reload-badge restart">Requires restart</span></label>
        <select class="input" id="setting-bootdriver" ${isRunning ? "disabled" : ""}>
          <option value="0" ${getPref("bootdriver") !== "-62" ? "selected" : ""}>Hard Disk</option>
          <option value="-62" ${getPref("bootdriver") === "-62" ? "selected" : ""}>CD-ROM</option>
        </select>
      </div>
      <details class="expert-fold">
        <summary>Expert Settings</summary>
        <div class="expert-content">
          <div class="form-group">
            <label>Ignore Illegal Memory Accesses</label>
            <select class="input" id="setting-ignoresegv">
              <option value="true" ${getPref("ignoresegv") !== "false" ? "selected" : ""}>Yes (recommended)</option>
              <option value="false" ${getPref("ignoresegv") === "false" ? "selected" : ""}>No (crash on SEGV)</option>
            </select>
            <p class="ss-text-muted">Some Mac OS software accesses memory outside its allocated range. Ignoring these prevents crashes at the cost of possible subtle glitches.</p>
          </div>
          <div class="form-group">
            <label>Ignore Illegal Instructions</label>
            <select class="input" id="setting-ignoreillegal">
              <option value="true" ${getPref("ignoreillegal") !== "false" ? "selected" : ""}>Yes (recommended)</option>
              <option value="false" ${getPref("ignoreillegal") === "false" ? "selected" : ""}>No (crash on SIGILL)</option>
            </select>
            <p class="ss-text-muted">Some ROM code contains instructions not supported by the emulator. Ignoring them keeps the system running.</p>
          </div>
          <div class="form-group">
            <label>Idle Wait</label>
            <select class="input" id="setting-idlewait">
              <option value="true" ${getPref("idlewait") !== "false" ? "selected" : ""}>Yes (save CPU when idle)</option>
              <option value="false" ${getPref("idlewait") === "false" ? "selected" : ""}>No</option>
            </select>
            <p class="ss-text-muted">Sleeps the host CPU when the guest is idle, reducing power and heat. Disable if the guest freezes during idle periods.</p>
          </div>
          <div class="form-group">
            <label>JIT Compiler <span class="hot-reload-badge restart">Requires restart</span></label>
            <select class="input" id="setting-jit" ${isRunning ? "disabled" : ""}>
              <option value="true" ${getPref("jit") !== "false" ? "selected" : ""}>Enabled</option>
              <option value="false" ${getPref("jit") === "false" ? "selected" : ""}>Disabled (interpreter)</option>
            </select>
            <p class="ss-text-muted">The AArch64 JIT compiles PowerPC code to native ARM64 for ~2x performance. Disable to fall back to the slower interpreter for debugging.</p>
          </div>
          <div class="form-group">
            <label>68K DR Emulator</label>
            <select class="input" id="setting-jit68k">
              <option value="false" ${getPref("jit68k") !== "true" ? "selected" : ""}>Disabled</option>
              <option value="true" ${getPref("jit68k") === "true" ? "selected" : ""}>Enabled</option>
            </select>
            <p class="ss-text-muted">The Mac OS ROM contains 68K code run by a built-in emulator (the "DR emulator"). This legacy JIT for that layer is usually not needed on ARM64.</p>
          </div>
          <div class="form-group">
            <label>Clipboard Conversion</label>
            <select class="input" id="setting-noclipconversion">
              <option value="false" ${getPref("noclipconversion") !== "true" ? "selected" : ""}>Enabled (convert clipboard)</option>
              <option value="true" ${getPref("noclipconversion") === "true" ? "selected" : ""}>Disabled (raw clipboard)</option>
            </select>
            <p class="ss-text-muted">Converts text encoding (Mac Roman to Unicode) and image formats when copying between guest and host. Disable if clipboard sync causes issues.</p>
          </div>
          <div class="form-group">
            <label>Hardware Cursor</label>
            <select class="input" id="setting-hardcursor">
              <option value="false" ${getPref("hardcursor") !== "true" ? "selected" : ""}>Software cursor</option>
              <option value="true" ${getPref("hardcursor") === "true" ? "selected" : ""}>Hardware cursor</option>
            </select>
            <p class="ss-text-muted">Hardware cursor uses the host OS cursor for lower latency. Software cursor renders the classic Mac arrow inside the guest framebuffer.</p>
          </div>
          <div class="form-group">
            <label>Serial Port A</label>
            <input type="text" class="input" id="setting-seriala" value="${escapeAttr(getPref("seriala"))}" placeholder="e.g. /dev/tty.usbserial" />
            <p class="ss-text-muted">Maps to the guest's modem port. Use a host serial device path or leave empty.</p>
          </div>
          <div class="form-group">
            <label>Serial Port B</label>
            <input type="text" class="input" id="setting-serialb" value="${escapeAttr(getPref("serialb"))}" placeholder="e.g. /dev/tty.usbserial" />
            <p class="ss-text-muted">Maps to the guest's printer port.</p>
          </div>
          <div class="form-group">
            <label>Keyboard Type</label>
            <input type="number" class="input" id="setting-keyboardtype" value="${escapeAttr(getPref("keyboardtype") || "5")}" />
            <p class="ss-text-muted">The keyboard type reported to Mac OS. 5 = Apple Extended Keyboard II (default, works for most layouts).</p>
          </div>
          <div class="form-group">
            <label>SDL Renderer</label>
            <input type="text" class="input" id="setting-sdlrender" value="${escapeAttr(getPref("sdlrender") || "")}" placeholder="auto (default)" />
            <p class="ss-text-muted">Override the SDL rendering backend. "auto" lets SDL choose (usually Metal on macOS). Try "software" if you see display glitches.</p>
          </div>
        </div>
      </details>
    `,
    debug: `
      <p class="ss-text-muted" style="margin-bottom: 16px;">Set environment variables for the next launch. These control JIT diagnostics, verification, and tracing. Changes take effect on the next Start — they don't modify the prefs file.</p>
      <div class="form-group">
        <label>JIT Verify Mode</label>
        <select class="input" id="debug-SS_JIT_VERIFY">
          <option value="" selected>Off</option>
          <option value="1">On — differential interp-vs-JIT check (VERY SLOW)</option>
        </select>
        <p class="ss-text-muted">Runs every block in both interpreter and JIT, compares results. Catches codegen bugs. Expect ~100x slowdown.</p>
      </div>
      <div class="form-group">
        <label>Disable Block Chaining</label>
        <select class="input" id="debug-SS_JIT_NO_CHAIN">
          <option value="" selected>Off (chaining enabled)</option>
          <option value="1">On — disable block chaining</option>
        </select>
        <p class="ss-text-muted">Forces every block to return to the dispatcher. Useful for isolating chaining-related bugs.</p>
      </div>
      <div class="form-group">
        <label>Disable ROM JIT</label>
        <select class="input" id="debug-SS_JIT_NO_ROM">
          <option value="" selected>Off (ROM JIT enabled)</option>
          <option value="1">On — interpret ROM code only</option>
        </select>
        <p class="ss-text-muted">Interpret ROM regions instead of JIT-compiling them. Isolates ROM vs RAM codegen bugs.</p>
      </div>
      <div class="form-group">
        <label>Force Interpreter</label>
        <select class="input" id="debug-SS_USE_JIT">
          <option value="" selected>JIT enabled (default)</option>
          <option value="0">Force interpreter (SS_USE_JIT=0)</option>
        </select>
        <p class="ss-text-muted">Completely disables the JIT. The emulator runs in pure interpreter mode.</p>
      </div>
      <div class="form-group">
        <label>Trace Ring</label>
        <select class="input" id="debug-SS_JIT_TRACE_RING">
          <option value="" selected>Off</option>
          <option value="1">On — circular trace buffer</option>
        </select>
        <p class="ss-text-muted">Records recent PCs in a ring buffer. Dumped on crash or stall. Helps diagnose infinite loops.</p>
      </div>
      <div class="form-group">
        <label>Diagnostic Log Path</label>
        <input type="text" class="input" id="debug-SS_JIT_DIAG_LOG" value="" placeholder="/tmp/jit_diag.log" />
        <p class="ss-text-muted">Write JIT heartbeat diagnostics to this file instead of stderr.</p>
      </div>
      <div class="form-group">
        <label>Watch Address (decimal, comma-separated)</label>
        <input type="text" class="input" id="debug-SS_JIT_WATCH_ADDR" value="" placeholder="e.g. 273866508" />
        <p class="ss-text-muted">Break when these guest addresses are written. For tracking down memory corruption.</p>
      </div>
      <div class="form-group">
        <label>Skip Opcode (decimal primary opcode)</label>
        <input type="text" class="input" id="debug-SS_JIT_SKIP_OPC" value="" placeholder="e.g. 31" />
        <p class="ss-text-muted">Force a specific primary opcode to fall back to the interpreter. For isolating a broken handler.</p>
      </div>
      <div class="form-group" style="margin-top: 24px; padding-top: 16px; border-top: 1px solid var(--ss-border);">
        <label>Run Logs</label>
        <button class="btn btn-secondary btn-sm" data-action="view-logs">View Logs</button>
        <p class="ss-text-muted">Last 10 run logs are kept in the VM's logs/ directory.</p>
      </div>
    `,
  };

  return sections[section] || "";
}

// Separate settings window (used when opened via ?settings=<id> URL param)
function renderSettings(): string {
  const vm = vms.find((v) => v.id === selectedVmId);
  if (!vm) return "";
  const isRunning = runningVmIds.has(vm.id);

  return `
    ${isRunning ? '<div class="settings-running-banner">VM is running. Hardware settings apply on next restart.</div>' : ""}
    <div class="settings-body">
      <div class="settings-sidebar">
        ${["general", "display", "storage", "network", "input", "advanced", "debug"]
          .map((s) => `
            <button class="settings-nav-item ${s === settingsSection ? "active" : ""}"
                    data-action="switch-section" data-section="${s}">
              ${s.charAt(0).toUpperCase() + s.slice(1)}
            </button>
          `).join("")}
      </div>
      <div class="settings-content">
        ${renderSettingsSectionContent(vm, isRunning, settingsSection)}
      </div>
    </div>
    <div class="detail-save-bar">
      <button class="btn btn-primary" data-action="save-settings">Save</button>
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

  // P1.5: Double-click VM row to boot
  document.querySelectorAll(".vm-row").forEach((el) => {
    el.addEventListener("dblclick", (e) => {
      const row = (e.currentTarget as HTMLElement);
      const vmId = row.dataset.id;
      if (vmId && !runningVmIds.has(vmId)) {
        const launchBtn = row.querySelector('[data-action="launch"]');
        if (launchBtn) handleAction({ target: launchBtn } as unknown as Event);
      }
    });
  });

  // P1.4: Right-click context menu
  document.querySelectorAll(".vm-row").forEach((el) => {
    el.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      const row = el as HTMLElement;
      const vmId = row.dataset.id;
      const vmName = row.dataset.name;
      if (!vmId) return;
      showContextMenu(e as MouseEvent, vmId, vmName || "VM");
    });
  });

  // Custom resolution toggle
  const presetSelect = document.getElementById("setting-screen-preset") as HTMLSelectElement | null;
  if (presetSelect) {
    presetSelect.addEventListener("change", () => {
      const customGroup = document.getElementById("custom-res-group");
      if (customGroup) {
        customGroup.style.display = presetSelect.value === "custom" ? "" : "none";
      }
    });
  }
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
    ["setting-screen-preset", "screen", (v) => {
      if (v === "custom") {
        const w = (document.getElementById("setting-screen-w") as HTMLInputElement)?.value || "800";
        const h = (document.getElementById("setting-screen-h") as HTMLInputElement)?.value || "600";
        return `win/${w}/${h}`;
      }
      return v;
    }],
    ["setting-frameskip", "frameskip", (v) => v],
    ["setting-ether", "ether", (v) => v],
    ["setting-nosound", "nosound", (v) => v],
    ["setting-jitcache", "jitcachesize", (v) => v],
    ["setting-gfxaccel", "gfxaccel", (v) => v],
    ["setting-vncserver", "vncserver", (v) => v],
    ["setting-vncport", "vncport", (v) => v],
    ["setting-mousewheelmode", "mousewheelmode", (v) => v],
    ["setting-mousewheellines", "mousewheellines", (v) => v],
    ["setting-swap_opt_cmd", "swap_opt_cmd", (v) => v],
    ["setting-keycodes", "keycodes", (v) => v],
    ["setting-bootdriver", "bootdriver", (v) => v],
    ["setting-nocdrom", "nocdrom", (v) => v],
    ["setting-ignoresegv", "ignoresegv", (v) => v],
    ["setting-ignoreillegal", "ignoreillegal", (v) => v],
    ["setting-idlewait", "idlewait", (v) => v],
    ["setting-jit", "jit", (v) => v],
    ["setting-jit68k", "jit68k", (v) => v],
    ["setting-noclipconversion", "noclipconversion", (v) => v],
    ["setting-hardcursor", "hardcursor", (v) => v],
    ["setting-seriala", "seriala", (v) => v],
    ["setting-serialb", "serialb", (v) => v],
    ["setting-keyboardtype", "keyboardtype", (v) => v],
    ["setting-sdlrender", "sdlrender", (v) => v],
  ];
  for (const [elId, key, transform] of fields) {
    const el = document.getElementById(elId) as HTMLInputElement | HTMLSelectElement | null;
    if (el) {
      pendingSettings[key] = transform(el.value);
    }
  }

  // Capture debug env vars from the Debug section
  const debugIds = [
    "SS_JIT_VERIFY", "SS_JIT_NO_CHAIN", "SS_JIT_NO_ROM", "SS_USE_JIT",
    "SS_JIT_TRACE_RING", "SS_JIT_DIAG_LOG", "SS_JIT_WATCH_ADDR", "SS_JIT_SKIP_OPC",
  ];
  for (const envKey of debugIds) {
    const el = document.getElementById(`debug-${envKey}`) as HTMLInputElement | HTMLSelectElement | null;
    if (el && el.value) {
      debugEnvVars[envKey] = el.value;
    } else if (el) {
      delete debugEnvVars[envKey];
    }
  }
}

async function handleAction(e: Event) {
  const target = (e.target as HTMLElement).closest("[data-action]") as HTMLElement;
  if (!target) return;

  const action = target.dataset.action;
  const id = target.dataset.id;

  switch (action) {
    case "select-vm":
      if (id && id !== selectedVmId) {
        captureCurrentSectionSettings();
        if (Object.keys(pendingSettings).length > 0) {
          if (!confirm("You have unsaved changes. Discard them?")) break;
        }
        selectedVmId = id;
        pendingSettings = {};
        await loadVmPrefs(id);
        render();
      }
      break;

    case "toggle-labels":
      toggleIconLabels();
      break;

    case "show-help":
      showToast("Resources: infinitemac.org · macintoshgarden.org · emaculation.com · 68kmla.org", "info", 10000);
      break;

    case "import-prefs": {
      const path = await pickFile("Select SheepShaver Prefs File");
      if (path) {
        try {
          const name = path.split("/").pop()?.replace("_prefs", "").replace(".", " ") || "Imported VM";
          await invoke("import_from_prefs", { prefsPath: path, name: `Imported: ${name}` });
          vms = await loadVms();
          showToast("VM imported from prefs file", "success");
          render();
        } catch (err) {
          showToast(`Import failed: ${err}`, "error");
        }
      }
      break;
    }

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

    case "wizard-cancel":
      currentView = "library";
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

    case "force-accept-rom":
      wizardState.romStatus = "accepted";
      render();
      break;

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
          await invoke("launch_vm", { id: vm.id, envVars: null });
          runningVmIds.add(vm.id);
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
        if (isSettingsWindow) {
          // Already in a settings window — this shouldn't happen, but handle gracefully
          break;
        }
        // Select the VM and show its detail pane (integrated mode)
        selectedVmId = id;
        settingsSection = "general";
        pendingSettings = {};
        await loadVmPrefs(id);
        render();
      }
      break;

    case "back-to-library":
      if (isSettingsWindow) {
        getCurrentWindow().close();
      } else {
        currentView = "library";
        render();
      }
      break;

    case "launch":
      if (id) {
        try {
          const envVars = Object.keys(debugEnvVars).length > 0 ? debugEnvVars : null;
          await invoke("launch_vm", { id, envVars });
          runningVmIds.add(id);
          vmLaunchTimestamps.set(id, Date.now());
          showToast("VM started. Click inside the classic desktop to capture the mouse. Ctrl-F5 to release.", "info", 8000);
          render();
        } catch (err) {
          showToast(`Failed to launch: ${err}`, "error");
        }
      }
      break;

    case "stop":
      try {
        await invoke("stop_vm", { id });
        if (id) vmLaunchTimestamps.delete(id);
        if (id) runningVmIds.delete(id);
        render();
      } catch (err) {
        console.error("Failed to stop VM:", err);
      }
      break;

    case "reveal":
      if (id) {
        invoke("reveal_vm_in_finder", { id }).catch((err) =>
          showToast(`Failed: ${err}`, "error")
        );
      }
      break;

    case "backup-disk":
      if (selectedVmId) {
        try {
          const result = (await invoke("backup_vm_disk", { id: selectedVmId })) as string;
          showToast(result, "success");
        } catch (err) {
          showToast(`Backup failed: ${err}`, "error");
        }
      }
      break;

    case "duplicate":
      if (id) {
        const vmName = target.dataset.name || "Copy";
        try {
          const dupResult = (await invoke("duplicate_vm", { id, newName: `${vmName} (Copy)` })) as VmProfile;
          vms = await loadVms();
          if (dupResult.shared_disk_warning) {
            showToast(dupResult.shared_disk_warning, "error", 10000);
          } else {
            showToast("VM duplicated", "success");
          }
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
          emit("vm-deleted", { id });
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
          await loadVmPrefs(selectedVmId);
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
        try {
          await invoke("add_vm_disk", { id: selectedVmId, path, isCdrom: false });
          vms = await loadVms();
          showToast("Disk added", "success");
          render();
        } catch (err) {
          showToast(`Failed to add disk: ${err}`, "error");
        }
      }
      break;
    }

    case "view-logs":
      if (selectedVmId) {
        try {
          const logs = (await invoke("list_vm_logs", { id: selectedVmId })) as string[];
          if (logs.length === 0) {
            showToast("No run logs yet — start the VM first", "info");
          } else {
            const latest = logs[0];
            const content = (await invoke("read_vm_log", { id: selectedVmId, logName: latest })) as string;
            const lines = content.split("\n").slice(-50).join("\n");
            alert(`Last 50 lines of ${latest}:\n\n${lines}`);
          }
        } catch (err) {
          showToast(`Failed to read logs: ${err}`, "error");
        }
      }
      break;

    case "pick-setting-extfs": {
      const folderResult = await open({ title: "Select Shared Folder", directory: true, multiple: false });
      const folderPath = typeof folderResult === "string" ? folderResult : null;
      if (folderPath && selectedVmId) {
        await invoke("update_vm_setting", { id: selectedVmId, key: "extfs", value: folderPath });
        await loadVmPrefs(selectedVmId);
        vms = await loadVms();
        showToast("Shared folder set", "success");
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

async function loadScreenshots() {
  for (const vm of vms) {
    try {
      const src = (await invoke("get_vm_screenshot", { id: vm.id })) as string | null;
      if (src) {
        vmScreenshots.set(vm.id, src);
      }
    } catch {
      // No screenshot available
    }
  }
}

let screenshotCounter = 0;
let screenshotInProgress = false;

async function pollRunningStatus() {
  const newRunning = await checkRunning();
  const changed = newRunning.size !== runningVmIds.size ||
    [...newRunning].some((id) => !runningVmIds.has(id));

  if (changed) {
    // Detect VMs that just stopped
    for (const id of runningVmIds) {
      if (!newRunning.has(id)) {
        vmLaunchTimestamps.delete(id);
      }
    }
    runningVmIds = newRunning;
    if (runningVmIds.size === 0) {
      vms = await loadVms();
      await loadScreenshots();
    }
    if (currentView === "library") render();
  }

  // Capture live screenshots every ~10s for all running VMs (guarded against reentrancy)
  if (runningVmIds.size > 0 && !screenshotInProgress && ++screenshotCounter >= 5) {
    screenshotCounter = 0;
    screenshotInProgress = true;
    for (const id of runningVmIds) {
      try {
        await invoke("capture_vm_screenshot", { id });
        const src = (await invoke("get_vm_screenshot", { id })) as string | null;
        if (src) vmScreenshots.set(id, src);
      } catch { /* VNC may not be ready */ }
    }
    screenshotInProgress = false;
    if (currentView === "library") render();
  }
}

function showContextMenu(e: MouseEvent, vmId: string, vmName: string) {
  const existing = document.getElementById("context-menu");
  if (existing) existing.remove();

  const isRunning = runningVmIds.has(vmId);
  const menu = document.createElement("div");
  menu.id = "context-menu";
  menu.className = "context-menu";
  menu.innerHTML = `
    ${isRunning
      ? `<button class="context-menu__item" data-action="stop" data-id="${escapeAttr(vmId)}">${icon(iconDisplayOn, "", 14)} Shut Down</button>`
      : `<button class="context-menu__item" data-action="launch" data-id="${escapeAttr(vmId)}">${icon(iconDisplayOff, "", 14)} Start</button>`
    }
    <button class="context-menu__item" data-action="settings" data-id="${escapeAttr(vmId)}">${icon(iconToolbox, "", 14)} Configure</button>
    <div class="context-menu__sep"></div>
    <button class="context-menu__item" data-action="duplicate" data-id="${escapeAttr(vmId)}" data-name="${escapeAttr(vmName)}">⎘ Duplicate</button>
    <button class="context-menu__item" data-action="reveal" data-id="${escapeAttr(vmId)}">${icon(iconFolder, "", 14)} Show in Finder</button>
    <div class="context-menu__sep"></div>
    <button class="context-menu__item context-menu__item--danger" data-action="delete" data-id="${escapeAttr(vmId)}">${icon(iconTrash, "", 14)} Delete</button>
  `;
  menu.style.left = `${e.clientX}px`;
  menu.style.top = `${e.clientY}px`;
  document.body.appendChild(menu);

  menu.querySelectorAll("[data-action]").forEach((btn) => {
    btn.addEventListener("click", (ev) => {
      menu.remove();
      handleAction(ev);
    });
  });

  const dismiss = (ev: Event) => {
    if (!menu.contains(ev.target as Node)) {
      menu.remove();
      document.removeEventListener("click", dismiss);
    }
  };
  setTimeout(() => document.addEventListener("click", dismiss), 0);
}

async function handleFileDrop(paths: string[]) {
  for (const path of paths) {
    const lower = path.toLowerCase();

    if (lower.endsWith(".rom") || lower.includes("rom")) {
      // ROM file — use it in the wizard
      if (currentView === "wizard" && wizardStep === 1) {
        wizardState.romPath = path;
        try {
          const info = (await invoke("verify_rom", { path })) as RomInfo;
          wizardState.romStatus = info.status as "" | "verified" | "accepted" | "error";
          wizardState.romName = info.name;
        } catch {
          wizardState.romStatus = "accepted";
        }
        render();
        showToast("ROM file dropped", "info");
      } else {
        showToast("Drop ROM files on the wizard's ROM step", "info");
      }
    } else if (lower.endsWith("_prefs") || lower.includes("sheepshaver_prefs") || lower.includes("prefs")) {
      // Prefs file — import as a new VM
      try {
        const name = path.split("/").pop()?.replace("_prefs", "").replace(".", " ") || "Imported VM";
        await invoke("import_from_prefs", { prefsPath: path, name: `Imported: ${name}` });
        vms = await loadVms();
        currentView = "library";
        render();
        showToast("VM imported from prefs file", "success");
      } catch (err) {
        showToast(`Import failed: ${err}`, "error");
      }
    } else if (lower.endsWith(".dsk") || lower.endsWith(".img") || lower.endsWith(".hfv")) {
      // Disk image — use in wizard or settings
      if (currentView === "wizard" && wizardStep === 2) {
        wizardState.diskMode = "existing";
        wizardState.diskPath = path;
        render();
        showToast("Disk image dropped", "info");
      } else {
        showToast("Drop disk images on the wizard's Disk step, or in VM settings", "info");
      }
    } else if (lower.endsWith(".iso") || lower.endsWith(".toast") || lower.endsWith(".cdr")) {
      // CD image
      if (currentView === "wizard" && wizardStep === 2) {
        wizardState.cdPath = path;
        render();
        showToast("CD image dropped", "info");
      }
    }
  }
}

async function init() {
  vms = await loadVms();
  runningVmIds = await checkRunning();

  // Detect if this is a settings window (opened with ?settings=<vmid>)
  const params = new URLSearchParams(window.location.search);
  const settingsVmId = params.get("settings");

  if (settingsVmId) {
    selectedVmId = settingsVmId;
    settingsSection = "general";
    pendingSettings = {};
    await loadVmPrefs(settingsVmId);
    currentView = "settings";
    isSettingsWindow = true;

    // P0.1: Unsaved changes confirmation on window close
    const thisWindow = getCurrentWindow();
    thisWindow.onCloseRequested(async (event) => {
      captureCurrentSectionSettings();
      if (Object.keys(pendingSettings).length > 0) {
        const discard = confirm("You have unsaved changes. Discard them?");
        if (!discard) {
          event.preventDefault();
          return;
        }
      }
    });

    // P0.2: Auto-close if the VM is deleted from the Control Center
    listen("vm-deleted", (event) => {
      const deletedId = (event.payload as { id: string })?.id;
      if (deletedId === settingsVmId) {
        thisWindow.close();
      }
    });

    render();
    return;
  }

  // Main window — Control Center
  await loadScreenshots();

  try {
    const status = (await invoke("check_emulator_status")) as { found: boolean; path: string };
    if (!status.found) {
      errorBanner = "SheepShaver binary not found. Build it first: cd SheepShaver && make build-ss";
    }
  } catch {
    // Not fatal — may be running in browser preview
  }

  listen<{ paths: string[] }>("tauri://drag-drop", (event) => {
    if (event.payload.paths?.length) {
      handleFileDrop(event.payload.paths);
    }
  });

  render();
  statusPollInterval = setInterval(pollRunningStatus, 2000);
}

init();
