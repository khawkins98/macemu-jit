# Classic Mac OS HIG Behavioral Patterns for SiliconSheep

> **Status:** 📖 Reference · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** Behavioral design guide for SiliconSheep, grounded in the Apple Macintosh Human Interface Guidelines (1992/1995) and classic Mac OS 9 pro app patterns (CodeWarrior, ResEdit, Conflict Catcher). Focuses on interaction design, not visual styling.

> Source: *Inside Macintosh: Macintosh Human Interface Guidelines* (1992/1995),
> *Mac OS 8 Human Interface Guidelines* — behavioral chapters at
> `dev.os9.ca/techpubs/mac/HIGuidelines/HIGuidelines-{113,117,127,133,137,139}.html`.
> App-pattern research from CodeWarrior IDE, ResEdit, Conflict Catcher, Apple System Profiler.

---

## 1. Window Behavior

**Click-to-activate, no click-through.** The first click in an inactive document window
only activates it — it performs no other action. "The user must click again. This behavior
protects the user from losing an existing selection when the window becomes active." A
web/Tauri stack violates this by default; SiliconSheep must intercept the first click on
an inactive VM detail pane or config panel and suppress its action.

**State restoration.** "When the user activates a window that had been deactivated,
reinstate the window just the way it was before." Restore scroll position, selection,
and expand/collapse state.

**Position persistence.** "Save window positions, and reopen windows in the size and
position in which the user left them." Before reopening, verify the saved position is
reasonable for the current monitor geometry. If the user did not move/resize, do not
persist a default position.

**Staggering.** First window goes upper-left; each subsequent window opens below and to
the right of its predecessor, always on the screen containing the frontmost window.

**Floating palettes (utility windows).** Float above all document windows at all times.
Controls in palettes act immediately without stealing key-window status — the document
window remains active. Use for an inspector/sidebar that shows VM details while the VM
library list stays focused.

## 2. Dialogs and Alerts

**Modal vs modeless.** Use movable-modal (preferred) when input is required before
proceeding (first-run wizard, "create VM" flow). Use modeless for ongoing attribute
editing (prefs panel, VM config) — changes "appear to take effect immediately."

**Never nest modals.** "The user should never see more than two modal dialog boxes on the
screen at any time." Avoid the "tunneling" anti-pattern where dismissing one modal
spawns another — "the user can't predict what will happen next."

**Alert hierarchy.** Note (informational, single OK), Caution (destructive action
pending, OK/Continue + Cancel), Stop (action impossible, single OK).

**Save/Don't Save/Cancel.** Don't Save sits far left, visually separated. Cancel and Save
group at right; Save is the default button (Return). Keyboard: Esc = Cancel, Cmd-D =
Don't Save. The spatial separation of the destructive option is the point.

**Dialog positioning.** Center horizontally, one-fifth of vertical space above it. If
related to a document window, position relative to it with one-fifth of the document
visible above.

## 3. Navigation and Document Model

**One object, one window (spatial model).** Each VM gets its own detail window; opening
it always shows the same window in the same position. Double-clicking a VM in the
library list opens (or raises) its window — never reuses another VM's window.

**Master-detail.** Select in a list, edit in an adjacent pane or a spawned window. The
list is the stable anchor; the detail area updates to match selection.

**Preferences.** A single modeless window with a tab or sidebar selector for categories.
Changes take effect immediately or on explicit Apply. Closing = "I'm done," not "cancel."

## 4. Feedback and State

**Progress.** Indeterminate barber-pole when duration is unknown (VM boot). Determinate
thermometer when estimable (disk image creation). Switch from indeterminate to
determinate once the duration becomes known.

**Disabled controls.** Dim the control; classic HIG had no tooltip/explanation affordance.
*Adaptation for SiliconSheep:* add a help-tag tooltip explaining why (e.g., "Start
requires a ROM file") — this extends the classic rule rather than contradicting it.

**Destructive actions.** Gate behind a Caution alert (Continue + Cancel, default = safe
choice) rather than promising Undo. VM operations (delete disk, reset prefs) are not
document-style undoable; confirmation is the safety net.

## 5. Exemplar Apps

| App | Pattern to adopt |
|-----|-----------------|
| **Conflict Catcher** | Scrolling list of items + per-item enable/disable toggles + saved "sets" = VM library with per-VM on/off and profile presets |
| **ResEdit** | Master-detail: resource type list on left, resource editor on right = VM list + config editor |
| **Apple System Profiler** | Tabbed info panes for hardware/software/network = VM detail tabs (hardware, storage, network) |
| **Sherlock** | Multi-pane: query top, results bottom = log viewer with filter bar above, scrolling output below |
| **CodeWarrior IDE** | Single project window with multiple panes (files, targets, errors) = single VM window with config + status + log panes |

---

## 6. Concrete Recommendations for SiliconSheep

Based on the patterns above — prioritized by how much they'd improve the current UI:

### 6.1 Collapse Settings into the Control Center (Conflict Catcher model)

The separate settings window creates a disconnect: "which VM am I configuring?" Conflict
Catcher's integrated detail pane is the right model — selecting a VM in the list shows its
configuration in the right/bottom panel of the same window. CodeWarrior's separate settings
dialog worked for heavyweight project documents; VMs in a list are lightweight selections.

**Recommendation:** evolve toward a master-detail layout. The Control Center becomes a
sidebar list; selecting a VM shows its config in the main area. The separate settings window
stays as a fallback (for users who detach it), but the default is integrated.

### 6.2 "Apply on next restart" for running VMs (CodeWarrior model)

CodeWarrior let you edit target settings while building — changes applied to the *next*
build, not the active one. SiliconSheep should follow this: let users edit all settings
while a VM runs, but mark changed-but-not-yet-applied fields with a dot or asterisk. Show
"Changes apply on restart" in the header. Don't block settings access.

### 6.3 Progress inline, not in dialogs (Norton/Disk First Aid model)

Disk image creation, first-run wizard steps, and boot progress should appear as an inline
progress area within the Control Center, not as modal dialogs or separate windows.

### 6.4 Spatial window identity (HIG + ResEdit model)

Each VM's detail/config window should remember its position and restore it on reopen.
Double-clicking a VM opens (or raises) its window — never reuses another VM's window. This
is the spatial model: the window IS the VM.

### 6.5 Save/Don't Save/Cancel ordering

Our unsaved-changes dialog should follow HIG: Don't Save far left (spatially separated),
Cancel and Save grouped at right, Save as the default. Keyboard: Return=Save, Esc=Cancel.

### 6.6 Click-to-activate suppression

The first click in an inactive window (e.g., switching from Control Center to a VM config)
should only activate it, not trigger any button action. This needs explicit handling in our
Tauri web layer — web views pass clicks through by default.
