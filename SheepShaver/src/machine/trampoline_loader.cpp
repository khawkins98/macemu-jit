/*
 *  trampoline_loader.cpp - SS_M18 Stage 2b, T1: staged-asset MacOS.elf loader
 *
 *  See include/trampoline_loader.h for the contract + the asset/ROM manifest.
 *
 *  Two layers:
 *    (1) a pure ELF parse + place core (tramp_*), no emulator dependency,
 *        exercised standalone by test_trampoline_loader.cpp;
 *    (2) an emulator-facing wrapper (TrampolineLoader*), boot-latched behind
 *        SS_M18_TRAMPOLINE  MachineProfileIsNewWorld()  the 9.0.1-ROM-identity
 *        guard, that reads the staged asset and places it via Mac2HostAddr.
 *        Compiled out of the standalone test build.
 *
 *  T1 ONLY loads + zero-fills BSS. NO relocation off vaddrs (ET_EXEC; the PIC
 *  entry stub self-relocates - Stop-rule #6). Registers/jump are T3.
 */

#include "trampoline_loader.h"

#include <stdio.h>
#include <string.h>

/* ELF constants (32-bit big-endian PPC). */
#define ET_EXEC      2
#define EM_PPC       20
#define PT_LOAD      1
#define ELFCLASS32   1
#define ELFDATA2MSB  2

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int fail(char *err, size_t errsz, const char *msg)
{
	if (err && errsz) { strncpy(err, msg, errsz - 1); err[errsz - 1] = 0; }
	return -1;
}

uint64_t tramp_fnv1a64(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

int tramp_parse_elf(const uint8_t *elf, size_t size, TrampImage *out,
                    char *err, size_t errsz)
{
	if (!elf || !out)
		return fail(err, errsz, "null argument");
	if (size < 52)
		return fail(err, errsz, "file too small for ELF header");
	if (!(elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F'))
		return fail(err, errsz, "bad ELF magic");
	if (elf[4] != ELFCLASS32)
		return fail(err, errsz, "not ELFCLASS32");
	if (elf[5] != ELFDATA2MSB)
		return fail(err, errsz, "not big-endian (ELFDATA2MSB)");

	uint16_t e_type      = be16(elf + 16);
	uint16_t e_machine   = be16(elf + 18);
	uint32_t e_entry     = be32(elf + 24);
	uint32_t e_phoff     = be32(elf + 28);
	uint16_t e_phentsize = be16(elf + 42);
	uint16_t e_phnum     = be16(elf + 44);

	if (e_type != ET_EXEC)
		return fail(err, errsz, "not ET_EXEC");
	if (e_machine != EM_PPC)
		return fail(err, errsz, "not EM_PPC");
	if (e_phentsize < 32)
		return fail(err, errsz, "phentsize too small");

	memset(out, 0, sizeof(*out));
	out->entry = e_entry;

	for (uint16_t i = 0; i < e_phnum; i++) {
		size_t o = (size_t)e_phoff + (size_t)i * e_phentsize;
		if (o + 32 > size)
			return fail(err, errsz, "program header out of bounds");
		const uint8_t *ph = elf + o;
		uint32_t p_type   = be32(ph + 0);
		if (p_type != PT_LOAD)
			continue;
		if (out->n_seg >= TRAMP_MAX_SEG)
			return fail(err, errsz, "too many PT_LOAD segments");
		TrampSeg *s = &out->seg[out->n_seg++];
		s->off    = be32(ph + 4);
		s->vaddr  = be32(ph + 8);
		s->filesz = be32(ph + 16);
		s->memsz  = be32(ph + 20);
		s->flags  = be32(ph + 24);
		if ((size_t)s->off + s->filesz > size)
			return fail(err, errsz, "PT_LOAD filesz exceeds file");
		if (s->memsz < s->filesz)
			return fail(err, errsz, "PT_LOAD memsz < filesz");
	}
	if (out->n_seg == 0)
		return fail(err, errsz, "no PT_LOAD segments");
	return 0;
}

int tramp_place_image(const uint8_t *elf, size_t size, const TrampImage *img,
                      tramp_xlate_fn xlate, void *ctx, uint32_t aperture_top,
                      char *err, size_t errsz)
{
	if (!elf || !img || !xlate)
		return fail(err, errsz, "null argument");

	/* Pre-flight: every placed range must end at or below the aperture top.
	 * Refuse BEFORE writing anything (no partial placement on overflow). */
	for (int i = 0; i < img->n_seg; i++) {
		const TrampSeg *s = &img->seg[i];
		uint32_t end = s->vaddr + s->memsz;     /* memsz >= filesz, checked at parse */
		if (end < s->vaddr)                      /* wrap */
			return fail(err, errsz, "PT_LOAD vaddr+memsz wraps");
		if (end > aperture_top)
			return fail(err, errsz, "PT_LOAD exceeds guest RAM aperture");
		if ((size_t)s->off + s->filesz > size)
			return fail(err, errsz, "PT_LOAD filesz exceeds file");
	}

	for (int i = 0; i < img->n_seg; i++) {
		const TrampSeg *s = &img->seg[i];
		uint8_t *dst = xlate(s->vaddr, ctx);
		if (!dst)
			return fail(err, errsz, "guest translation returned NULL");
		if (s->filesz)
			memcpy(dst, elf + s->off, s->filesz);
		if (s->memsz > s->filesz)
			memset(dst + s->filesz, 0, s->memsz - s->filesz);   /* BSS tail */
	}
	return 0;
}

/* ================================================================== *
 *  Emulator-facing wrapper.                                          *
 * ================================================================== */

#ifndef TRAMPOLINE_LOADER_STANDALONE_TEST

#include <stdlib.h>

#include "sysdeps.h"
#include "cpu_emulation.h"      /* Mac2HostAddr, ROMBaseHost, RAMBase, RAMSize */
#include "machine_profile.h"    /* MachineProfileIsNewWorld */

static int  g_gate = -1;        /* boot-latched gate (-1 unresolved) */
static bool g_loader_ran = false;

static inline uint8_t *tramp_mac2host(uint32_t addr, void * /*ctx*/)
{
	return Mac2HostAddr(addr);
}

bool TrampolineLoaderGateEnabled(void)
{
	if (g_gate < 0) {
		const char *e = getenv("SS_M18_TRAMPOLINE");
		bool env_on = (e && e[0] && e[0] != '0');
		bool nw     = MachineProfileIsNewWorld();
		if (!(env_on && nw)) {
			g_gate = 0;                         /* default OFF: inert */
		} else {
			/* A-1 ROM-identity guard (Stop-rule #12): the staged ELF is the
			 * 9.0.1 Trampoline; require the live ROM to be the 9.0.1 (decoded
			 * checksum word at ROMBaseHost+0). Refuse the 1.1 ROM and any
			 * mismatch - do NOT launch the 9.0.1 ELF against wrong ROM bytes. */
			uint32_t rom_cksum = ((uint32_t)ROMBaseHost[0] << 24) |
			                     ((uint32_t)ROMBaseHost[1] << 16) |
			                     ((uint32_t)ROMBaseHost[2] << 8)  |
			                      (uint32_t)ROMBaseHost[3];
			if (rom_cksum != TRAMP_ROM_CHECKSUM_9_0_1) {
				fprintf(stderr,
				        "[S2B-ROM-MISMATCH] SS_M18_TRAMPOLINE requires the 9.0.1 ROM "
				        "(checksum %08x, file md5 %s); loaded ROM checksum=%08x - "
				        "REFUSING to launch (Stop-rule #12)\n",
				        TRAMP_ROM_CHECKSUM_9_0_1, TRAMP_ROM_FILE_MD5_9_0_1, rom_cksum);
				g_gate = 0;
			} else {
				g_gate = 1;
			}
		}
	}
	return g_gate != 0;
}

static uint8_t *read_asset(const char *path, size_t *out_len, char *err, size_t errsz)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fail(err, errsz, "cannot open asset"); return NULL; }
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fail(err, errsz, "seek failed"); return NULL; }
	long sz = ftell(f);
	if (sz < 0) { fclose(f); fail(err, errsz, "tell failed"); return NULL; }
	rewind(f);
	uint8_t *buf = (uint8_t *)malloc((size_t)sz);
	if (!buf) { fclose(f); fail(err, errsz, "out of memory"); return NULL; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (rd != (size_t)sz) { free(buf); fail(err, errsz, "short read"); return NULL; }
	*out_len = (size_t)sz;
	return buf;
}

int TrampolineLoaderRun(void)
{
	const char *path = getenv(TRAMP_ASSET_ENV);
	if (!path || !path[0])
		path = TRAMP_ASSET_DEFAULT_PATH;

	char err[160] = {0};
	size_t len = 0;
	uint8_t *asset = read_asset(path, &len, err, sizeof err);
	if (!asset) {
		fprintf(stderr, "[S2B-ASSET-MISSING] staged Trampoline asset '%s' (%s); "
		        "stage MacOS.elf (md5 %s) or set %s - REFUSING to launch\n",
		        path, err, TRAMP_ASSET_MD5, TRAMP_ASSET_ENV);
		return -1;
	}

	/* A-4 integrity: size + FNV-1a-64 must match the pinned manifest. */
	if (len != TRAMP_ASSET_SIZE || tramp_fnv1a64(asset, len) != TRAMP_ASSET_FNV1A64) {
		fprintf(stderr, "[S2B-ASSET-MISMATCH] '%s' size=%zu (want %u) - identity does "
		        "not match the pinned 9.0.1 MacOS.elf (md5 %s) - REFUSING to launch\n",
		        path, len, TRAMP_ASSET_SIZE, TRAMP_ASSET_MD5);
		free(asset);
		return -1;
	}

	TrampImage img;
	if (tramp_parse_elf(asset, len, &img, err, sizeof err) < 0) {
		fprintf(stderr, "[S2B-ASSET-MISMATCH] '%s' ELF parse failed: %s - REFUSING\n", path, err);
		free(asset);
		return -1;
	}

	/* The placed image must fit below the top of guest RAM. */
	uint32_t aperture_top = RAMBase + RAMSize;
	if (TRAMP_APERTURE_TOP > RAMSize) {
		fprintf(stderr, "[S2B-LOADER] guest RAM (%u bytes) too small for the Trampoline "
		        "image top 0x%x - REFUSING\n", RAMSize, TRAMP_APERTURE_TOP);
		free(asset);
		return -1;
	}
	/* Placement is at absolute guest addresses (data 0x100000 / exec 0x200000),
	 * which live in low RAM. Aperture check uses RAMSize as the high bound. */
	if (tramp_place_image(asset, len, &img, tramp_mac2host, NULL, RAMSize, err, sizeof err) < 0) {
		fprintf(stderr, "[S2B-LOADER] placement refused: %s (Stop-rule #6 - no relocation)\n", err);
		free(asset);
		return -1;
	}
	(void)aperture_top;

	free(asset);
	g_loader_ran = true;
	fprintf(stderr, "[S2B-LOADER] placed %d PT_LOAD from '%s' (entry=0x%x): "
	        "data vaddr=0x%x filesz=0x%x memsz=0x%x; exec vaddr=0x%x filesz=0x%x\n",
	        img.n_seg, path, img.entry,
	        TRAMP_DATA_VADDR, TRAMP_DATA_FILESZ, TRAMP_DATA_MEMSZ,
	        TRAMP_EXEC_VADDR, TRAMP_EXEC_FILESZ);
	return 0;
}

bool TrampolineLoaderRan(void)
{
	return g_loader_ran;
}

/* ================================================================== *
 *  BootX pre-stage: stage the 4MB Mac OS ROM image into guest RAM.   *
 * ================================================================== */

static uint32_t g_rom_virt    = 0;
static uint32_t g_parcel_size = 0;

uint32_t TrampolineRomVirt(void)    { return g_rom_virt; }
uint32_t TrampolineParcelSize(void) { return g_parcel_size; }

int TrampolineStageParcels(void)
{
	/* rom_virt: 4MB-aligned guest-physical base (env-overridable). */
	uint32_t rom_virt = TRAMP_ROM_VIRT_DEFAULT;
	if (const char *e = getenv("SS_M18_ROM_VIRT"))
		rom_virt = (uint32_t)strtoul(e, NULL, 0);
	if (rom_virt & (0x400000u - 1)) {
		fprintf(stderr, "[S2B-PARCEL] rom_virt 0x%08x is not 4MB-aligned - REFUSING\n", rom_virt);
		return -1;
	}

	/* Source selection (PHASE-0 verdict).
	 *  - Default: the DECOMPRESSED 4MB image already in SheepShaver's ROM
	 *    aperture at guest 0x50000000 (ROMBaseHost). NewWorld .rom files ship
	 *    compressed, but SheepShaver decompresses into the aperture at load, so
	 *    ConfigInfo is in-place at +0x30D000. The Trampoline's AAPL,toolbox-parcels
	 *    reader reads ConfigInfo from this image (KernelCodeOffset @ +0x4C =>
	 *    NanoKernelEntry = rom_virt + 0x310000).
	 *  - Fallback: SS_M18_PARCEL_FILE overrides the source with a staged file
	 *    (e.g. the compressed 'prcl' Parcels container) if the headline boot shows
	 *    the reader decompresses unconditionally (RT #3 risk). */
	const char *parcel_file = getenv("SS_M18_PARCEL_FILE");
	uint8_t *src = NULL;
	size_t   src_len = 0;
	uint8_t *src_owned = NULL;

	if (parcel_file && parcel_file[0]) {
		char ferr[160] = {0};
		src_owned = read_asset(parcel_file, &src_len, ferr, sizeof ferr);
		if (!src_owned) {
			fprintf(stderr, "[S2B-PARCEL] SS_M18_PARCEL_FILE '%s' unreadable (%s) - REFUSING\n",
			        parcel_file, ferr);
			return -1;
		}
		src = src_owned;
	} else {
		src     = ROMBaseHost;
		src_len = TRAMP_PARCEL_SIZE;
	}

	uint32_t size = (uint32_t)src_len;
	if (size > TRAMP_PARCEL_SIZE) size = TRAMP_PARCEL_SIZE;  /* aperture cap */

	/* PHASE-0 runtime verification: the aperture must hold the DECOMPRESSED image
	 * (ConfigInfo ROMImageBaseOffset @ +0x30D028 == 0xFFCF3000). Verify against the
	 * aperture regardless of source (the aperture is always the decompressed ROM). */
	uint32_t cfg = ((uint32_t)ROMBaseHost[0x30D028] << 24) |
	               ((uint32_t)ROMBaseHost[0x30D029] << 16) |
	               ((uint32_t)ROMBaseHost[0x30D02A] << 8)  |
	                (uint32_t)ROMBaseHost[0x30D02B];
	fprintf(stderr, "[S2B-PARCEL] PHASE-0 ConfigInfo probe: aperture[0x5030D028]=0x%08x "
	        "(expect 0xFFCF3000 = decompressed image present)\n", cfg);
	if (cfg != 0xFFCF3000u)
		fprintf(stderr, "[S2B-PARCEL] WARNING: ConfigInfo mismatch - the ROM aperture may "
		        "not be the decompressed 9.0.1 image; NanoKernelEntry may be garbage\n");

	/* Bounds: [rom_virt, rom_virt+size) must fit below RAMSize and not overlap the
	 * loaded MacOS.elf / shim (tops ~0x211000). */
	uint64_t end = (uint64_t)rom_virt + size;
	if (end > RAMSize) {
		fprintf(stderr, "[S2B-PARCEL] rom_virt+size 0x%llx exceeds RAMSize 0x%x - REFUSING\n",
		        (unsigned long long)end, RAMSize);
		if (src_owned) free(src_owned);
		return -1;
	}
	if (rom_virt < 0x00220000u) {
		fprintf(stderr, "[S2B-PARCEL] rom_virt 0x%08x overlaps the loaded MacOS.elf/shim "
		        "region (<0x220000) - REFUSING\n", rom_virt);
		if (src_owned) free(src_owned);
		return -1;
	}

	uint8_t *dst = Mac2HostAddr(rom_virt);
	memcpy(dst, src, size);

	g_rom_virt    = rom_virt;
	g_parcel_size = size;
	if (src_owned) free(src_owned);

	/* NanoKernelEntry the Trampoline will derive (KernelCodeOffset @ ConfigInfo+0x4C). */
	uint32_t nk_entry = rom_virt + 0x310000u;
	fprintf(stderr, "[S2B-PARCEL] staged %u KB ROM image into guest [0x%08x,0x%08x) from %s; "
	        "AAPL,toolbox-parcels=(0x%08x,0x%08x); NanoKernelEntry=0x%08x\n",
	        size / 1024, rom_virt, rom_virt + size,
	        (parcel_file && parcel_file[0]) ? parcel_file : "ROM aperture 0x50000000",
	        rom_virt, size, nk_entry);
	return 0;
}

#endif /* TRAMPOLINE_LOADER_STANDALONE_TEST */
