# Demanding Mac OS 9 / PowerPC software — a stress-test catalog (with sources)

> **Status:** 🟡 Open — living catalog · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** A curated, *linkable* list of period-correct, demanding PowerPC Mac OS 9
> software for stress-testing the JIT — especially the **AltiVec/FP-under-real-software** gap (ROADMAP
> A2/A3, A5-V). Prioritized by (1) actually uses AltiVec, (2) CPU-bound (runs in the emulator without
> special hardware), (3) still downloadable. Companion to [`TESTING.md`](TESTING.md) (the *methodology*).
> _Markers: ✅ done · 🟡 in progress · ⏸ deferred · ☐ todo. Sources are community abandonware archives;
> software licensing is the user's call (same grey area as ROMs)._

## How to use this

Run a workload **once under `SS_JIT_VERIFY=1`** (the differential interp-vs-JIT oracle) to shake out
codegen bugs in VRs/FPRs the harness can't see — this is the "real software" validation `TESTING.md`
calls the gold standard. The **AltiVec** titles are the highest value: AltiVec is our recurring
correctness gap (the `ev_mixed` / shift-rotate / `vmul*h` bugs), and almost nothing in the automated
harness or a plain boot exercises it. Also run them under the B1 profiler for hot-routine/HLE data.

### Hard emulator caveats (gate everything)
- **No 3D acceleration** (no RAVE/QuickDraw3D/OpenGL hardware) → OpenGL-only games are non-starters;
  software-renderer games and *offline* 3D render are fine.
- **No FireWire/DV capture** → video *capture* paths don't work, but *render/export from existing
  clips* does (camera-free).
- **No GPU video decode existed in this era** → all codec decode is CPU-bound, so all of it is valid.
- Several apps **need serial numbers** (flagged); distributed-compute clients **want internet** (flagged).

---

## ★ Best AltiVec picks (start here — these actually exercise the vector emit paths)

| Rank | Title / version | Why it's the pick | Download | Caveat |
|---|---|---|---|---|
| 1 | **AltiVec Fractal Carbon 1.3** (Dauger Research) | **Purpose-built AltiVec** Mandelbrot; **falls back to scalar on non-G4 → built-in A/B oracle.** No license, no network, no source media. The cleanest possible vector torture test. | [daugerresearch.com/fractals/altivecfractalcarbon.shtml](https://daugerresearch.com/fractals/altivecfractalcarbon.shtml) | Needs a window (not headless); OS 8.5+/CarbonLib 1.2+. **Zero-friction.** |
| 2 | **SoundJam MP Plus 2.5.x** / **iTunes 1.x** | **Confirmed AltiVec MP3 encode** ("Velocity Engine"); the app `TESTING.md` already names as the gold standard. Batch-encode = sustained vector DSP. | [Macintosh Garden](https://macintoshgarden.org/apps/soundjam) · [Macintosh Repository](https://www.macintoshrepository.org/2545-soundjam-mp) · [archive.org](https://archive.org/details/sound-jam-mp-plus-cd) | Needs a source audio file to encode. **Zero-friction otherwise.** iTunes 1.x inherited the same encoder. |
| 3 | **Photoshop 5.5 + Adobe "AltiVecCore" plug-in** | The historically famous AltiVec **"bake-off"**: Gaussian Blur (+48%), Lighting Effects (+187%) on G4. Big-radius blur on a large image = the canonical AltiVec stress action. | Plug-in: [Adobe ftpID=1028](https://supportdownloads.adobe.com/detail.jsp?ftpID=1028) · PS 5.x: [Macintosh Repository](https://www.macintoshrepository.org/117-adobe-photoshop-5-0-x) | **Needs serial** + the **separate AltiVecCore plug-in installed**. ⚠️ **PS 6.0 removed AltiVec**; PS 7 has it but needs OS 9.1+. |
| 4 | **Sorenson Video 3 encode** (via QuickTime 5.0.2 Pro / Cleaner 5) | **AltiVec video encode** — the heaviest sustained vector+FP workload of the era. | [Macintosh Repository – SV3](https://www.macintoshrepository.org/16443-sorenson-video-3) | QuickTime **Pro / Cleaner need a key**; needs a source `.mov`/`.dv` clip (no camera). |
| 5 | **LightWave 3D 7b render** (NewTek) | **7b explicitly added AltiVec render** (Macworld: up to ~87% faster). Renders to file = unattended. | [Macintosh Repository](https://www.macintoshrepository.org) (search "LightWave 7") | Dongle/serial historically; check the abandonware build. Render is CPU-only. |
| 6 | **distributed.net `dnetc` (classic Mac OS, AltiVec RC5)** | *The* OS 9 AltiVec distributed-compute icon — a hand-written AltiVec inner loop that runs flat-out indefinitely. Ideal infinite stress kernel. | ⚠️ **Hunt the archive** — the [official page](https://www.distributed.net/Download_clients) now serves only an OS X PPC build; the **classic OS 9 GUI client** must be found on archive.org / Mac Garden / Repository. | Wants internet to fetch/flush blocks (can buffer offline). **Highest value *if* the OS 9 binary is located.** |

---

## Scalar-FP backups (no AltiVec, but CPU-bound and easy to run)

| Title / version | Why | Download | Caveat |
|---|---|---|---|
| **POV-Ray 3.5 / 3.6 (Mac PPC)** | The classic FP raytracer; ships `benchmark.pov`; deterministic, **renders to file unattended.** Cleanest scalar-FP torture. | [Macintosh Repository](https://www.macintoshrepository.org/550-pov-ray-3-x) | **Stock Mac build is NOT AltiVec** (AltiVec was unofficial MegaPOV patches). Pure scalar FP. |
| **Mathematica 4.x** | Arbitrary-precision + dense linear algebra = brutal scalar FP; trivially scriptable to spin CPU. | [Macintosh Repository](https://www.macintoshrepository.org/705-mathematica-4) · [Macintosh Garden](https://macintoshgarden.org/apps/mathematica-4) | Needs activation/license. No AltiVec. |
| **N2MP3 / LAME (Mac) MP3 encode** | LAME-based scalar MP3 encode — heavy integer/FP. | [RareWares](https://www.rarewares.org/rrw/n2mp3.php) · [lame.sourceforge.io](https://lame.sourceforge.io/) | Scalar (no committed classic-era AltiVec LAME). |
| **GraphicConverter (4.x / 6.5 Classic)** | Batch-convert/filter large image sets = sustained scalar load. | [Macintosh Garden 6.5 Classic](https://macintoshgarden.org/apps/graphicconverter-65-classic) (free reg code on page) | Not AltiVec-marketed. |

---

## Media decode (all CPU-bound — no GPU decode existed)

| Title | Why | Download | Caveat |
|---|---|---|---|
| **QuickTime 5/6** + **Sorenson Video 3** / **MPEG-1** / **3ivx-DivX** clips | Software codec decode = steady FP/integer load; SV3 and MPEG-4 ASP (3ivx) are the heaviest. QT6 added some AltiVec decode paths. | QuickTime 6 on [Macintosh Repository](https://www.macintoshrepository.org); 3ivx/DivX components on [Macintosh Garden](https://macintoshgarden.org) | Verify OS 9 (not OS X) QuickTime components. Cinepak is *light* (baseline). |

## Video editing render (camera-free export = valid stress)

| Title | Why | Download | Caveat |
|---|---|---|---|
| **iMovie 2** (OS 9) | Effect/transition render + **"Export to QuickTime" is pure CPU** — confirmed to work **without a DV camera**. Apple cited iMovie as an early AltiVec adopter (partial). | [Macintosh Repository / Garden](https://www.macintoshrepository.org) (search "iMovie 2") | *Capture* needs FireWire (skip); render/export is the test. |
| **Final Cut Pro 1.2 / 2.0** (OS 9) | Timeline effects render-to-disk from existing clips = sustained FP/int (some AltiVec in 2.x). | [Macintosh Repository](https://www.macintoshrepository.org) ("Final Cut Pro 2") | **Needs serial**; capture needs FireWire (render path doesn't). |

## Games (software-renderer only — additions to the existing Marathon/Quake1 set)

| Title | Runs? | Why | Download |
|---|---|---|---|
| **Myth II: Soulblighter** | ✅ software renderer | large unit sim + terrain = CPU/FP, full-stack | [Macintosh Garden](https://macintoshgarden.org) ("Myth II") |
| **Tomb Raider 1 / 2** (Aspyr) | ✅ software mode | affine texture + transform/lighting on CPU | [Macintosh Garden](https://macintoshgarden.org/games/tomb-raider) (TR3 reportedly crashes SheepShaver — use TR1/TR2) |
| **Unreal Tournament (GOTY)** | ⚠️ verify | useful *only if* the Mac port ships the software renderer | [Macintosh Garden](https://macintoshgarden.org) |
| **Quake III, Tony Hawk, Oni** | ❌ skip | OpenGL/3D-HW oriented — non-starters without 3D acceleration | — |

## Benchmarks / diagnostics (repeatable scoring across JIT changes)

| Title | Use | Download |
|---|---|---|
| **MacBench 5.0** (Ziff-Davis) | CPU/FP/memory micro-suite (use the **processor/FP** subtests) | [Macintosh Repository](https://www.macintoshrepository.org/2337-macbench-5-0) |
| **Speedometer 4.0.2** | classic PPC CPU/FP benchmark (CPU/FP tests are the useful ones) | [Macintosh Repository](https://www.macintoshrepository.org/12926-speedometer-4-x) |
| **Photoshop 5.5 + AltiVecCore — Gaussian Blur / Lighting Effects** | the canonical repeatable **AltiVec** measurement | see AltiVec picks #3 above |

---

## Accuracy hedges (don't over-claim)
- **Official SETI@home = NO AltiVec** (architecture-agnostic; AltiVec SETI clients were OS X). Use only as a *scalar FP* load.
- **POV-Ray stock Mac = NO AltiVec.** **Photoshop 6.0 = AltiVec removed** (5.5+plug-in and 7 have it).
- **Confirmed AltiVec renderers:** LightWave 7b. **Unconfirmed for their OS 9 builds (don't assume):**
  Cinema 4D, Carrara, Bryce, Strata, Infini-D.
- **Could not pin a live OS 9 download:** classic-Mac `dnetc` AltiVec client, classic-Mac MATLAB,
  classic Folding@home — flag for manual archive-hunting.
- Archive URLs drift — if a direct link rots, search the site (Macintosh Garden / Repository / archive.org).

## The three to actually try first (zero-friction)
1. **AltiVec Fractal Carbon** — AltiVec, built-in scalar oracle, no license/network/media.
2. **SoundJam MP** — AltiVec MP3 encode (just needs a source audio file).
3. **POV-Ray 3.6** — scalar-FP, ships its own benchmark scene, renders unattended.

## Roadmap ties
- **A2/A3** (AltiVec correctness + "AltiVec under real software"): the AltiVec picks are the in-the-wild
  validation path — run under `SS_JIT_VERIFY`.
- **A5-V** (`SS_JIT_VERIFY`-under-E2E): these are the workloads to drive in that nightly gate.
- **B1 profiler / HLE**: same workloads under the profiler surface hot AltiVec/FP routines (HLE candidates).
- **Concurrency** (`SheepShaver/docs/CONCURRENCY-MODEL.md`): the multicore-fragile titles (Royal Flush,
  The Dig, A-Train — see `TESTING.md`) are a *separate* list for the thread-safety empirical test.
