/*
 *  test_trampoline_loader.cpp - SS_M18 Stage 2b, T1 G-gate (placement unit).
 *
 *  Standalone (no kpx_cpu / no emulator link; mirrors test_openfirmware_ci).
 *  Loads the real staged MacOS.elf, exercises the pure parse + place core
 *  against a flat mock guest-RAM buffer, and asserts:
 *    - the 2 PT_LOAD land at vaddr 0x100000 (data) / 0x200000 (exec);
 *    - segment filesz/memsz match the pinned manifest, entry = 0x20f078;
 *    - placed file bytes are byte-identical to the asset;
 *    - the data BSS tail (0x19920 - 0x6bc0) is zero-filled;
 *    - the asset FNV-1a-64 matches the pinned manifest (A-4 integrity);
 *    - placement REFUSES when the aperture cannot hold the image (Stop-rule #6).
 *
 *  This is the placement half of G2b.a; the PIC-stub-runs half is owed to T3
 *  launch + the post-S1 boot.
 */

#ifndef TRAMPOLINE_LOADER_STANDALONE_TEST
#define TRAMPOLINE_LOADER_STANDALONE_TEST
#endif
#include "trampoline_loader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); abort(); } n_pass++; } while (0)

/* Mock guest aperture: guest addr 0 maps to g_ram[0]. */
static uint8_t *g_ram = NULL;
static uint8_t *mock_xlate(uint32_t addr, void * /*ctx*/) { return g_ram + addr; }

static uint8_t *load_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	uint8_t *b = (uint8_t *)malloc((size_t)sz);
	size_t rd = fread(b, 1, (size_t)sz, f);
	fclose(f);
	if (rd != (size_t)sz) { free(b); return NULL; }
	*out_len = (size_t)sz;
	return b;
}

int main(void)
{
	const char *path = getenv(TRAMP_ASSET_ENV);
	if (!path || !path[0]) path = TRAMP_ASSET_DEFAULT_PATH;

	size_t len = 0;
	uint8_t *asset = load_file(path, &len);
	if (!asset) {
		fprintf(stderr, "[SKIP-LOUD] staged asset '%s' absent. Set %s or stage "
		        "MacOS.elf (md5 %s). This is the A-4 residue (asset not committed).\n",
		        path, TRAMP_ASSET_ENV, TRAMP_ASSET_MD5);
		/* FAIL LOUD: T1 G-gate requires the asset. Non-zero exit. */
		return 2;
	}

	/* A-4 integrity. */
	CHECK(len == TRAMP_ASSET_SIZE);
	CHECK(tramp_fnv1a64(asset, len) == TRAMP_ASSET_FNV1A64);

	/* Parse. */
	char err[160] = {0};
	TrampImage img;
	CHECK(tramp_parse_elf(asset, len, &img, err, sizeof err) == 0);
	CHECK(img.entry == TRAMP_ENTRY);
	CHECK(img.n_seg == 2);

	/* Identify the data + exec segments by vaddr (file order is exec-first). */
	const TrampSeg *data = NULL, *exec = NULL;
	for (int i = 0; i < img.n_seg; i++) {
		if (img.seg[i].vaddr == TRAMP_DATA_VADDR) data = &img.seg[i];
		if (img.seg[i].vaddr == TRAMP_EXEC_VADDR) exec = &img.seg[i];
	}
	CHECK(data != NULL);
	CHECK(exec != NULL);
	CHECK(data->filesz == TRAMP_DATA_FILESZ);
	CHECK(data->memsz  == TRAMP_DATA_MEMSZ);
	CHECK(data->flags  == 6);  /* PF_R|PF_W */
	CHECK(exec->filesz == TRAMP_EXEC_FILESZ);
	CHECK(exec->memsz  == TRAMP_EXEC_FILESZ);
	CHECK(exec->flags  == 5);  /* PF_R|PF_X */

	/* Place into a poisoned mock aperture (0xAA) to prove BSS is actively zeroed. */
	size_t ram_size = TRAMP_APERTURE_TOP + 0x1000;
	g_ram = (uint8_t *)malloc(ram_size);
	memset(g_ram, 0xAA, ram_size);
	CHECK(tramp_place_image(asset, len, &img, mock_xlate, NULL,
	                        (uint32_t)ram_size, err, sizeof err) == 0);

	/* Data segment: file bytes byte-identical, BSS tail zeroed. */
	CHECK(memcmp(g_ram + TRAMP_DATA_VADDR, asset + data->off, data->filesz) == 0);
	for (uint32_t i = TRAMP_DATA_FILESZ; i < TRAMP_DATA_MEMSZ; i++)
		CHECK(g_ram[TRAMP_DATA_VADDR + i] == 0x00);
	/* Byte just past the data memsz must be untouched poison (no overrun). */
	CHECK(g_ram[TRAMP_DATA_VADDR + TRAMP_DATA_MEMSZ] == 0xAA);

	/* Exec segment: byte-identical, no BSS (memsz == filesz). */
	CHECK(memcmp(g_ram + TRAMP_EXEC_VADDR, asset + exec->off, exec->filesz) == 0);
	CHECK(g_ram[TRAMP_EXEC_VADDR + TRAMP_EXEC_FILESZ] == 0xAA);

	/* Stop-rule #6: refuse rather than relocate when the aperture is too small. */
	char err2[160] = {0};
	CHECK(tramp_place_image(asset, len, &img, mock_xlate, NULL,
	                        TRAMP_EXEC_VADDR, err2, sizeof err2) < 0);

	free(g_ram);
	free(asset);
	printf("test_trampoline_loader: %d checks passed (asset '%s')\n", n_pass, path);
	return 0;
}
