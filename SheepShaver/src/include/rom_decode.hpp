/*
 *  rom_decode.hpp - Reusable Mac ROM image decoder + type detection
 *
 *  Extracted from rom_patches.cpp so that standalone tools (rom-inspect) and the
 *  emulator share one decode path.  Header-only and self-contained: depends only
 *  on <stdint.h> + <arpa/inet.h>, with no SheepShaver runtime globals.  The
 *  emulator's DecodeROM() wraps decode_rom_image() with ROMBaseHost/ROM_SIZE.
 *
 *  Handles three on-disk ROM formats:
 *    - raw 4 MB plain image
 *    - CHRP container, LZSS-compressed payload
 *    - CHRP container, parcels payload (each 'rom ' parcel is itself LZSS)
 *
 *  rom_detect_type() returns the ROM type by matching the nanokernel ID string at
 *  decoded offset 0x30d064.  The returned int matches the ROMTYPE_* enum ordering
 *  in rom_patches.h (TNT=0 .. NEWWORLD=5), or -1 if unrecognized.
 *
 *  See docs/superpowers/specs/2026-06-03-rom-inspector-design.md
 */

#ifndef ROM_DECODE_HPP
#define ROM_DECODE_HPP

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>   /* ntohl */

#ifndef FOURCC
#define FOURCC(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#endif

/* Size of the decoded ROM image. Must match cpu_emulation.h's ROM_SIZE. */
#ifndef ROM_DECODE_SIZE
#define ROM_DECODE_SIZE 0x400000
#endif

/* Offset of the nanokernel ID string within the decoded ROM image. */
#define ROM_NANOKERNEL_ID_OFFSET 0x30d064

/* Decode LZSS data (verbatim from rom_patches.cpp). */
static inline void decode_lzss(const uint8_t *src, uint8_t *dest, int size)
{
	char dict[0x1000];
	int run_mask = 0, dict_idx = 0xfee;
	for (;;) {
		if (run_mask < 0x100) {
			// Start new run
			if (--size < 0)
				break;
			run_mask = *src++ | 0xff00;
		}
		bool bit = run_mask & 1;
		run_mask >>= 1;
		if (bit) {
			// Verbatim copy
			if (--size < 0)
				break;
			int c = *src++;
			dict[dict_idx++] = c;
			*dest++ = c;
			dict_idx &= 0xfff;
		} else {
			// Copy from dictionary
			if (--size < 0)
				break;
			int idx = *src++;
			if (--size < 0)
				break;
			int cnt = *src++;
			idx |= (cnt << 4) & 0xf00;
			cnt = (cnt & 0x0f) + 3;
			while (cnt--) {
				char c = dict[idx++];
				dict[dict_idx++] = c;
				*dest++ = c;
				idx &= 0xfff;
				dict_idx &= 0xfff;
			}
		}
	}
}

/* Decode parcels of ROM image (MacOS 9.X and even earlier).
 * Verbatim from rom_patches.cpp, with debug prints removed. */
static inline void decode_parcels(const uint8_t *src, uint8_t *dest, int size)
{
	uint32_t parcel_offset = 0x14;
	while (parcel_offset != 0) {
		const uint32_t *parcel_data = (const uint32_t *)(src + parcel_offset);
		uint32_t next_offset = ntohl(parcel_data[0]);
		uint32_t parcel_type = ntohl(parcel_data[1]);
		if (parcel_type == FOURCC('r','o','m',' ')) {
			uint32_t lzss_offset  = ntohl(parcel_data[2]);
			uint32_t lzss_size = ((uintptr_t)src + next_offset) - ((uintptr_t)parcel_data + lzss_offset);
			decode_lzss((const uint8_t *)parcel_data + lzss_offset, dest, lzss_size);
		}
		parcel_offset = next_offset;
	}
}

/* On-disk ROM format, for reporting. */
enum rom_format {
	ROM_FMT_RAW = 0,        /* plain 4 MB image */
	ROM_FMT_CHRP_LZSS,      /* CHRP container, LZSS payload */
	ROM_FMT_CHRP_PARCELS,   /* CHRP container, parcels payload */
	ROM_FMT_UNKNOWN = -1
};

/* Decode a ROM image of `size` bytes from `data` into `dest` (`dest_size` bytes).
 * Generalized DecodeROM(): the emulator passes ROMBaseHost / ROM_SIZE.
 * If `out_fmt` is non-NULL it receives the detected on-disk format.
 * Returns true on success. */
static inline bool decode_rom_image(const uint8_t *data, uint32_t size,
                                    uint8_t *dest, uint32_t dest_size,
                                    int *out_fmt)
{
	if (out_fmt) *out_fmt = ROM_FMT_UNKNOWN;

	if (size == dest_size) {
		// Plain ROM image
		memcpy(dest, data, dest_size);
		if (out_fmt) *out_fmt = ROM_FMT_RAW;
		return true;
	}
	else if (size >= 11 && strncmp((const char *)data, "<CHRP-BOOT>", 11) == 0) {
		// CHRP compressed ROM image. Work on a NUL-terminated copy so string
		// searches cannot run past short or malformed ROM files.
		char *rom_text = new char[size + 1];
		memcpy(rom_text, data, size);
		rom_text[size] = '\0';

		uint32_t image_offset = 0, image_size = 0;
		bool decode_info_ok = false;

		char *s = strstr(rom_text, "constant lzss-offset");
		if (s != NULL) {
			// Probably a plain LZSS compressed ROM image
			if (s >= rom_text + 7 && sscanf(s - 7, "%06x", &image_offset) == 1) {
				s = strstr(rom_text, "constant lzss-size");
				if (s != NULL && s >= rom_text + 7 && (sscanf(s - 7, "%06x", &image_size) == 1))
					decode_info_ok = true;
			}
		}
		else {
			// Probably a MacOS 9.2.x ROM image
			s = strstr(rom_text, "constant parcels-offset");
			if (s != NULL) {
				if (s >= rom_text + 7 && sscanf(s - 7, "%06x", &image_offset) == 1) {
					s = strstr(rom_text, "constant parcels-size");
					if (s != NULL && s >= rom_text + 7 && (sscanf(s - 7, "%06x", &image_size) == 1))
						decode_info_ok = true;
				}
			}
		}
		delete[] rom_text;

		// No valid information to decode the ROM found, or the embedded image
		// points outside the bytes that were actually read?
		if (!decode_info_ok || image_offset > size || image_size > size - image_offset || image_size < 4)
			return false;

		// Check signature, this could be a parcels-based ROM image
		uint32_t rom_signature;
		memcpy(&rom_signature, data + image_offset, sizeof(rom_signature));
		rom_signature = ntohl(rom_signature);
		if (rom_signature == FOURCC('p','r','c','l')) {
			decode_parcels(data + image_offset, dest, image_size);
			if (out_fmt) *out_fmt = ROM_FMT_CHRP_PARCELS;
		}
		else {
			decode_lzss(data + image_offset, dest, image_size);
			if (out_fmt) *out_fmt = ROM_FMT_CHRP_LZSS;
		}
		return true;
	}
	return false;
}

/* Detect ROM type from the nanokernel ID string in a decoded image.
 * Returns -1 if unrecognized, else 0..5 matching rom_patches.h ROMTYPE_* ordering. */
static inline int rom_detect_type(const uint8_t *decoded)
{
	const char *id = (const char *)decoded + ROM_NANOKERNEL_ID_OFFSET;
	if (!memcmp(id, "Boot TNT", 8))           return 0;  /* ROMTYPE_TNT */
	if (!memcmp(id, "Boot Alchemy", 12))      return 1;  /* ROMTYPE_ALCHEMY */
	if (!memcmp(id, "Boot Zanzibar", 13))     return 2;  /* ROMTYPE_ZANZIBAR */
	if (!memcmp(id, "Boot Gazelle", 12))      return 3;  /* ROMTYPE_GAZELLE */
	if (!memcmp(id, "Boot Gossamer", 13))     return 4;  /* ROMTYPE_GOSSAMER */
	if (!memcmp(id, "NewWorld", 8))           return 5;  /* ROMTYPE_NEWWORLD */
	return -1;
}

static inline const char *rom_type_name(int type)
{
	switch (type) {
	case 0: return "TNT";
	case 1: return "Alchemy";
	case 2: return "Zanzibar";
	case 3: return "Gazelle";
	case 4: return "Gossamer";
	case 5: return "NewWorld";
	default: return "UNKNOWN";
	}
}

static inline const char *rom_format_name(int fmt)
{
	switch (fmt) {
	case ROM_FMT_RAW:          return "raw 4MB image";
	case ROM_FMT_CHRP_LZSS:    return "CHRP container, LZSS payload";
	case ROM_FMT_CHRP_PARCELS: return "CHRP container, parcels payload";
	default:                   return "unknown";
	}
}

#endif /* ROM_DECODE_HPP */
