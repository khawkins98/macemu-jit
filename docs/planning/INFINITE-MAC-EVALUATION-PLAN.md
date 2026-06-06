# Plan: Infinite Mac comparative evaluation (host/runtime integration)

> **Status:** 🟡 In progress — exploratory evaluation · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Track a disciplined comparison against Infinite Mac for host/runtime techniques we may adapt in native macOS flows.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR

Use [mihaip/infinite-mac](https://github.com/mihaip/infinite-mac) as a **runtime-orchestration
reference**, not a product-direction mandate.

High-value angle: host integration patterns (dynamic file injection, media loading, emulator
multi-instance networking).

> **Validated 2026-06-06 (web).**
> - **Infinite Mac runs our *own* SheepShaver lineage** — it compiles the same cebix/kanjitalk755
>   SheepShaver (plus Basilisk II, Mini vMac, DingusPPC, Previous, PearPC) to WASM. So it's a
>   **downstream sibling**: a live, working example of host-integrating *the exact emulator core we
>   fork*, just in a browser shell instead of native macOS. That makes it the most useful of the three
>   references for Silicon Sheep's host/runtime integration.
> - **"Includes Snow" — true but secondary.** DingusPPC is its core PowerPC engine; Snow was added
>   later as an additional 68K core (not in the canonical "original emulators" set). Don't overweight
>   the Snow tie.
> - **Networking detail:** AppleTalk over **subdomain-defined zones** relayed through Cloudflare
>   Durable Objects (`ether_js.cpp` → JS), **off by default**. It's a web-relay overlay — a *design
>   comparator* for our native VDE/bridging, not a transferable mechanism.
> - **Runtime media injection:** *build-time* disk/CD import is documented (`import-disks`/
>   `import-cd-roms`); live drag-and-drop injection is a product feature but **not** spelled out in repo
>   docs — verify before citing as an architecture pattern.

---

## What looked immediately useful

From the current code/docs:

- **Dynamic file/media injection path** exists in the UI↔worker boundary:
  upload actions + CD-ROM loads are pushed at runtime and consumed in worker-side request drains.
- **Lazy/chunked upload access** is implemented for uploaded media, including browser-specific
  handling and range-based chunking.
- **Zone-based networking model** is explicit:
  a shared Ethernet zone model routes packets between participating emulator instances, including
  AppleTalk-targeted packet routing in that virtual zone.
- **Operationally clear deployment split** (frontend + worker + storage sync) gives useful ideas for
  environment bootstrap and asset-pipeline hygiene.

---

## Important networking clarification

Infinite Mac does have AppleTalk support in its own environment model (virtual named zones between
instances), so this is **not** a "no AppleTalk" system. The practical question is whether that
zone-scoped overlay maps to our native macOS networking goals (VDE/TAP/host bridging), rather than
whether AppleTalk exists at all.

---

## Scope and non-goals

**In scope**
- Host/runtime integration patterns we can adapt to native workflows.
- Dynamic file/share/media availability mechanics.
- Network-topology design ideas and where they constrain or enable functionality.
- Lessons for Silicon Sheep setup UX and diagnostics.

**Out of scope**
- Rebuilding this repo as a browser/WASM product.
- Replacing emulator cores because of web-runtime architecture choices.
- Importing code directly.

---

## Adopt / defer / avoid (initial pass)

### Adopt now (low risk, high leverage)
- **Hot disk/asset injection + software library** — ✅ **folded into the SiliconSheep plan**
  (`DESKTOP_INTEGRATION_PLAN.md` Tier 2 → "Hot disk/CD insertion + software library"). Key validated
  finding (2026-06-06, persistent.info write-up): Infinite Mac's runtime injection rides the **same
  `extfs.cpp`** we have, plus a watched **Downloads/Uploads/Saved** folder convention; its disk
  *streaming* (256 K content-addressed HTTP-range chunks, service worker, IndexedDB, Emscripten FS) is
  **browser-only and does not transfer** — native macOS uses real files + APFS `clonefile` dedup and
  skips the streaming layer. SheepShaver's `disk.cpp` already has the runtime mount machinery
  (`DiskMountVolume`/`to_be_mounted`/`mount_mountable_volumes`), so true hot disk insertion is feasible.
- Runtime action-queue patterns for file/media injection with explicit boundaries.
- Better documentation of network mode capabilities/limits (mirroring their clear zone model).
- Host setup ergonomics around environment bootstrap and data-pipeline staging.

### Defer (good ideas, wrong timing)
- Any web-only architecture work that does not transfer to native runtime.
- Large host-network re-architecture before current Track A/B priorities settle.

### Avoid for now
- Strategy shifts that optimize for browser constraints over native macOS goals.

---

## Work plan

### I0. Baseline capture — ✅ done
- Confirmed relevant areas: files/media actions, chunked upload/media path, ethernet zone routing.
- Confirmed project overlap with DingusPPC/Snow as a comparative discovery source.

### I1. Crosswalk into native roadmap — 🟡 in progress
- Build a matrix mapping Infinite Mac runtime patterns to:
  - Silicon Sheep setup/UX,
  - SheepShaver file/network integration points,
  - and current constraints in this repo.

### I2. Pick bounded experiments — ☐ todo
- Select 2-3 low-risk, native-friendly experiments with success/rollback criteria.

### I3. Decide keep/defer status — ☐ todo
- Promote adopted items into Track C/A docs; keep others as deferred comparators.

---

## Early recommendations

1. Treat Infinite Mac as the strongest **host integration** reference among current external targets.
2. Keep its AppleTalk zone model as a **networking design comparator**, not a direct substitute for
   native bridging models.
3. Use it as a curated "ecosystem radar" source alongside direct Dingus/Snow reviews.
