/*
 *  rom-inspect.cpp - Standalone Mac ROM inspector for SheepShaver
 *
 *  Reports, for any ROM file, whether SheepShaver's DecodeROM()/PatchROM() would
 *  accept it — WITHOUT launching the emulator.  Shares the exact decode + type-
 *  detection code with the emulator via rom_decode.hpp, so its verdict matches
 *  what the real PatchROM() type-detection would conclude.
 *
 *  Usage:  rom-inspect <rom-file>
 *  Exit:   0 if PatchROM would accept the ROM, 1 if it would reject it,
 *          2 on usage/IO error.
 *
 *  See docs/superpowers/specs/2026-06-03-rom-inspector-design.md
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "rom_decode.hpp"

// Render the (possibly non-printable) nanokernel ID bytes for display.
static void print_printable(const uint8_t *p, int n)
{
	for (int i = 0; i < n; i++) {
		int c = p[i];
		putchar(isprint(c) ? c : '.');
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <rom-file>\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];

	// Read the whole file (NOT capped at 4MB — main_unix.cpp caps its read at
	// ROM_SIZE, which would truncate a CHRP file > 4MB; reading in full here is
	// more correct and lets us flag such cases).
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return 2; }
	struct stat st;
	if (stat(path, &st) != 0) { fprintf(stderr, "error: cannot stat %s\n", path); fclose(f); return 2; }
	long fsize = (long)st.st_size;
	if (fsize <= 0) { fprintf(stderr, "error: empty or unreadable %s\n", path); fclose(f); return 2; }

	uint8_t *data = (uint8_t *)malloc(fsize);
	if (!data) { fprintf(stderr, "error: out of memory\n"); fclose(f); return 2; }
	long got = (long)fread(data, 1, fsize, f);
	fclose(f);
	if (got != fsize) { fprintf(stderr, "error: short read (%ld/%ld)\n", got, fsize); free(data); return 2; }

	printf("ROM file:    %s\n", path);
	printf("File size:   %ld bytes (%.2f MB)\n", fsize, fsize / (1024.0 * 1024.0));

	// Decode into a ROM_DECODE_SIZE buffer, exactly as the emulator would.
	uint8_t *decoded = (uint8_t *)calloc(1, ROM_DECODE_SIZE);
	if (!decoded) { fprintf(stderr, "error: out of memory\n"); free(data); return 2; }

	int fmt = ROM_FMT_UNKNOWN;
	bool ok = decode_rom_image(data, (uint32_t)fsize, decoded, ROM_DECODE_SIZE, &fmt);
	free(data);

	if (!ok) {
		printf("Format:      unrecognized (not raw 4MB, not a decodable CHRP image)\n");
		printf("Decode:      FAILED\n");
		printf("Verdict:     UNSUPPORTED — DecodeROM() would return false\n");
		free(decoded);
		return 1;
	}

	printf("Format:      %s\n", rom_format_name(fmt));
	printf("Decode:      ok (%d bytes decoded image)\n", ROM_DECODE_SIZE);

	const uint8_t *id = decoded + ROM_NANOKERNEL_ID_OFFSET;
	printf("Nanokernel ID @0x%x: \"", ROM_NANOKERNEL_ID_OFFSET);
	print_printable(id, 16);
	printf("\"\n");

	int type = rom_detect_type(decoded);
	if (type < 0) {
		printf("ROM type:    UNKNOWN\n");
		printf("Verdict:     REJECTED at type-detection — no known nanokernel ID at 0x%x.\n", ROM_NANOKERNEL_ID_OFFSET);
		printf("             PatchROM() returns false here → emulator shows \"Unsupported ROM type\".\n");
		free(decoded);
		return 1;
	}

	printf("ROM type:    %s (ROMTYPE index %d)\n", rom_type_name(type), type);
	printf("Type-detect: PASS — PatchROM() recognizes this ROM type.\n");
	printf("Verdict:     decode + type-detection OK.\n");
	printf("\n");
	printf("  NOTE: this tool models DecodeROM() and PatchROM()'s TYPE DETECTION only.\n");
	printf("  PatchROM() then runs patch-space checks and byte-pattern searches\n");
	printf("  (patch_nanokernel_boot/_68k_emul/_nanokernel/_68k). A type-recognized\n");
	printf("  ROM can STILL be rejected there — and the emulator shows the same\n");
	printf("  \"Unsupported ROM type\" alert for ANY PatchROM() failure (main.cpp:162).\n");
	printf("  So 'type-detection OK' means 'not rejected for format/type', NOT\n");
	printf("  'guaranteed to boot'.\n");
	free(decoded);
	return 0;
}
