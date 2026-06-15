/*
 *  trampoline_loader.h - SS_M18 Stage 2b, T1: staged-asset MacOS.elf ELF loader
 *
 *  Operation NewSheep. Loads the md5-verified 9.0.1 Trampoline (MacOS.elf) and
 *  places its 2 PT_LOAD segments at their fixed ELF vaddrs in guest RAM, behind
 *  SS_M18_TRAMPOLINE  MachineProfileIsNewWorld()  a 9.0.1-ROM-identity guard
 *  (default OFF). NO relocation off vaddrs (ET_EXEC; the PIC entry stub
 *  self-relocates - Stop-rule #6). T1 ONLY loads; T3 sets registers + jumps.
 *
 *  Plan: docs/superpowers/plans/2026-06-15-ss-m18-s2b-impl-loader-launch.md (rev-2).
 *  Contracts: docs/planning/newsheep/FINDINGS-s2b-loader-handoff.md.
 *
 *  The pure ELF-parse/place core (tramp_parse_elf / tramp_place_image) has no
 *  emulator dependency and is exercised standalone by test_trampoline_loader.cpp.
 *  The emulator-facing wrapper (TrampolineLoader*) is compiled into the binary
 *  only outside the standalone unit-test build.
 */

#ifndef TRAMPOLINE_LOADER_H
#define TRAMPOLINE_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 *  Asset manifest (A-4: pinned, reproducible; the loader FAILS LOUD   *
 *  if the staged asset is absent or its identity does not match).     *
 * ------------------------------------------------------------------ */

/* Staged 9.0.1 Trampoline. The 94144 B binary is NOT committed to git
 * (out-of-tree artifact extracted from the 9.0.1 parcels via tbxi). It is
 * pinned here by size + md5 + a runtime FNV-1a-64 integrity hash. Override the
 * path with the SS_M18_ASSET env var (CI / clone reproducibility). The loader
 * STOPs with [S2B-ASSET-MISSING] / [S2B-ASSET-MISMATCH] if the bytes are absent
 * or do not match - never a silent fall-through. */
#define TRAMP_ASSET_DEFAULT_PATH "/tmp/newsheep/dump-9.0.1/MacOS.elf"
#define TRAMP_ASSET_ENV          "SS_M18_ASSET"
#define TRAMP_ASSET_SIZE         94144u
#define TRAMP_ASSET_MD5          "1300a95e1a243c582c6c7d619075075a"
#define TRAMP_ASSET_FNV1A64      0xd48d6ab1eca34849ULL  /* FNV-1a-64 of the 94144 B asset */

/* ELF facts (re-verified from the asset; see plan "Codebase facts"). */
#define TRAMP_ENTRY              0x20f078u   /* PIC mflr/bl .+8 self-reloc stub */
#define TRAMP_DATA_VADDR         0x100000u   /* PT_LOAD rw: filesz 0x6bc0 memsz 0x19920 */
#define TRAMP_DATA_FILESZ        0x6bc0u
#define TRAMP_DATA_MEMSZ         0x19920u
#define TRAMP_EXEC_VADDR         0x200000u   /* PT_LOAD r-x: filesz 0x10260 memsz 0x10260 */
#define TRAMP_EXEC_FILESZ        0x10260u
#define TRAMP_APERTURE_TOP       0x210260u   /* highest placed guest addr (exec vaddr + memsz) */

/* ROM-identity guard (A-1, Stop-rule #12). The staged ELF is the 9.0.1
 * Trampoline; it derives NanoKernelEntry from the LIVE ROM's component table at
 * runtime, so it MUST be launched against the 9.0.1 ROM. Both the active 1.1 ROM
 * and the 9.0.1 ROM satisfy MachineProfileIsNewWorld(), so the gate additionally
 * REQUIRES the decoded ROM's checksum word (ROMBaseHost+0, big-endian) to match
 * the 9.0.1 value below. Verified from the decoded 9.0.1 'rom ' parcel image
 * (md5 7b1378be15d99ac1a15ab2fc22bcc56f - already referenced at glue:1888;
 * ROM file md5 66210b4f71df8a580eb175f52b9d0f88). The 1.1 ROM checksum is
 * 0xfd86d120 (rom_patches.cpp:110) and is correctly REFUSED. */
#define TRAMP_ROM_CHECKSUM_9_0_1 0xec86128eu
#define TRAMP_ROM_FILE_MD5_9_0_1 "66210b4f71df8a580eb175f52b9d0f88"
#define TRAMP_ROM_IMAGE_MD5_9_0_1 "7b1378be15d99ac1a15ab2fc22bcc56f"

/* ------------------------------------------------------------------ *
 *  Pure ELF parse + place core (no emulator dependency; unit-tested). *
 * ------------------------------------------------------------------ */

#define TRAMP_MAX_SEG 8

typedef struct {
	uint32_t vaddr;
	uint32_t off;     /* file offset of the segment bytes */
	uint32_t filesz;
	uint32_t memsz;
	uint32_t flags;   /* PF_X=1 PF_W=2 PF_R=4 */
} TrampSeg;

typedef struct {
	uint32_t entry;
	int      n_seg;   /* number of PT_LOAD segments collected */
	TrampSeg seg[TRAMP_MAX_SEG];
} TrampImage;

/* Translate a guest address to a host pointer (emulator: Mac2HostAddr;
 * unit test: a flat mock-RAM base). */
typedef uint8_t *(*tramp_xlate_fn)(uint32_t guest_addr, void *ctx);

/* Parse the ELF header + program headers; collect PT_LOAD into out.
 * Validates: ELF magic, ELFCLASS32, ELFDATA2MSB (big-endian), EM_PPC, ET_EXEC.
 * Returns 0 on success, <0 on malformed input (message in err). */
int tramp_parse_elf(const uint8_t *elf, size_t size, TrampImage *out,
                    char *err, size_t errsz);

/* Place each PT_LOAD: copy filesz bytes to xlate(vaddr); zero-fill memsz-filesz.
 * Asserts every placed range stays below aperture_top and refuses (returns <0,
 * message in err) on overflow - NEVER a relocation hack (Stop-rule #6). The
 * caller is responsible for guest aperture base/bounds; this routine writes only
 * through xlate(). Returns 0 on success. */
int tramp_place_image(const uint8_t *elf, size_t size, const TrampImage *img,
                      tramp_xlate_fn xlate, void *ctx, uint32_t aperture_top,
                      char *err, size_t errsz);

/* FNV-1a-64 over a byte range (asset integrity check; matches TRAMP_ASSET_FNV1A64). */
uint64_t tramp_fnv1a64(const uint8_t *p, size_t n);

#ifndef TRAMPOLINE_LOADER_STANDALONE_TEST
/* ------------------------------------------------------------------ *
 *  Emulator-facing wrapper (boot-latched gate; reads the staged asset *
 *  and places it via Mac2HostAddr). Linked into the SheepShaver binary.*
 * ------------------------------------------------------------------ */

/* Boot-latched gate: SS_M18_TRAMPOLINE  MachineProfileIsNewWorld()  the
 * 9.0.1-ROM-identity guard. Resolved ONCE (mirrors NkSupervisorEnabled()).
 * On SS_M18_TRAMPOLINE ON but a non-9.0.1 ROM: emits [S2B-ROM-MISMATCH] and
 * returns false (the caller must NOT launch - Stop-rule #12). */
bool TrampolineLoaderGateEnabled(void);

/* Load the staged asset and place its PT_LOAD segments into guest RAM. Must be
 * called only when TrampolineLoaderGateEnabled(). Returns 0 on success (and
 * latches TrampolineLoaderRan()), <0 on failure (asset missing/mismatched or
 * placement overlap - the loader STOPs, it does not partially place). T1 ONLY
 * loads; it does NOT set registers or jump (that is T3). */
int TrampolineLoaderRun(void);

/* True once TrampolineLoaderRun() has succeeded (for T5's two-gate forge
 * predicate: if (!TrampolineLoaderRan()) { ...forge... }). */
bool TrampolineLoaderRan(void);
#endif /* TRAMPOLINE_LOADER_STANDALONE_TEST */

#endif /* TRAMPOLINE_LOADER_H */
