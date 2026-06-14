/*
 *  rom_patches.cpp - ROM patches
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * TODO:
 *  IRQ_NEST must be handled atomically
 *  Don't use r1 in extra routines
 */

#include <string.h>

#include "sysdeps.h"
#include "rom_patches.h"
#include "main.h"
#include "prefs.h"
#include "machine_profile.h"
#include "exc_core.h"		// M8 Task A: ExcRiserWindow (trap_return window export)
#include "cpu_emulation.h"
#include "emul_op.h"
#include "xlowmem.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "audio.h"
#include "audio_defs.h"
#include "serial.h"
#include "macos_util.h"
#include "thunks.h"
#include "rom_decode.hpp"	// decode_lzss/decode_parcels/decode_rom_image/rom_detect_type

#define DEBUG 0
#include "debug.h"


// 68k breakpoint address
//#define M68K_BREAK_POINT 0x29e0		// BootMe
//#define M68K_BREAK_POINT 0x2a1e		// Boot block code returned
//#define M68K_BREAK_POINT 0x3150		// CritError
//#define M68K_BREAK_POINT 0x187ce		// Unimplemented trap

// PowerPC breakpoint address
//#define POWERPC_BREAK_POINT 0x36e6c0	// 68k emulator start

#define DISABLE_SCSI 1


// Other ROM addresses
const uint32 CHECK_LOAD_PATCH_SPACE = 0x2fcf00;
const uint32 ZERO_SCRAP_PATCH_SPACE = 0x2fcf80;
const uint32 PUT_SCRAP_PATCH_SPACE = 0x2fcfc0;
const uint32 GET_SCRAP_PATCH_SPACE = 0x2fd100;
const uint32 ADDR_MAP_PATCH_SPACE = 0x2fd140;
const uint32 TIME_MANAGER_PATCH_SPACE = 0x2fd240;	// 4 TM EMUL_OP stub bodies (SS_NW_TM_TRAPS; checked at the site, not in PatchROM's global check, so non-newworld ROMs are unaffected)
const uint32 IRQ_POST_PATCH_SPACE = 0x2fd280;	// M8 Task B (Q-C3): from-emulator post-leg deferred-pair staging stub (SS_NW_IRQ_CONSUME; checked at the site)

// Global variables
int ROMType;				// ROM type
static uint32 sony_offset;	// Offset of .Sony driver resource

// M8 slot-4 consumption Task A (2026-06-12 plan, coordinator ACK note 1):
// the trap_return riser/reload windows for the deferred-EE-edge rfi-atomicity
// emulation (ppc-execute.cpp/ppc-cpu.cpp). SINGLE SOURCE OF TRUTH: filled at
// the trap_return patch site below from the very values that patch emits —
// no hand-copied magic ranges anywhere else. All-zero (e.g. trap_return
// skipped, or pre-patch) = empty windows = the latch is dead.
ExcRiserWindow g_exc_riser_window = { 0, 0, 0, 0, 0 };

// Prototypes
static bool patch_nanokernel_boot(void);
static bool patch_68k_emul(void);
static bool patch_nanokernel(void);
static bool patch_68k(void);


/*
 *  Decode ROM image, 4 MB plain images or NewWorld images
 *
 *  The decode logic (LZSS, parcels, CHRP container parsing) now lives in the
 *  reusable header rom_decode.hpp so that standalone tools (rom-inspect) share
 *  one code path.  This wrapper just supplies the emulator's ROMBaseHost/ROM_SIZE.
 */

bool DecodeROM(uint8 *data, uint32 size)
{
	return decode_rom_image(data, size, ROMBaseHost, ROM_SIZE, NULL);
}


/*
 *  ROM-version discrimination for multi-version NewWorld patching.
 *
 *  The byte-pattern patches below are calibrated to the 1998 LZSS "Mac OS ROM 1.1"
 *  (ROMTYPE_NEWWORLD, checksum 0xfd86d120). Newer NewWorld ROMs (e.g. the G4-era 9.0.4
 *  parcels ROM, checksum 0xb8d0b672, which carries the 'ppcf' AltiVec gestalt) share the
 *  same ROMTYPE but have a drifted/rewritten layout. `g_rom_904_lenient` enables a
 *  best-effort port path for that ROM WITHOUT disturbing the byte-identical 1.1 path:
 *    - find_rom_data falls back to a whole-image search (handles RELOCATED patterns), and
 *    - patch sites may treat a miss as skip-with-warning (handles ABSENT patterns).
 *  See docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md (Phase 2),
 *  docs/planning/PATCH-68K-SHIM-INVENTORY.md (per-pattern status table),
 *  and SheepShaver/docs/DIAGNOSTICS.md ("ROM patching diagnostics") for usage.
 */
static bool g_rom_904_lenient = false;

/*
 *  Search ROM for byte string, return ROM offset (or 0)
 */

static uint32 find_rom_data(uint32 start, uint32 end, const uint8 *data, uint32 data_len)
{
	const bool trace = getenv("SS_ROM_PATCH_TRACE") != NULL;
	uint32 ofs = start;
	while (ofs < end) {
		if (!memcmp(ROMBaseHost + ofs, data, data_len)) {
			if (trace)
				fprintf(stderr, "[ROMPATCH] find_rom_data [%06x,%06x) len=%u pat=%02x%02x%02x%02x -> HIT @%06x\n",
				        start, end, data_len, data[0], data_len>1?data[1]:0, data_len>2?data[2]:0, data_len>3?data[3]:0, ofs);
			return ofs;
		}
		ofs++;
	}
	// 9.0.4 lenient: pattern may have RELOCATED — retry across the whole image.
	if (g_rom_904_lenient) {
		for (uint32 o = 0; o + data_len <= ROM_SIZE; o++) {
			if (!memcmp(ROMBaseHost + o, data, data_len)) {
				if (trace)
					fprintf(stderr, "[ROMPATCH] find_rom_data [%06x,%06x) pat=%02x%02x%02x%02x -> RELOCATED @%06x (904 whole-image fallback)\n",
					        start, end, data[0], data_len>1?data[1]:0, data_len>2?data[2]:0, data_len>3?data[3]:0, o);
				return o;
			}
		}
	}
	if (trace)
		fprintf(stderr, "[ROMPATCH] find_rom_data [%06x,%06x) len=%u pat=%02x%02x%02x%02x -> MISS%s\n",
		        start, end, data_len, data[0], data_len>1?data[1]:0, data_len>2?data[2]:0, data_len>3?data[3]:0,
		        g_rom_904_lenient ? " (absent even whole-image)" : " (abort point)");
	return 0;
}


/*
 *  Search ROM resource by type/ID, return ROM offset of resource data
 */

static uint32 rsrc_ptr = 0;

// id = 4711 means "find any ID"
static uint32 find_rom_resource(uint32 s_type, int16 s_id = 4711, bool cont = false)
{
	uint32 lp = ROMBase + 0x1a;
	uint32 x = ReadMacInt32(lp);
	uint32 header_size = ReadMacInt8(ROMBase + x + 5);

	if (!cont)
		rsrc_ptr = x;
	else if (rsrc_ptr == 0)
		return 0;

	for (;;) {
		lp = ROMBase + rsrc_ptr;
		rsrc_ptr = ReadMacInt32(lp);
		if (rsrc_ptr == 0)
			break;

		rsrc_ptr += header_size;

		lp = ROMBase + rsrc_ptr + 4;
		uint32 data = ReadMacInt32(lp);
		uint32 type = ReadMacInt32(lp + 4);
		int16 id = ReadMacInt16(lp + 8);
		if (type == s_type && (id == s_id || s_id == 4711))
			return data;
	}
	return 0;
}


/*
 *  Search offset of A-Trap routine in ROM
 */

static uint32 find_rom_trap(uint16 trap)
{
	uint32 lp = ROMBase + ReadMacInt32(ROMBase + 0x22);

	if (trap > 0xa800)
		return ReadMacInt32(lp + 4 * (trap & 0x3ff));
	else
		return ReadMacInt32(lp + 4 * ((trap & 0xff) + 0x400));
}


/*
 *  Return target of branch instruction specified at ADDR, or 0 if
 *  there is no such instruction
 */

static uint32 rom_powerpc_branch_target(uint32 addr)
{
	uint32 opcode = ntohl(*(uint32 *)(ROMBaseHost + addr));
	uint32 primop = opcode >> 26;
	uint32 target = 0;

	if (primop == 18) {			// Branch
		target = opcode & 0x3fffffc;
		if (target & 0x2000000)
			target |= 0xfc000000;
		if ((opcode & 2) == 0)
			target += addr;
	}
	else if (primop == 16) {	// Branch Conditional
		target = (int32)(int16)(opcode & 0xfffc);
		if ((opcode & 2) == 0)
			target += addr;
	}
	return target;
}


/*
 *  Search ROM for instruction branching to target address, return 0 if none found
 */

static uint32 find_rom_powerpc_branch(uint32 start, uint32 end, uint32 target)
{
	for (uint32 addr = start; addr < end; addr += 4) {
		if (rom_powerpc_branch_target(addr) == target)
			return addr;
	}
	return 0;
}


/*
 *  Check that requested ROM patch space is really available
 */

static bool check_rom_patch_space(uint32 base, uint32 size)
{
	size = (size + 3) & -4;
	for (int i = 0; i < size; i += 4) {
		uint32 x = ntohl(*(uint32 *)(ROMBaseHost + base + i));
		if (x != 0x6b636b63 && x != 0)
			return false;
	}
	return true;
}


/*
 *  List of audio sifters installed in ROM and System file
 */

struct sift_entry {
	uint32 type;
	int16 id;
};
static sift_entry sifter_list[32];
static int num_sifters;

void AddSifter(uint32 type, int16 id)
{
	if (FindSifter(type, id))
		return;
	D(bug(" adding sifter type %c%c%c%c (%08x), id %d\n", type >> 24, (type >> 16) & 0xff, (type >> 8) & 0xff, type & 0xff, type, id));
	sifter_list[num_sifters].type = type;
	sifter_list[num_sifters].id = id;
	num_sifters++;
}

bool FindSifter(uint32 type, int16 id)
{
	for (int i=0; i<num_sifters; i++) {
		if (sifter_list[i].type == type && sifter_list[i].id == id)
			return true;
	}
	return false;
}


/*
 *  Driver stubs
 */

static const uint8 sony_driver[] = {	// Replacement for .Sony driver
	// Driver header
	SonyDriverFlags >> 8, SonyDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x18,							// Open() offset
	0x00, 0x1c,							// Prime() offset
	0x00, 0x20,							// Control() offset
	0x00, 0x2c,							// Status() offset
	0x00, 0x52,							// Close() offset
	0x05, 0x2e, 0x53, 0x6f, 0x6e, 0x79,	// ".Sony"

	// Open()
	M68K_EMUL_OP_SONY_OPEN >> 8, M68K_EMUL_OP_SONY_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_SONY_PRIME >> 8, M68K_EMUL_OP_SONY_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_SONY_CONTROL >> 8, M68K_EMUL_OP_SONY_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_SONY_STATUS >> 8, M68K_EMUL_OP_SONY_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};

static const uint8 disk_driver[] = {	// Generic disk driver
	// Driver header
	DiskDriverFlags >> 8, DiskDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x18,							// Open() offset
	0x00, 0x1c,							// Prime() offset
	0x00, 0x20,							// Control() offset
	0x00, 0x2c,							// Status() offset
	0x00, 0x52,							// Close() offset
	0x05, 0x2e, 0x44, 0x69, 0x73, 0x6b,	// ".Disk"

	// Open()
	M68K_EMUL_OP_DISK_OPEN >> 8, M68K_EMUL_OP_DISK_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_DISK_PRIME >> 8, M68K_EMUL_OP_DISK_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_DISK_CONTROL >> 8, M68K_EMUL_OP_DISK_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_DISK_STATUS >> 8, M68K_EMUL_OP_DISK_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};

static const uint8 cdrom_driver[] = {	// CD-ROM driver
	// Driver header
	CDROMDriverFlags >> 8, CDROMDriverFlags & 0xff, 0, 0, 0, 0, 0, 0,
	0x00, 0x1c,							// Open() offset
	0x00, 0x20,							// Prime() offset
	0x00, 0x24,							// Control() offset
	0x00, 0x30,							// Status() offset
	0x00, 0x56,							// Close() offset
	0x08, 0x2e, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x43, 0x44, 0x00,	// ".AppleCD"

	// Open()
	M68K_EMUL_OP_CDROM_OPEN >> 8, M68K_EMUL_OP_CDROM_OPEN & 0xff,
	0x4e, 0x75,							//  rts

	// Prime()
	M68K_EMUL_OP_CDROM_PRIME >> 8, M68K_EMUL_OP_CDROM_PRIME & 0xff,
	0x60, 0x0e,							//  bra		IOReturn

	// Control()
	M68K_EMUL_OP_CDROM_CONTROL >> 8, M68K_EMUL_OP_CDROM_CONTROL & 0xff,
	0x0c, 0x68, 0x00, 0x01, 0x00, 0x1a,	//  cmp.w	#1,$1a(a0)
	0x66, 0x04,							//  bne		IOReturn
	0x4e, 0x75,							//  rts

	// Status()
	M68K_EMUL_OP_CDROM_STATUS >> 8, M68K_EMUL_OP_CDROM_STATUS & 0xff,

	// IOReturn
	0x32, 0x28, 0x00, 0x06,				//  move.w	6(a0),d1
	0x08, 0x01, 0x00, 0x09,				//  btst		#9,d1
	0x67, 0x0c,							//  beq		1
	0x4a, 0x40,							//  tst.w	d0
	0x6f, 0x02,							//  ble		2
	0x42, 0x40,							//  clr.w	d0
	0x31, 0x40, 0x00, 0x10,				//2 move.w	d0,$10(a0)
	0x4e, 0x75,							//  rts
	0x4a, 0x40,							//1 tst.w	d0
	0x6f, 0x04,							//  ble		3
	0x42, 0x40,							//  clr.w	d0
	0x4e, 0x75,							//  rts
	0x2f, 0x38, 0x08, 0xfc,				//3 move.l	$8fc,-(sp)
	0x4e, 0x75,							//  rts

	// Close()
	0x70, 0xe8,							//  moveq	#-24,d0
	0x4e, 0x75							//  rts
};

static uint32 long_ptr;

static void SetLongBase(uint32 addr)
{
	long_ptr = addr;
}

static void Long(uint32 value)
{
	WriteMacInt32(long_ptr, value);
	long_ptr += 4;
}

static void gen_ain_driver(uintptr addr)
{
	SetLongBase(addr);

	// .AIn driver header
	Long(0x4d000000); Long(0x00000000);
	Long(0x00200040); Long(0x00600080);
	Long(0x00a0042e); Long(0x41496e00);
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_NOTHING));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_PRIME_IN));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CONTROL));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_STATUS));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_NOTHING));
	Long(0x00000000); Long(0x00000000);
};

static void gen_aout_driver(uintptr addr)
{
	SetLongBase(addr);

	// .AOut driver header
	Long(0x4d000000); Long(0x00000000);
	Long(0x00200040); Long(0x00600080);
	Long(0x00a0052e); Long(0x414f7574);
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_OPEN));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_PRIME_OUT));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CONTROL));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_STATUS));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CLOSE));
	Long(0x00000000); Long(0x00000000);
};

static void gen_bin_driver(uintptr addr)
{
	SetLongBase(addr);

	// .BIn driver header
	Long(0x4d000000); Long(0x00000000);
	Long(0x00200040); Long(0x00600080);
	Long(0x00a0042e); Long(0x42496e00);
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_NOTHING));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_PRIME_IN));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CONTROL));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_STATUS));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_NOTHING));
	Long(0x00000000); Long(0x00000000);
};

static void gen_bout_driver(uintptr addr)
{
	SetLongBase(addr);

	// .BOut driver header
	Long(0x4d000000); Long(0x00000000);
	Long(0x00200040); Long(0x00600080);
	Long(0x00a0052e); Long(0x424f7574);
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_OPEN));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_PRIME_OUT));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CONTROL));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_STATUS));
	Long(0x00000000); Long(0x00000000);
	Long(0xaafe0700); Long(0x00000000);
	Long(0x00000000); Long(0x00179822);
	Long(0x00010004); Long(NativeTVECT(NATIVE_SERIAL_CLOSE));
	Long(0x00000000); Long(0x00000000);
};

static const uint8 adbop_patch[] = {	// Call ADBOp() completion procedure
										// The completion procedure may call ADBOp() again!
	0x40, 0xe7,				//	move	sr,-(sp)
	0x00, 0x7c, 0x07, 0x00,	//	ori		#$0700,sr
	M68K_EMUL_OP_ADBOP >> 8, M68K_EMUL_OP_ADBOP & 0xff,
	0x48, 0xe7, 0x70, 0xf0,	//	movem.l	d1-d3/a0-a3,-(sp)
	0x26, 0x48,				//	move.l	a0,a3
	0x4a, 0xab, 0x00, 0x04,	//	tst.l	4(a3)
	0x67, 0x00, 0x00, 0x18,	//	beq		1
	0x20, 0x53,				//	move.l	(a3),a0
	0x22, 0x6b, 0x00, 0x04,	//	move.l	4(a3),a1
	0x24, 0x6b, 0x00, 0x08,	//	move.l	8(a3),a2
	0x26, 0x78, 0x0c, 0xf8,	//	move.l	$cf8,a3
	0x4e, 0x91,				//	jsr		(a1)
	0x70, 0x00,				//	moveq	#0,d0
	0x60, 0x00, 0x00, 0x04,	//	bra		2
	0x70, 0xff,				//1	moveq	#-1,d0
	0x4c, 0xdf, 0x0f, 0x0e,	//2	movem.l	(sp)+,d1-d3/a0-a3
	0x46, 0xdf,				//	move	(sp)+,sr
	0x4e, 0x75				//	rts
};


/*
 *  Install ROM patches (RAMBase and KernelDataAddr must be set)
 */

bool PatchROM(void)
{
	// Print ROM info
	D(bug("Checksum: %08lx\n", ntohl(*(uint32 *)ROMBaseHost)));
	D(bug("Version: %04x\n", ntohs(*(uint16 *)(ROMBaseHost + 8))));
	D(bug("Sub Version: %04x\n", ntohs(*(uint16 *)(ROMBaseHost + 18))));
	D(bug("Nanokernel ID: %s\n", (char *)ROMBaseHost + 0x30d064));
	D(bug("Resource Map at %08lx\n", ntohl(*(uint32 *)(ROMBaseHost + 26))));
	D(bug("Trap Tables at %08lx\n\n", ntohl(*(uint32 *)(ROMBaseHost + 34))));

	// Detect ROM type (shared with rom-inspect via rom_decode.hpp).
	// rom_detect_type() returns -1 (unrecognized) or 0..5 matching the
	// ROMTYPE_* enum ordering in rom_patches.h.
	ROMType = rom_detect_type(ROMBaseHost);
	fprintf(stderr, "[ROMPATCH] ROM type detected: %d (%s)\n", ROMType, rom_type_name(ROMType));
	if (ROMType == ROMTYPE_NEWWORLD)
		fprintf(stderr, "[ROMPATCH] NOTE: NewWorld ROM detected — full boot is NOT expected to succeed.\n"
		        "[ROMPATCH]   This ROM requires ongoing Machine Layer work (M9+). See docs/planning/MACHINE-LAYER-PLAN.md.\n"
		        "[ROMPATCH]   Use SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 for the current frontier.\n");
	if (ROMType < 0)
		return false;

	// EXPERIMENTAL (D3 Phase 2): best-effort patching of the G4-era 9.0.4 parcels ROM
	// (checksum 0xb8d0b672), which carries the 'ppcf' AltiVec gestalt. Auto-enabled ONLY
	// for that exact checksum, so the working 1.1 ROM (0xfd86d120) path is byte-identical.
	// Opt-out with SS_ROM_NO_904. See NEW-WORLD-ROM-SUPPORT-PLAN.md.
	{
		uint32 cksum = ntohl(*(uint32 *)ROMBaseHost);
		if (cksum == 0xb8d0b672 && !getenv("SS_ROM_NO_904")) {
			g_rom_904_lenient = true;
			fprintf(stderr, "[ROMPATCH] 9.0.4 G4 ROM (cksum %08x) detected — lenient patch mode ON (experimental)\n", cksum);
		}
		// SS_ROM_LENIENT=1: diagnostic override — force lenient mode for ANY ROM (e.g. the versioned
		// New World parcels ROMs whose 31 relocated patches the whole-image fallback auto-resolves).
		// Lets us probe how far PatchROM gets on a new ROM without hardcoding its checksum. The ~17
		// absent patches still need RE; this just stops the early relocated-pattern aborts.
		else if (getenv("SS_ROM_LENIENT") && ROMType == ROMTYPE_NEWWORLD) {
			g_rom_904_lenient = true;
			fprintf(stderr, "[ROMPATCH] SS_ROM_LENIENT — lenient patch mode FORCED for NewWorld ROM (cksum %08x, diagnostic)\n", cksum);
		}
	}

	// Check that other ROM addresses point to really free regions
	if (!check_rom_patch_space(CHECK_LOAD_PATCH_SPACE, 0x40))
		return false;
	if (!check_rom_patch_space(ZERO_SCRAP_PATCH_SPACE, 0x40))
		return false;
	if (!check_rom_patch_space(PUT_SCRAP_PATCH_SPACE, 0x40))
		return false;
	if (!check_rom_patch_space(GET_SCRAP_PATCH_SPACE, 0x40))
		return false;
	if (!check_rom_patch_space(ADDR_MAP_PATCH_SPACE - 10 * 4, 0x100))
		return false;

	// Apply patches
	if (!patch_nanokernel_boot()) return false;
	if (!patch_68k_emul()) return false;
	if (!patch_nanokernel()) return false;
	if (!patch_68k()) {
		// Parcels: patch_68k installs 68k-side EMUL_OP HLE (nvram/via/drivers/time) whose byte-patterns
		// are absent. With the jump68k redirect (Path A), the 68k init WILL be reached — missing HLE
		// shims are the expected next wall. Let PatchROM complete so we can observe where it stalls.
		if (g_rom_904_lenient)
			fprintf(stderr, "[ROMPATCH] parcels: patch_68k incomplete — boot continues "
			        "(missing 68k HLE shims are the expected next wall)\n");
		else
			return false;
	}

#ifdef M68K_BREAK_POINT
	// Install 68k breakpoint
	uint16 *wp = (uint16 *)(ROMBaseHost + M68K_BREAK_POINT);
	*wp++ = htons(M68K_EMUL_BREAK);
	*wp = htons(M68K_EMUL_RETURN);
#endif

#ifdef POWERPC_BREAK_POINT
	// Install PowerPC breakpoint
	uint32 *lp = (uint32 *)(ROMBaseHost + POWERPC_BREAK_POINT);
	*lp = htonl(0);
#endif

	// PATH B DIAGNOSTIC (SS_NW_SYNTH_ENTRY) — may be removable.
	// Skips the nanokernel and enters the DR Emulator directly. Hits a
	// dead end at the first Mixed-Mode transition (0xFFC0 F-line requires
	// nanokernel). Kept as a diagnostic/research tool; Path A (jump68k
	// redirect via the nanokernel) is the forward path.
	if (MachineEnvFlag("SS_NW_SYNTH_ENTRY") && MachineProfileIsNewWorld()) {
		uint32 *lp = (uint32 *)(ROMBaseHost + 0x310000);
		lp[0] = htonl(0x7C3042A6);  // mfspr r1, SPRG0       (r1 = KDP)
		lp[1] = htonl(0x3BE11000);  // addi r31, r1, 0x1000  (r31 = ECB)
		lp[2] = htonl(0x3FC05036);  // lis r30, 0x5036        (r30 = ROM base mask)
		lp[3] = htonl(0x3FA05048);  // lis r29, 0x5048        (r29 = dispatch table)
		const uint32 target = 0x36e964;
		int32_t offset = target - 0x310010;
		lp[4] = htonl(0x48000000 | (offset & 0x03FFFFFC));  // b ROM+0x36e964
		fprintf(stderr, "[NW-SYNTH] ROM+0x310000: cold-start dispatch "
		        "(r31=ECB, r30=0x50360000, r29=0x50480000, b 0x%x)\n"
		        "  verify: %08x %08x %08x %08x %08x\n",
		        target, ntohl(lp[0]), ntohl(lp[1]), ntohl(lp[2]),
		        ntohl(lp[3]), ntohl(lp[4]));
	}

	// Copy 68k emulator to 2MB boundary
	memcpy(ROMBaseHost + ROM_SIZE, ROMBaseHost + (ROM_SIZE - 0x100000), 0x100000);

	// NewWorld DR Emulator register-fixup trampoline (SS_NW_TRAMPOLINE).
	//
	// The nanokernel's context-switch restores r29/r30/r31 from the ECB
	// with addresses computed for its own BAT/SR layout (0x75xxxxxx), and
	// writes 68k vectors (guest[0]/[4]) using its virtual ROM address
	// (0x97xxxxxx).  Neither exists in SheepShaver's flat model.
	//
	// Trampoline at mirror 0x50429b40 (genuine free space, 222KB zero run
	// at ROM+0x329b40): fixes r29/r30/r31, writes correct 68k vectors,
	// then branches to cold-start.  Table[0] redirected here.
	//
	// Diagnostic mode: always cold-starts (re-inits handler table every
	// context-switch).  Production ongoing-entry support is TODO.
	static auto PatchROM_NW_trampoline = []() {
		if (!MachineProfileIsNewWorld())
			return;

		uint32 *tbl0 = (uint32 *)(ROMBaseHost + 0x46e8c0);
		uint32 old_val = ntohl(*tbl0);
		const uint32 expected = 0x48001040u;  // b +0x1040 (ongoing entry)
		if (old_val != expected) {
			fprintf(stderr, "[NW-TRAMP] table[0] @ ROM+0x46e8c0: unexpected %08x "
			        "(expected %08x) — skipping trampoline\n", old_val, expected);
			return;
		}

		const uint32 tramp_offset = 0x429b40;
		uint32 *tp = (uint32 *)(ROMBaseHost + tramp_offset);
		if (ntohl(tp[0]) != 0 || ntohl(tp[1]) != 0) {
			fprintf(stderr, "[NW-TRAMP] trampoline target ROM+0x%x not zero "
			        "(%08x %08x) — skipping\n",
			        tramp_offset, ntohl(tp[0]), ntohl(tp[1]));
			return;
		}

		// Register fixups (r29/r30/r31 from nanokernel virtual → flat model)
		tp[0]  = htonl(0x3FE068FFu);  // lis  r31, 0x68ff
		tp[1]  = htonl(0x63FFF000u);  // ori  r31, r31, 0xf000  → r31=0x68fff000 (ECB)
		tp[2]  = htonl(0x3FC05046u);  // lis  r30, 0x5046       → r30=0x50460000 (mirror)
		// M6a Wave 1 (M6A-DR-HANDOFF-ANALYSIS.md §0/§4): r29 is the DR dispatcher's
		// opcode-table base — handler = (r29 & 0xFFF80007) | (opcode<<3) at mirror
		// 0x5046e9dc.  The old seed 0x5046e000 was the probe-verified smoking gun:
		// its implied 512KB base 0x50400000 has no table (mirror NK/emulator code),
		// so the first 68k opcode (0x4efa) dispatched into the Thud console help
		// printer (0x504277d0).  Correct base = LA_DispatchTable = ROMBase+0x480000
		// — the dump-verified MIRROR dispatch table (131072/131072 slots populated;
		// its relative branches keep execution inside the 0x46xxxx mirror emulator).
		tp[3]  = htonl(0x3FA05048u);  // lis  r29, 0x5048
		tp[4]  = htonl(0x63BD0000u);  // ori  r29, r29, 0x0000  → r29=0x50480000 (LA_DispatchTable)
		// 68k vector fixups: nanokernel wrote guest[0]/[4] using its own
		// virtual addresses (0x97xxxxxx).  Cold-start at 5046e9b4 reads
		// guest[0]=SSP, guest[4]=PC as flat addresses — must be valid.
		tp[5]  = htonl(0x3B800000u);  // li   r28, 0            (base for stw)
		tp[6]  = htonl(0x3C005000u);  // lis  r0, 0x5000
		tp[7]  = htonl(0x6000002Au);  // ori  r0, r0, 0x002a    → r0=0x5000002a (ROM+0x2a)
		tp[8]  = htonl(0x901C0004u);  // stw  r0, 4(r28)        guest[4] = 68k reset PC
		tp[9]  = htonl(0x3C000010u);  // lis  r0, 0x0010        → r0=0x00100000 (1MB)
		tp[10] = htonl(0x901C0000u);  // stw  r0, 0(r28)        guest[0] = 68k initial SSP

		// M6a Wave 2 (M6A-WAVE2-SHIM-RECON.md §2 + §3 rows 1-2): guest-side
		// re-assertion of the Wave-2 seeds.  Both are seeded host-side in
		// init_emul_ppc ([NW-TRAMP] glue cluster), but live in regions the NK
		// cold-init wipes — the Wave-1 crash dump PROVES glue-time vector writes
		// don't survive ([0x2C]=0 at the F-line fault, memo §1.3, despite glue's
		// rte-stub loop seeding vectors 2..63).  The trampoline runs at table[0]
		// dispatch — after NK init, before the 68k cold start — so stores here
		// stick.  r0/r28 are existing trampoline scratch; r28 is restored to 0
		// afterwards (cold-start register contract unchanged).
		//
		// (1) 68k exception vectors 0x10 (illegal) / 0x28 (A-line) / 0x2C (F-line)
		//     → dedicated `bra.s *` stop stubs at 0x50429c00/10/20 (written below
		//     in the same zero run), memo §2 "exception-vector quick-win": a
		//     vectored exception parks at a unique probe-able PC instead of
		//     sliding through address 0 (memo §1.3 slide).
		tp[11] = htonl(0x3C005042u);  // lis  r0, 0x5042
		tp[12] = htonl(0x60009C00u);  // ori  r0, r0, 0x9c00    → r0=0x50429c00 (illegal stub)
		tp[13] = htonl(0x901C0010u);  // stw  r0, 0x10(r28)     guest[0x10] = illegal vector
		tp[14] = htonl(0x3C005042u);  // lis  r0, 0x5042
		tp[15] = htonl(0x60009C10u);  // ori  r0, r0, 0x9c10    → r0=0x50429c10 (A-line stub)
		tp[16] = htonl(0x901C0028u);  // stw  r0, 0x28(r28)     guest[0x28] = A-line vector
		tp[17] = htonl(0x3C005042u);  // lis  r0, 0x5042
		tp[18] = htonl(0x60009C20u);  // ori  r0, r0, 0x9c20    → r0=0x50429c20 (F-line stub)
		tp[19] = htonl(0x901C002Cu);  // stw  r0, 0x2c(r28)     guest[0x2C] = F-line vector
		// (2) [KDP+0xfd0] = 'Hnfo' hardware-info record @0x68ff4f00 (memo §2
		//     Option A).  The record BODY lives in the sub-KDP pool (host-seeded
		//     in glue, not wiped); only this KDP-page pointer needs guest-side
		//     re-assertion (cf. the KDP+0x6b4 cap clobber precedent).
		tp[20] = htonl(0x3F8068FFu);  // lis  r28, 0x68ff
		tp[21] = htonl(0x639CEFD0u);  // ori  r28, r28, 0xefd0  → r28=0x68ffefd0 (KDP+0xfd0)
		tp[22] = htonl(0x3C0068FFu);  // lis  r0, 0x68ff
		tp[23] = htonl(0x60004F00u);  // ori  r0, r0, 0x4f00    → r0=0x68ff4f00 ('Hnfo' record)
		tp[24] = htonl(0x901C0000u);  // stw  r0, 0(r28)        [KDP+0xfd0] = record
		// (3) [KDP+0x67c] is left for the NK to initialize.
		// The NK's cold-init writes [KDP+0x67c] = 0x68fff070 (ECB+0x70, the NK's
		// from-emulator interrupt-pending halfword).  OP_IRQ_NW reads this pointer
		// and writes 0 to 0x68fff070 to retire the interrupt post — that IS the
		// correct retirement path.  A prior attempt to overwrite [KDP+0x67c] with a
		// shadow address (0x68ff6084) in these two slots broke the NK's interrupt
		// dispatch: the NK posts level|0x8000 to whatever address is in [KDP+0x67c];
		// with our shadow value the ECB+0x70 pending cell was never set → no 68k
		// delivery → stall (dec_expiries=5 boot).  Fix: always nops here.
		tp[25] = htonl(0x60000000u);  // nop
		tp[26] = htonl(0x60000000u);  // nop
		uint32 idx = 27;
		// M6a Wave 2 recon (MPLibrary reboot-loop diagnosis): the DR emulator's
		// Mixed Mode Magic path (opcode 0xFE01, the $AAFE RoutineDescriptor
		// service) allocates a 0x220-byte 68k-context save record from a pool
		// the NK provisions in the ECB on real hardware:
		//   [ECB+0xE0] record array base (virtual)   [ECB+0xE4] base (physical)
		//   [ECB+0xE8] existence bitmap              [ECB+0xEC] in-use bitmap
		// Allocator at staged 0x5046e304: cntlzw([0xE8] & ~[0xEC]); if no free
		// bit it branches to entry-vector slot +0x3c (0x5046e8fc) — ZERO in the
		// static table — and the zero-fall-through stub re-enters table[0] →
		// this trampoline's cold start → 68k reset → the ~80 ms reboot loop.
		// Probe-verified live: all four words read 0 at the bail.
		//
		// Rung 2 Task T (plan 2026-06-11-m6a-rung2-mixedmode-switch, rev 2 C1):
		// the pool is now the NEWWORLD PROFILE DEFAULT (SS_NW_MM_POOL=0 opts
		// OUT for A/B; =1 is a harmless explicit-on no-op).  Base RELOCATED
		// 0x68ff5000 → 0x68ff5800: the old base was NOT a free gap — it is
		// the 'Hnfo' machine-detect scratch record (sheepshaver_glue.cpp seeds
		// [hnfo_rec+0x08] = irp_base+0x1000 = 0x68ff5000, and the ROM
		// copy-out at ROM+0xAC20 writes scratch +0x10..+0x17 — inside what
		// was MM save record 0).  New base per the sub-KDP occupancy map
		// (M6A-ONGOING-ENTRY-DESIGN.md "Rung 2 contracts" → "Sub-KDP occupancy
		// map"): pool = 0x68ff5800..0x68ff6080 (4 × 0x220), Task-X scratch
		// word reserved at 0x68ff6080, all clear of NKSystemInfo (0x68ff4000
		// +0x120), IRP banks (+0xDF0..0xEBC), Hnfo record (0x68ff4f00), Hnfo
		// scratch (0x68ff5000..0x68ff57ff reserve), the NK free-list
		// (KDP-0x7000 = 0x68ff7000) and the NK-supplied initial MM record
		// (0x68ffb8e0).  V=P flat model → virt and phys bases identical.
		// Guest-side (per-cycle) like the Hnfo re-assert: NK cold-init rebuilds
		// the ECB every cycle, so glue-time seeds would not survive.
		// Pool sizing: 4 records + the slot-15 loud stop (Task-0 Q-A pinned —
		// observed concurrent depth 1; never guess bigger, residue R-1).
		const char *mm_pool_env = getenv("SS_NW_MM_POOL");
		bool mm_pool = !(mm_pool_env && strcmp(mm_pool_env, "0") == 0);
		// Rung 2 Task Y (acceptance flip, plan rev 2 P6): SS_NW_MM_SWITCH is
		// the newworld profile DEFAULT (was env-opt-in through Tasks V..X).
		// Opt-out with SS_NW_MM_SWITCH=0 (A/B: restores the pre-switch
		// baseline — FE01 retry spin, 0 TVector visits, always-cold table[0]).
		// Paravirtual/OldWorld untouched (this whole lambda early-returns on
		// non-newworld profiles).
		//
		// Rung 2 Task V (plan rev 2 C11): the switch implies the pool —
		// the switch's whole point is that FE01 allocates a save record and the
		// NK switches contexts; switch-without-pool would park the first FE01
		// at the slot-15 exhaust stop by construction.  SS_NW_MM_POOL=0 with
		// the switch on (now the default) is a MISCONFIGURATION: log loudly and
		// re-enable the pool (the switch wins; a pool-only A/B needs
		// SS_NW_MM_SWITCH=0 too).
		const char *mm_switch_env = getenv("SS_NW_MM_SWITCH");
		const bool mm_switch = !(mm_switch_env && strcmp(mm_switch_env, "0") == 0);
		if (mm_switch && !mm_pool) {
			fprintf(stderr, "[NW-TRAMP] V: MISCONFIG — SS_NW_MM_POOL=0 with the "
			        "MixedMode switch on (switch implies pool, plan rev 2 C11); "
			        "ignoring the pool opt-out and provisioning the pool. "
			        "For a pool-off A/B set SS_NW_MM_SWITCH=0 as well.\n");
			mm_pool = true;
		}
		if (mm_pool) {
			// (a) Idempotent constant re-asserts — SAFE on every entry
			//     (cold or warm): base pointers + existence bitmap never
			//     change after provisioning.
			tp[idx++] = htonl(0x3F8068FFu);  // lis  r28, 0x68ff
			tp[idx++] = htonl(0x639CF000u);  // ori  r28, r28, 0xf000 → r28=ECB
			tp[idx++] = htonl(0x3C0068FFu);  // lis  r0, 0x68ff
			tp[idx++] = htonl(0x60005800u);  // ori  r0, r0, 0x5800   → 0x68ff5800
			tp[idx++] = htonl(0x901C00E0u);  // stw  r0, 0xE0(r28)    pool base (virt)
			tp[idx++] = htonl(0x901C00E4u);  // stw  r0, 0xE4(r28)    pool base (phys, V=P)
			tp[idx++] = htonl(0x3C00F000u);  // lis  r0, 0xF000       → 4-slot bitmap
			tp[idx++] = htonl(0x901C00E8u);  // stw  r0, 0xE8(r28)    existence bitmap
			// (b) COLD-ARM-ONLY INVARIANT (plan rev 2 C3): the [ECB+0xEC]
			//     in-use-bitmap wipe is DESTRUCTIVE on a warm re-entry — it
			//     frees records that may hold live 68k contexts (Tasks V/W
			//     round trips).  Today table[0] is always-cold (PROBE-O1:
			//     1 cold entry, 0 re-entries per boot), so every-entry ==
			//     cold-only and this placement is exact.  Task X's R2
			//     discriminator MUST wrap exactly these two words inside its
			//     cold arm (the scratch word reserved at 0x68ff6080 is the
			//     discriminator's home); the (a) re-asserts above may stay
			//     on both arms.  Task-W update: under SS_NW_MM_SWITCH the W
			//     discriminator (below) routes warm table[0] entries around
			//     this trampoline entirely, so the wipe IS structurally
			//     cold-only there; switch-off keeps the documented-invariant
			//     status (always-cold regime, every-entry == cold-only).
			tp[idx++] = htonl(0x38000000u);  // li   r0, 0
			tp[idx++] = htonl(0x901C00ECu);  // stw  r0, 0xEC(r28)    in-use bitmap (COLD ARM ONLY)
		}
		// Rung 2 Task V (SS_NW_MM_SWITCH, newworld default-ON since Task Y): the two
		// KDP seeds that break the FE01 retry spin — Q-C PINNED
		// (M6A-ONGOING-ENTRY-DESIGN.md "Rung 2 contracts" → Q-C).  The spin's
		// pivot is the NK switch-to-context service 0x503143a0 (reached via
		// [KDP+0x5f4] from the slot-1 stub):
		//   (1) `mtcrf 0x3f,r7; bnel cr2,…` classifies the call from the
		//       stub-composed [KDP+0x660] flags word; seeded 0, cr2eq==0 ⇒
		//       every slot-1 call bounces "not from emulator" having done
		//       nothing ⇒ command byte stays 0 ⇒ FE07 EQ ⇒ rte ⇒ retry.
		//       Seed: set bit 0x00200000 (= cr2eq under mtcrf 0x3f; the
		//       stub's rlwimi chain only composes bits 0x80000000/0x20, so
		//       the bit passes straight through into cr2).  OR, not store —
		//       preserve whatever else the NK/stub family derives.
		//   (2) the MRU context-cache lookup ([KDP+0x340/348/350/358] keys,
		//       paired ctx ptrs at +0x344/34c/354/35c) must accept the
		//       requested context ID r3=0x68fff400 (the MMCB; the service
		//       compares r8 = r3 & ~0x3F, and 0x68fff400 is already
		//       64-aligned).  Live the cache is NK-init poison (-1/0 ×4
		//       pairs), and the slow path's page-table validation
		//       (bl 0x503154b8) is unverified on the flat model (residue
		//       R-6) — so pre-seed MRU pair 0: key [KDP+0x340]=0x68fff400,
		//       ctx [KDP+0x344]=0x68fff400 (the natural pair value is the
		//       MMCB itself; the hit path consumes it as
		//       [pair+0x5c]:=[KDP+0x648], [KDP-0x14]:=pair → NK SAVE
		//       0x50312b0c → scheduler resumes the MixedMode ctx at
		//       [MMCB+0xfc]=0x500ebc20, the ROM's own native glue that
		//       calls the TVector 0x500cef8c).
		// PLACEMENT (survival rationale): both seeds are KDP-PAGE state.  NK
		// cold-init wipes/poisons the KDP page AFTER init_emul_ppc (the
		// [KDP+0xfd0] re-assert precedent above; the MRU poison was probed
		// live DURING the spin, i.e. post-trampoline of an unseeded run) —
		// glue-time seeds would not survive.  The trampoline runs at table[0]
		// dispatch, after NK cold-init, so stores here stick; nothing on the
		// FE01 path re-initializes either word (the slow path only ADDS MRU
		// entries).  Falsifiable live; one-iteration rule applies.
		// Cold/warm classification (Task-V review): (a)-class — the
		// [KDP+0x660] OR may stay on both Task-X arms (idempotent).  The MRU
		// pair-0 re-assert is provisionally both-arms but Task X must
		// re-evaluate: a warm re-assert can evict an NK-installed MRU entry
		// -> unverified slow path (R-6).  (Task-W status: the W discriminator
		// below routes warm entries AROUND this trampoline entirely, so both
		// seeds are structurally cold-only today; the classification governs
		// Task X's arm design.)
		// FE07 needs NO implementation (plan rev 2 P8 satisfied by Q-C):
		// its `bne` falls through when the completion path writes a nonzero
		// command byte — the DR side is complete.
		// MSR: NO new MSR writes (Q-E verdict — the MixedMode excursion's MSR
		// is per-context, carried by the NK switch's ctx SRR1 slot).
		// Budget: +9 words; pool+switch maximal layout = 46 insns ending
		// ROM+0x429bf8, still below the 0x429c00 stop stubs.
		if (mm_switch) {
			tp[idx++] = htonl(0x3F8068FFu);  // lis  r28, 0x68ff
			tp[idx++] = htonl(0x639CE000u);  // ori  r28, r28, 0xe000 → r28=KDP (0x68ffe000)
			tp[idx++] = htonl(0x801C0660u);  // lwz  r0, 0x660(r28)
			tp[idx++] = htonl(0x64000020u);  // oris r0, r0, 0x0020   set "from emulator" bit 0x00200000
			tp[idx++] = htonl(0x901C0660u);  // stw  r0, 0x660(r28)   [KDP+0x660] flags word
			tp[idx++] = htonl(0x3C0068FFu);  // lis  r0, 0x68ff
			tp[idx++] = htonl(0x6000F400u);  // ori  r0, r0, 0xf400   → r0=0x68fff400 (MMCB)
			tp[idx++] = htonl(0x901C0340u);  // stw  r0, 0x340(r28)   MRU pair-0 key = MMCB id
			tp[idx++] = htonl(0x901C0344u);  // stw  r0, 0x344(r28)   MRU pair-0 ctx ptr = MMCB
		}
		tp[idx] = htonl(0x3B800000u);  // li   r28, 0            (restore trampoline invariant)

		// M6a Wave 1, UserModeMSR transition (memo §5.2; plan rev 2 findings 1+5).
		// The unpatched ROM's jump68k dispatch tail does mtsrr0/mtsrr1/rfi with
		// UserModeMSR = [KDP-0x964] = 0x0000D032 (EE=1, PR=1); the Path-A redirect
		// dropped the mtsrr1, and this trampoline path performed NO MSR transition
		// at all — post-M3a (real stored MSR) the 68k world otherwise runs with the
		// NK's EE=0 MSR and the deferral machinery holds interrupts forever.
		// Site choice (rev 2 finding 5, preferred): trampoline-side GUEST
		// instructions, with the architectural mtmsr riding execute_mtmsr's EE 0→1
		// edge re-raise (M3a Task 3) — a host-side seed would bypass that re-raise.
		// (M6A-ONGOING-ENTRY-DESIGN correction: the original `lwz r0,-0x964(r1)`
		// assumed r1=KDP, but table[0] arrives with the CONTEXT-RESTORED register
		// file — r1=0 at cold boot — and the lwz faulted at guest 0-0x964
		// (Boot B's actual death, ea=0x...fffff69c; delivered_dec was 0: interrupt
		// plumbing was never reached). UserModeMSR is loaded r1-independently via
		// immediates instead.)  The 222KB zero run leaves ample patch-word budget.
		// Env-gated SS_M6A_USER_MSR=1, DEFAULT OFF (rev 2 finding 1): EE=1 during
		// 68k execution routes the first DEC delivery through unverified NK
		// save/restore plumbing; Boot A (off) = clean stall capture, Boot B (on) =
		// rung-2 recon.  When OFF the trampoline is byte-identical to the
		// no-MSR-write layout (37 insns with the default pool, 27 with
		// SS_NW_MM_POOL=0, 46 with pool+switch; branch slot tracks b_idx).
		//
		// Rung 2 Task V resolution (Q-E verdict — QUARANTINE): the MixedMode
		// switch needs no trampoline-side MSR write (per-context MSR via the
		// NK switch), SS_M6A_USER_MSR remains a known-broken diagnostic
		// (zero-page slide, residue R-2), and combining it with the switch
		// would also overrun the 0x429c00 word budget (49 > 48).  With
		// SS_NW_MM_SWITCH=1 the user-msr block is therefore DISABLED, loudly.
		bool user_msr = MachineEnvFlag("SS_M6A_USER_MSR");
		if (mm_switch && user_msr) {
			fprintf(stderr, "[NW-TRAMP] V: SS_M6A_USER_MSR=1 ignored under "
			        "SS_NW_MM_SWITCH=1 (Q-E verdict: per-context MSR via the NK "
			        "switch; user-msr stays quarantined known-broken diagnostic)\n");
			user_msr = false;
		}
		uint32 b_idx = idx + 1;
		if (user_msr) {
			tp[b_idx++] = htonl(0x3C000000u);  // lis  r0, 0             r1-independent immediate load
			tp[b_idx++] = htonl(0x6000D032u);  // ori  r0, r0, 0xd032    r0 = UserModeMSR (EE=1, PR=1)
			tp[b_idx++] = htonl(0x7C000124u);  // mtmsr r0               (EE-edge re-raise fires)
		}
		// b → mirror cold-start 0x5046e964 (offset computed from the b's own slot)
		tp[b_idx] = htonl(0x48000000u |
		                  ((0x46e964u - (tramp_offset + b_idx * 4)) & 0x03FFFFFCu));
		// Trampoline budget: 48 words (0x429b40–0x429bff). b_idx is the last slot.
		// Maximal layout (pool+switch+VIA_IFR) fills exactly 48 words (b_idx=47).
		// VIA_IFR uses 0 trampoline words (redirects the existing branch slot only).
		// This fires at PatchROM time — a boot with an over-budget trampoline means
		// some new feature added words without accounting for the fixed section.
		if (b_idx > 47) {
			fprintf(stderr, "[NW-TRAMP] BUDGET OVERRUN: b_idx=%u > 47 (48-word limit). "
			        "Trampoline writes past 0x429bff into patch-space! "
			        "Reduce word count or extend the budget region.\n", (unsigned)b_idx);
		}

		// Rung 2 Task W (SS_NW_MM_SWITCH pair; plan "Task W: FE02 switch-back"):
		// the WARM/ONGOING arm at table[0].  Live falsification (Task-W boots
		// 1-2, addendum "Task W results"): the native MixedMode completion
		// does NOT call the NK switch service 0x503143a0 (zero backward-
		// direction visits; slow/error/save legs zero) — it re-enters the
		// 68k-emulator world by branching THROUGH THE ENTRY-VECTOR TABLE
		// (the forward hit path planted [ctx+0x5c]:=[KDP+0x648] for exactly
		// this), arriving at table[0] with the native glue's register file
		// (r1=native stack 0x103ffa2c, r24=scratch junk).  The always-cold
		// trampoline then rewrote guest[0]/[4] and cold-started the 68k — a
		// reset per excursion (design doc §2.4 failure mode (ii), live: ring
		// `100266f2 → 0 → 1 → 5000002c`, 68k reset PC).  Fix: a cold/warm
		// discriminator in a NEW verified-zero region (the trampoline's
		// 46/48-word budget is full — plan Task-W note), table[0] → 0x50429d00:
		//   COLD (scratch 0x68ff6080 == 0): set scratch := 1, b trampoline —
		//        cold-start exactly once per boot; the [ECB+0xEC] wipe and the
		//        V seeds become STRUCTURALLY cold-only (rev 2 C3).
		//   WARM (scratch != 0): re-assert pool constants [ECB+0xE0/E4/E8] —
		//        the NK ctx save writes through r6=ECB (probed live:
		//        [ECB+0xEC]=saved r12=0x5046e1a0; ctx slot stride 8 puts
		//        saved r11 at +0xE4) and OVERLAYS the pool words on every
		//        switch-out; without the warm re-assert the next FE01
		//        allocation computes the record's PHYS address from saved-r11
		//        junk (0x0002f072, the stub's MSR fiction) and the NK would
		//        save 0x220 bytes into low 68k RAM.  [ECB+0xEC] is NOT touched
		//        (the in-use record is live at warm entry; the word
		//        accumulates overlay junk in bits 1/3 — residue R-10 in the
		//        addendum) — then b 0x5046f900, the DISPLACED original
		//        table[0] target: the slot-0 stub saves caller r7-r13 and
		//        exits via [KDP+0x5f0]=NK 0x50313bf8, which restores the
		//        parked emulator ctx → resume 0x5046e1a0 → the FE01 service
		//        reads the command byte and completes (e1f4: r24:=saved 68k
		//        PC — the post-$AAFE resume).
		// Register contract: warm arm clobbers r0/cr0 (PPC-ABI volatile; the
		// DR/glue protocol treats them so on the forward leg too) and CTR
		// (dead at entry — the caller arrived via bctr, and the architectural
		// slot stubs clobber it immediately with mtctr r1); r28 is stashed in
		// CTR and restored.  Scratch 0x68ff6080 is the occupancy-map address
		// RESERVED for Task X's R2 discriminator — Task W uses it minimally
		// (cold-once test-and-set); Task X refines (re-census, route
		// decision, R4) on top of this provisional arm.
		// PatchROM-time only (rev 2 C4); verify-zero-first on the whole
		// region (rev 2 C6).  Switch-off: table[0] → trampoline direct,
		// byte-identical to the pre-W layout (region not written).
		uint32 tbl0_target = tramp_offset;  // default: always-cold trampoline
		if (mm_switch) {
			const uint32 w_offset = 0x429d00;
			uint32 *wp = (uint32 *)(ROMBaseHost + w_offset);
			// COLD-ARM EXTRA SEED ([KDP+0x658], one-iteration re-pin, Task-W
			// boot 4): the warm route reaches the NK selector service —
			// the completion calls table[0] with r3=0xff (THE COMMAND BYTE
			// rides in r3) and LR=0x500ef258 (its continuation) → 0x50313cc8
			// `beq cr2` → 0x50312af8 `lwz r9, 0x658(r1)` — the switch-back
			// TARGET ctx is **[KDP+0x658]**, live-probed GARBAGE (=1).
			// Writer census (W2 rev 3.1 item 5 correction of the original
			// "no NK writer" claim): ONE cold-init writer exists in the NK
			// image — `stw r12,0x658(r1)` at static 0x310834 (dump-verified,
			// the live-garbage source).  It runs during NK cold-init, i.e.
			// BEFORE table[0] dispatch, so this cold-arm seed wins; there is
			// NO post-init/switch-path writer.  Residue: an NK reset/re-init
			// would re-run 0x310834 and re-garbage the word while the
			// discriminator scratch stays warm (addendum residue R-15).
			// The restore-from-junk resumed at a data address (boot-3
			// SIGSEGV, resume PC 0x68fff740).  Semantics = "the 68k-emulator
			// context block" (the paravirtual CR-injection writes the parked
			// emulator's saved CR at [[KDP+0x658]]+0xdc — sheepshaver_glue
			// :2276, main_unix :2678; ECB ctx slot +0xdc = saved r13/CR,
			// design doc §2.1) → seed [KDP+0x658] = ECB 0x68fff000.
			// KDP-page state ⇒ trampoline-resident timing like the V seeds;
			// the trampoline itself is at its 48-word budget, so the seed
			// lives on THIS cold arm (same once-per-boot, post-NK-init).
			//
			// W2 rev 3.1 item 6 (one-word hardening, red-team endorsed):
			// cold-arm `[KDP-0x14]:=ECB` right after the [KDP+0x658] seed —
			// [KDP-0x14] is the NK's "current context" word (the forward hit
			// path writes [KDP-0x14]:=pair); seeding it to the ECB gives the
			// pre-first-switch window a sane current-ctx instead of NK-init
			// junk.  Register state verified: r28=KDP, r0=ECB live there.
			//
			// W2 rev 3.1 item 2 (THE warm-arm world flip, R-12): the warm
			// arm now performs the [KDP+0x65c] current-world flip —
			// [KDP+0x65c] is the slot-stub SAVE TARGET (`lwz r6,0x65c(r1)`,
			// design doc §1.3) and must hold the OUTGOING world's ctx: ECB
			// while the 68k world runs (forward switch parks the emulator
			// there), MMCB during native excursions (the warm switch-back
			// must park the NATIVE ctx there, not overwrite the parked
			// emulator — Task-W boot 5's self-switch).  The warm arm runs
			// exactly when the native completion re-enters via table[0], so
			// set [KDP+0x65c]:=MMCB (0x68fff400) here, before the slot-0
			// stub saves through it.  The ECB-restoring half of the flip is
			// the slot-1 retargeted region below (forward direction).
			// Region: 30 words, 0x429d00..0x429d74 (end exclusive 0x429d78).
			static const uint32 w_code[] = {
				0x7F8903A6u,  // 0x429d00: mtctr r28          stash caller r28 (CTR dead: arrived via bctr)
				0x3F8068FFu,  // 0x429d04: lis   r28, 0x68ff
				0x639C6080u,  // 0x429d08: ori   r28, r28, 0x6080 → r28 = R2 scratch 0x68ff6080
				0x801C0000u,  // 0x429d0c: lwz   r0, 0(r28)
				0x2C000000u,  // 0x429d10: cmpwi r0, 0         (cr0 only — cr2 is the DR mode field)
				0x40820028u,  // 0x429d14: bne   +0x28 (warm arm 0x429d3c)
				// cold arm — r0/r28/CTR free by the cold-start contract:
				0x38000001u,  // 0x429d18: li    r0, 1
				0x901C0000u,  // 0x429d1c: stw   r0, 0(r28)    scratch := 1 (cold exactly once)
				0x3F8068FFu,  // 0x429d20: lis   r28, 0x68ff
				0x639CE000u,  // 0x429d24: ori   r28, r28, 0xe000 → r28 = KDP
				0x3C0068FFu,  // 0x429d28: lis   r0, 0x68ff
				0x6000F000u,  // 0x429d2c: ori   r0, r0, 0xf000   → r0 = ECB
				0x901C0658u,  // 0x429d30: stw   r0, 0x658(r28) [KDP+0x658] = emulator ctx (NK switch-back target)
				0x901CFFECu,  // 0x429d34: stw   r0, -0x14(r28) [KDP-0x14] = ECB (item-6 hardening: current ctx)
				0x4BFFFE08u,  // 0x429d38: b     0x50429b40    → the cold trampoline
				// warm arm — preserves everything except r0/cr0/CTR:
				0x3F8068FFu,  // 0x429d3c: lis   r28, 0x68ff
				0x639CF000u,  // 0x429d40: ori   r28, r28, 0xf000 → r28 = ECB
				0x3C0068FFu,  // 0x429d44: lis   r0, 0x68ff
				0x60005800u,  // 0x429d48: ori   r0, r0, 0x5800   → 0x68ff5800 (pool base)
				0x901C00E0u,  // 0x429d4c: stw   r0, 0xE0(r28)  pool virt base re-assert
				0x901C00E4u,  // 0x429d50: stw   r0, 0xE4(r28)  pool phys base re-assert (overlay repair)
				0x3C00F000u,  // 0x429d54: lis   r0, 0xF000     → existence bitmap
				0x901C00E8u,  // 0x429d58: stw   r0, 0xE8(r28)  existence bitmap re-assert
				// the world flip (rev 3.1 item 2): [KDP+0x65c] := MMCB
				0x3F8068FFu,  // 0x429d5c: lis   r28, 0x68ff
				0x639CE000u,  // 0x429d60: ori   r28, r28, 0xe000 → r28 = KDP
				0x3C0068FFu,  // 0x429d64: lis   r0, 0x68ff
				0x6000F400u,  // 0x429d68: ori   r0, r0, 0xf400   → r0 = MMCB
				0x901C065Cu,  // 0x429d6c: stw   r0, 0x65c(r28) [KDP+0x65c] = MMCB (native excursion = outgoing world)
				0x7F8902A6u,  // 0x429d70: mfctr r28            restore caller r28
				0x48045B8Cu,  // 0x429d74: b     0x5046f900     ongoing entry = original slot-0 stub
			};
			const size_t w_n = sizeof(w_code) / sizeof(w_code[0]);
			bool w_zero = true;
			for (size_t i = 0; i < w_n; i++)
				if (ntohl(wp[i]) != 0) { w_zero = false; break; }
			if (!w_zero) {
				fprintf(stderr, "[NW-TRAMP] W: discriminator site ROM+0x%x not zero "
				        "— ongoing arm SKIPPED (table[0] stays always-cold)\n",
				        w_offset);
			} else {
				for (size_t i = 0; i < w_n; i++)
					wp[i] = htonl(w_code[i]);
				tbl0_target = w_offset;
				fprintf(stderr, "[NW-TRAMP] W: table[0] → cold/warm discriminator "
				        "0x50429d00 (scratch 0x68ff6080; cold → trampoline once; "
				        "warm → pool re-assert + [KDP+0x65c]:=MMCB flip + "
				        "b 0x5046f900 slot-0 stub)\n");
			}

			// W2 rev 3.1 item 1: the slot-1 (forward-direction) half of the
			// [KDP+0x65c] world-flip discipline (R-12).  Slot 1 is the
			// FE01/MixedMode entry the 68k world consumes (Q-B: the only
			// consumed slot); every traversal means "the 68k-emulator world
			// is the outgoing world", so [KDP+0x65c] must be ECB before the
			// stub body saves through it / before the next forward switch
			// parks the emulator there.  Mechanism (red-team corrected
			// design — NOT an in-place prepend, NOT patch_68k_emul): a
			// RETARGETED 7-word region after the enlarged W region, doing
			// the flip then falling into the ORIGINAL slot-1 stub body at
			// 0x5046fa00 (= 0x46e8c4 + 0x113c, the displaced static branch
			// target).  Clobbers r0/CTR only — the same registers the
			// architectural stub body clobbers in its first words (mtctr r1;
			// lwz r1,XLM_KERNEL_DATA(0)), so the prepend is
			// convention-transparent.  Idempotent on every call (stores the
			// constant ECB).  Switch-off: NEITHER write happens (this whole
			// block is mm_switch-gated) — the table word keeps its static
			// 0x4800113c and the region stays zero, byte-identical baseline.
			// Verify discipline: region verify-zero-first (rev 2 C6); the
			// table word is NONZERO by design, so it gets the sibling
			// verify-EXPECTED check (== 0x4800113c) — loud skip otherwise.
			// Task-X fold (W2 review minor): the slot-1 flip only arms when
			// the W discriminator actually landed (tbl0_target == w_offset).
			// Half-armed would be the ECB half of the flip with NO MMCB half
			// (the warm arm lives in the W region): the first native
			// completion's warm save would then overwrite the parked emulator
			// ctx again (Task-W boot-5 self-switch) — belt-and-braces over
			// half-armed discipline.
			if (tbl0_target != w_offset) {
				fprintf(stderr, "[NW-TRAMP] W2: slot-1 flip SKIPPED — W "
				        "discriminator did not land (table[0] stays always-cold; "
				        "a half-armed flip would re-open the boot-5 self-switch)\n");
			} else {
				const uint32 s1_offset = 0x429d80;            // mirror 0x50429d80
				const uint32 slot1_offset = 0x46e8c0 + 0x04;  // entry-vector slot 1
				const uint32 slot1_expected = 0x4800113Cu;    // b +0x113c → 0x46fa00 (static)
				static const uint32 s1_code[] = {
					0x7C2903A6u,  // 0x429d80: mtctr r1            stash caller r1 (CTR dead: stub body re-clobbers)
					0x80202804u,  // 0x429d84: lwz   r1, 0x2804(0) r1 = [XLM_KERNEL_DATA] = KDP
					0x3C0068FFu,  // 0x429d88: lis   r0, 0x68ff
					0x6000F000u,  // 0x429d8c: ori   r0, r0, 0xf000 → r0 = ECB
					0x9001065Cu,  // 0x429d90: stw   r0, 0x65c(r1) [KDP+0x65c] = ECB (68k world = outgoing world)
					0x7C2902A6u,  // 0x429d94: mfctr r1            restore caller r1
					0x48045C68u,  // 0x429d98: b     0x5046fa00    → the original slot-1 stub body
				};
				const size_t s1_n = sizeof(s1_code) / sizeof(s1_code[0]);
				uint32 *s1p = (uint32 *)(ROMBaseHost + s1_offset);
				uint32 *slot1 = (uint32 *)(ROMBaseHost + slot1_offset);
				bool s1_zero = true;
				for (size_t i = 0; i < s1_n; i++)
					if (ntohl(s1p[i]) != 0) { s1_zero = false; break; }
				if (!s1_zero) {
					fprintf(stderr, "[NW-TRAMP] W2: slot-1 flip region ROM+0x%x not "
					        "zero — flip SKIPPED (slot 1 keeps its static stub)\n",
					        s1_offset);
				} else if (ntohl(*slot1) != slot1_expected) {
					fprintf(stderr, "[NW-TRAMP] W2: entry-vector slot 1 ROM+0x%x reads "
					        "%08x (expected %08x) — flip SKIPPED\n",
					        slot1_offset, ntohl(*slot1), slot1_expected);
				} else {
					for (size_t i = 0; i < s1_n; i++)
						s1p[i] = htonl(s1_code[i]);
					*slot1 = htonl(0x48000000u |
					               ((s1_offset - slot1_offset) & 0x03FFFFFCu));
					fprintf(stderr, "[NW-TRAMP] W2: slot 1 → flip region 0x50429d80 "
					        "([KDP+0x65c]:=ECB, then b 0x5046fa00 original stub)\n");
				}
			}
		}
		*tbl0 = htonl(0x48000000u | ((tbl0_target - 0x46e8c0u) & 0x03FFFFFCu));
		                             // table[0] → discriminator (switch-on) / trampoline

		// M6a Wave 2 item #2 (M6A-WAVE2-SHIM-RECON.md §2 quick-win): the 68k
		// "diagnosable stop" stubs the vectors above point at — one `bra.s *`
		// (0x60FE) self-loop per vector, 0x10 apart in the same mirror zero run,
		// safely past the trampoline code (ends ≤ ROM+0x429bf8 (pool+switch
		// maximal layout; pool+msr was 0x429be0 — msr and switch are mutually
		// exclusive, see the Task-V quarantine above)):
		//   0x50429c00  illegal-instruction stop (vector offset 0x10)
		//   0x50429c10  A-line stop              (vector offset 0x28)
		//   0x50429c20  F-line stop              (vector offset 0x2C)
		// A vectored 68k exception parks the world at one of these unique PCs,
		// probe-able via SS_PROBE_PC, instead of the memo §1.3 PC=0 slide.
		const uint32 stub_offset = 0x429c00;
		uint16 *sp = (uint16 *)(ROMBaseHost + stub_offset);
		sp[0x00 / 2] = htons(0x60FE);  // bra.s *  (illegal stop)
		sp[0x10 / 2] = htons(0x60FE);  // bra.s *  (A-line stop)
		sp[0x20 / 2] = htons(0x60FE);  // bra.s *  (F-line stop)

		// Rung 2 Task T (plan rev 2 P2): entry-vector slot 15 (+0x3c) is the
		// DR allocator's no-free-record bail target (0x5046e304: cntlzw of
		// [ECB+0xE8] & ~[ECB+0xEC]; exhaustion → b slot 15).  Statically it is
		// POWERPC_ILLEGAL == 0x00000000 (emul_op.h:26; patch_68k_emul writes
		// branches at slots 0-3,5 and zeros elsewhere), so exhaustion used to
		// fall through zeros into table[0] → cold start → the ~80 ms reboot
		// loop.  Plant a LOUD STOP instead: slot 15 → a unique parked-PC PPC
		// `b *` self-loop at mirror 0x50429cf0 (the Q-B advance-enumeration
		// address, above the 0x429c30 budget line; the Task-U slots will use
		// 0x429c40..0x429cd0).  Any pool exhaustion now parks at a probe-able
		// PC (SS_PROBE_PC=0x50429cf0) — this is also the pool-sizing tripwire
		// (residue R-1: re-measured at Task Y via this stop never firing).
		// With SS_NW_MM_POOL=0 (A/B opt-out) the FIRST FE01 parks here —
		// a loud park replaces the old reboot-loop signature by design.
		// Both writes are verify-zero-first (rev 2 C6 — the bra.s stubs above
		// predate that rule; do not copy them).  PatchROM-time only (rev 2 C4).
		const uint32 exhaust_stub_offset = 0x429cf0;          // mirror 0x50429cf0
		const uint32 slot15_offset = 0x46e8c0 + 0x3c;         // entry-vector slot 15
		uint32 *exhaust_stub = (uint32 *)(ROMBaseHost + exhaust_stub_offset);
		uint32 *slot15 = (uint32 *)(ROMBaseHost + slot15_offset);
		if (ntohl(*exhaust_stub) != 0) {
			fprintf(stderr, "[NW-TRAMP] slot-15 exhaust stub site ROM+0x%x not zero "
			        "(%08x) — skipping loud stop\n",
			        exhaust_stub_offset, ntohl(*exhaust_stub));
		} else if (ntohl(*slot15) != 0) {
			fprintf(stderr, "[NW-TRAMP] entry-vector slot 15 ROM+0x%x not zero "
			        "(%08x, expected POWERPC_ILLEGAL=0) — skipping loud stop\n",
			        slot15_offset, ntohl(*slot15));
		} else {
			*exhaust_stub = htonl(0x48000000u);  // b *  (parked self-loop)
			*slot15 = htonl(0x48000000u |
			                ((exhaust_stub_offset - slot15_offset) & 0x03FFFFFCu));
			fprintf(stderr, "[NW-TRAMP] T: slot 15 (+0x3c, allocator exhaustion) → "
			        "loud stop 0x50429cf0 (b *)\n");
		}

		// Rung 2 Task U (plan rev 2; Q-B pinned consumed-slot map,
		// M6A-ONGOING-ENTRY-DESIGN.md "Rung 2 contracts" → Q-B): under the
		// live FE01 path ONLY slot 1 is consumed (10M visits; slots 2/3/5
		// zero visits; slot 0 = the one cold start) and slot 1 ALREADY has
		// its real static branch from patch_68k_emul — so Task U plants NO
		// new real handlers.  The remaining ZERO slots (4 — the
		// deliberately-dead interrupt vector — and 6-14, all
		// POWERPC_ILLEGAL == 0x00000000) were zero-slides: any future
		// fall-through would slide through zeros into table[0] → cold start
		// → the reboot-loop signature.  Plant one UNIQUE parked-PC PPC
		// `b *` self-loop per dead slot, at exactly the Q-B
		// advance-enumeration addresses (the Task-U gate is live
		// parked-PCs ⊆ this set — expected EMPTY in the current regime):
		//   slot  4 → 0x50429c40    slot 10 → 0x50429c90
		//   slot  6 → 0x50429c50    slot 11 → 0x50429ca0
		//   slot  7 → 0x50429c60    slot 12 → 0x50429cb0
		//   slot  8 → 0x50429c70    slot 13 → 0x50429cc0
		//   slot  9 → 0x50429c80    slot 14 → 0x50429cd0
		// (slot 15 → 0x50429cf0 is Task T's exhaust stop above.)
		// Same idiom as slot 15: verify-zero-first on BOTH the table slot
		// and the stub site (rev 2 C6), PatchROM-time only (rev 2 C4),
		// after the mirror memcpy.  Slots 0-3,5 keep their static branches
		// (unconsumed ≠ parked).
		{
			static const struct { uint8 slot; uint32 stub; } u_slots[] = {
				{  4, 0x429c40u }, {  6, 0x429c50u }, {  7, 0x429c60u },
				{  8, 0x429c70u }, {  9, 0x429c80u }, { 10, 0x429c90u },
				{ 11, 0x429ca0u }, { 12, 0x429cb0u }, { 13, 0x429cc0u },
				{ 14, 0x429cd0u },
			};
			uint32 u_mask = 0;
			for (size_t i = 0; i < sizeof(u_slots) / sizeof(u_slots[0]); i++) {
				const uint32 slot_offset = 0x46e8c0u + u_slots[i].slot * 4u;
				const uint32 u_stub_offset = u_slots[i].stub;
				uint32 *slot_p = (uint32 *)(ROMBaseHost + slot_offset);
				uint32 *stub_p = (uint32 *)(ROMBaseHost + u_stub_offset);
				if (ntohl(*stub_p) != 0) {
					fprintf(stderr, "[NW-TRAMP] U: slot-%u stub site ROM+0x%x "
					        "not zero (%08x) — skipping loud stop\n",
					        u_slots[i].slot, u_stub_offset, ntohl(*stub_p));
					continue;
				}
				if (ntohl(*slot_p) != 0) {
					fprintf(stderr, "[NW-TRAMP] U: entry-vector slot %u ROM+0x%x "
					        "not zero (%08x, expected POWERPC_ILLEGAL=0) — "
					        "skipping loud stop\n",
					        u_slots[i].slot, slot_offset, ntohl(*slot_p));
					continue;
				}
				*stub_p = htonl(0x48000000u);  // b *  (parked self-loop)
				*slot_p = htonl(0x48000000u |
				                ((u_stub_offset - slot_offset) & 0x03FFFFFCu));
				u_mask |= 1u << u_slots[i].slot;
			}
			fprintf(stderr, "[NW-TRAMP] U: dead entry-vector slots {4,6-14} → "
			        "unique loud stops 0x50429c40..0x50429cd0 "
			        "(written mask=0x%04x, expected 0x7fd0)\n", u_mask);
		}

		// FE1F-service-surface Task A (plan rev 3, RATIFIED 2026-06-11) — the
		// rung-2 "dead slots get loud stops" policy is RETIRED: the slots were
		// never dead.  The raw ROM's `twi 31,r31,N` placeholders (0x0fff000N at
		// file 0x36e8c0+4N, raw md5 7b1378be…; slot 14 duplicates 0x0d — the
		// ROM's own quirk, restored verbatim) ARE the design: executing a slot
		// raises a program interrupt (vector 0x700) and the NK's published
		// handler ([KDP+0x37c]=0x50314700 [PROBE✓]) decodes the slot id and
		// dispatches through the exit-pointer array [KDP+0x5f0+4·slot] — the
		// same NK selector-service gateway regime the sc surface traverses.
		// Slot 15 (Task T's allocator-exhaustion stop) IS also a raw
		// placeholder (0x0fff000f at file 0x36e8fc, re-verified this session)
		// → restored too; the exhaustion diagnostics move to telemetry on the
		// 0x700 delivery path (SheepExcProgramShim's slot-15 loud line +
		// delivered-program counter, sheepshaver_glue.cpp).
		//
		// Restore is NEWWORLD DEFAULT since Task C (acceptance battery green
		// pre/post-flip); explicit-"0"-only opt-out SS_NW_FE1F_SURFACE=0 (the
		// SS_NW_SC_SURFACE polarity precedent — the same gate, same polarity,
		// arms the 0x700 delivery surface in init_emul_ppc).  Opted out, the
		// Task-T/U stops above stay and the 0x5000f248 park baseline is
		// byte-identical.  Verify-EXPECTED discipline: each slot's current
		// word must be exactly the stop branch Task T/U wrote above (same
		// branch arithmetic), else skip loudly.
		const char *fe1f_env = getenv("SS_NW_FE1F_SURFACE");
		if (!(fe1f_env && strcmp(fe1f_env, "0") == 0)) {
			static const struct { uint8 slot; uint32 stub; uint32 raw; } r_slots[] = {
				{  4, 0x429c40u, 0x0fff0004u }, {  6, 0x429c50u, 0x0fff0006u },
				{  7, 0x429c60u, 0x0fff0007u }, {  8, 0x429c70u, 0x0fff0008u },
				{  9, 0x429c80u, 0x0fff0009u }, { 10, 0x429c90u, 0x0fff000au },
				{ 11, 0x429ca0u, 0x0fff000bu }, { 12, 0x429cb0u, 0x0fff000cu },
				{ 13, 0x429cc0u, 0x0fff000du }, { 14, 0x429cd0u, 0x0fff000du },
				{ 15, 0x429cf0u, 0x0fff000fu },  // Task T's exhaust stop, see note
			};
			uint32 r_mask = 0;
			for (size_t i = 0; i < sizeof(r_slots) / sizeof(r_slots[0]); i++) {
				const uint32 slot_offset = 0x46e8c0u + r_slots[i].slot * 4u;
				const uint32 expected = 0x48000000u |
					((r_slots[i].stub - slot_offset) & 0x03FFFFFCu);
				uint32 *slot_p = (uint32 *)(ROMBaseHost + slot_offset);
				if (ntohl(*slot_p) != expected) {
					fprintf(stderr, "[NW-FE1F] restore: slot %u ROM+0x%x holds "
					        "%08x, expected the Task-%s stop %08x — skipping\n",
					        r_slots[i].slot, slot_offset, ntohl(*slot_p),
					        r_slots[i].slot == 15 ? "T" : "U", expected);
					continue;
				}
				*slot_p = htonl(r_slots[i].raw);
				r_mask |= 1u << r_slots[i].slot;
			}
			fprintf(stderr, "[NW-FE1F] raw twi placeholders RESTORED over the "
			        "parked stops, slots {4,6-15} (mask=0x%04x, expected 0xffd0; "
			        "slot 14 = raw 0x0fff000d quirk verbatim) — twi → 0x700 → "
			        "[KDP+0x37c]=0x50314700\n", r_mask);
		}

		// 68k PC-desync fix — DR r0≡0 invariant re-assert (plan
		// 2026-06-11-68k-pc-desync Task A; root cause BINDING-pinned in
		// DSAT-WALL-RECON.md "Task 0 — the re-dispatch mechanism pinned",
		// commit c8429b23).  The DR maintains a standing invariant r0 == 0 in
		// the 68k-emulator world: its flag-writeback idiom is `addco. rX,rX,r0`
		// (mirror 0x50460c64), and this very stub composes constants from it
		// (`oris r6,r0,0x1300` @0x5046e2bc, `ori r6,r0,0xe05c` @0x5046e360,
		// `stw r0,0x210(r5)` @0x5046e34c — all verified [PATCH] this task).
		// Our M6A FE1F twi-callout PROGRAM delivery saves the in-flight r0
		// (= the NK service selector, e.g. 0x36 — the DR's own marshalling ABI,
		// `mr r0,r8` @0x5046db48) into the emulator ctx r0 slot (+0x104); the
		// NK save-and-switch (0x50312b0c) deliberately does NOT save r0 (the
		// invariant is its contract), so every later world switch-IN reloads
		// the stale selector into r0 and the first poisoned addco.-class
		// writeback mis-dispatches the 68k stream (the e388 selector shim's
		// jmp landed at 0xe412 = 0xe3dc + 0x36 → line-1111 → SysError 10 →
		// the DSAT wall).  On real hardware the slot-8 callout is a
		// parcels-patched direct call — no exception, no poisoned save.
		//
		// Fix shape (Task-0 BINDING; rev-2 F1 bctr-PRESERVING — no EE/MSR/rfi
		// semantics change, NK exit paths untouched): re-assert the invariant
		// with Apple's own `li r0,0` idiom (the 0x5046db7c callout-return
		// precedent) at the slot-exit re-entry point 0x5046e1a0 — the resume
		// PC every slot-1 world switch-out parks for the emulator ctx (LR of
		// the `bnel cr2,slot1` at 0x5046e19c).  Site survey (this task,
		// [PATCH]): the only link-calls into the entry-vector table are
		// e19c→slot1 (resume e1a0, THE consumed path), c9e4→slot2 (resume
		// c9e8) and c4f0→slot4 (resume c4f4) — the latter two are unconsumed
		// (Q-B census, zero visits) and are NOT patched; no other static
		// branch targets e1a0/e1a4.  r0 is provably dead across e19c→e1a0
		// (the NK switch reloads it from ctx regardless, so the raw code
		// cannot depend on it), making the zero safe on the bnel-not-taken
		// fall-through too.
		//
		// Mechanism: displace the word at 0x5046e1a0 (`lwz r1,0x10c(r3)`,
		// verify-EXPECTED-first) to a 3-word stub in the staged zero run
		// (0x429da0, above the 0x429d9c free line, verify-zero-first):
		// li r0,0; the displaced lwz; b 0x5046e1a4.  Staged copy ONLY (the
		// primary 0x5036e1a0 image is not executed by the DR regime).
		// NEWWORLD DEFAULT since the desync plan's Task C (acceptance battery
		// green pre/post-flip) — explicit-"0"-only opt-out
		// SS_NW_DR_R0_INVARIANT=0 (the SS_NW_SC_SURFACE/SS_NW_FE1F_SURFACE
		// polarity precedent: getenv+strcmp, NOT MachineEnvFlag).  Opted out,
		// neither region is written — byte-identical DSAT baseline
		// ([$C70]:=0x5000e448 #3391708, [$AF0]:=0x000A #3391720±1, SIGSEGV
		// ea=0x40000fffff42 @0x504661a0).  Paravirtual reach: NONE (inside
		// this MachineProfileIsNewWorld()-early-returning lambda AND the env
		// gate; legacy patch bodies untouched).
		const char *r0inv_env = getenv("SS_NW_DR_R0_INVARIANT");
		if (r0inv_env && strcmp(r0inv_env, "0") == 0) {
			fprintf(stderr, "[NW-DR-R0] r0-invariant fix OFF (SS_NW_DR_R0_INVARIANT=0 "
			        "opt-out) — the DSAT 68k PC-desync baseline is reproduced\n");
		} else {
			const uint32 site_offset = 0x46e1a0;       // mirror 0x5046e1a0 (slot-exit re-entry)
			const uint32 site_expected = 0x8023010Cu;  // lwz r1, 0x10c(r3)
			const uint32 r0_stub_offset = 0x429da0;    // staged zero run (free ≥ 0x429d9c)
			uint32 *site_p = (uint32 *)(ROMBaseHost + site_offset);
			uint32 *r0sp = (uint32 *)(ROMBaseHost + r0_stub_offset);
			if (ntohl(*site_p) != site_expected) {
				fprintf(stderr, "[NW-DR-R0] site ROM+0x%x reads %08x (expected %08x = "
				        "lwz r1,0x10c(r3)) — r0-invariant fix SKIPPED\n",
				        site_offset, ntohl(*site_p), site_expected);
			} else if (ntohl(r0sp[0]) != 0 || ntohl(r0sp[1]) != 0 || ntohl(r0sp[2]) != 0) {
				fprintf(stderr, "[NW-DR-R0] stub site ROM+0x%x not zero (%08x %08x %08x) "
				        "— r0-invariant fix SKIPPED\n",
				        r0_stub_offset, ntohl(r0sp[0]), ntohl(r0sp[1]), ntohl(r0sp[2]));
			} else {
				const uint32 stub_w0 = 0x38000000u;    // li  r0, 0  (the invariant re-assert)
				const uint32 stub_w1 = site_expected;  // lwz r1, 0x10c(r3)  (displaced word)
				const uint32 stub_w2 = 0x48000000u |
					(((site_offset + 4) - (r0_stub_offset + 8)) & 0x03FFFFFCu);
				                                       // b 0x5046e1a4 (rejoin past the site)
				const uint32 site_new = 0x48000000u |
					((r0_stub_offset - site_offset) & 0x03FFFFFCu);  // b 0x50429da0
				fprintf(stderr, "[NW-DR-R0] ARMED (newworld default; opt-out "
				        "SS_NW_DR_R0_INVARIANT=0): DR r0==0 "
				        "invariant re-assert at switch-in resume 0x5046e1a0\n");
				fprintf(stderr, "[NW-DR-R0] rewrite stub ROM+0x%x: 00000000x3 -> "
				        "%08x %08x %08x (li r0,0; lwz r1,0x10c(r3); b 0x5046e1a4)\n",
				        r0_stub_offset, stub_w0, stub_w1, stub_w2);
				fprintf(stderr, "[NW-DR-R0] rewrite site ROM+0x%x: %08x -> %08x "
				        "(b 0x50429da0)\n", site_offset, ntohl(*site_p), site_new);
				r0sp[0] = htonl(stub_w0);
				r0sp[1] = htonl(stub_w1);
				r0sp[2] = htonl(stub_w2);
				*site_p = htonl(site_new);
			}
		}

		fprintf(stderr, "[NW-TRAMP] register-fixup trampoline at ROM+0x%x "
		        "(%u insns), table[0] → trampoline → cold-start\n",
		        tramp_offset, (unsigned)(b_idx + 1));
		fprintf(stderr, "[NW-TRAMP] fixup: r31=0x68fff000 r30=0x50460000 "
		        "r29=0x50480000 guest[4]=0x5000002a guest[0]=0x00100000 "
		        "user-msr=%s mm-pool=%s mm-switch=%s\n",
		        user_msr ? "ON (SS_M6A_USER_MSR)" : "off",
		        mm_pool ? "ON (default; ECB+0xE0/E4=0x68ff5800 E8=0xF0000000)"
		                : "OFF (SS_NW_MM_POOL=0 opt-out)",
		        mm_switch ? "ON (default; [KDP+0x660]|=0x00200000, "
		                    "MRU[0x340/0x344]=0x68fff400)"
		                  : "OFF (SS_NW_MM_SWITCH=0 opt-out)");
		{
			fprintf(stderr, "[NW-TRAMP] W2 vector stop stubs (bra.s *): "
			        "illegal[0x10]=0x50429c00 aline[0x28]=0x50429c10 "
			        "fline[0x2c]=0x50429c20; [KDP+0xfd0]=0x68ff4f00 ('Hnfo') "
			        "re-asserted guest-side; [KDP+0x67c] left for NK cold-init (NK sets=0x68fff070)\n");
		}
	};
	PatchROM_NW_trampoline();

	// SS_DUMP_ROM: dump decompressed ROM image for offline disassembly.
	// Lives here (PatchROM) not in patch_68k() so it fires even when patch_68k fails on parcels.
	//
	// LIMITATION: writes ROM_SIZE (4MB) bytes — the file content only.  The extra 1MB
	// patch/mirror area (ROM_AREA_SIZE - ROM_SIZE = 0x100000, offsets 0x400000–0x4fffff)
	// is NOT included.  Addresses like the trampoline (ROM+0x429b40) and the DR emulator
	// mirror (ROM+0x46e964) live in that region and will not appear in the dump file.
	// To disassemble those, read from the live process (ROMBaseHost + offset) in lldb after
	// the emulator has started and PatchROM has run.
	{
		const char *dump_path = getenv("SS_DUMP_ROM");
		if (dump_path && *dump_path) {
			FILE *f = fopen(dump_path, "wb");
			if (f) {
				fwrite(ROMBaseHost, 1, ROM_SIZE, f);
				fclose(f);
				fprintf(stderr, "[ROM-DUMP] wrote %u bytes (ROM_SIZE, 4MB file content only; "
				        "patch/mirror area 0x400000-0x4fffff NOT included) to %s\n",
				        (unsigned)ROM_SIZE, dump_path);
			} else {
				fprintf(stderr, "[ROM-DUMP] failed to open %s for writing: %s\n", dump_path, strerror(errno));
			}
		}
	}

	return true;
}


/*
 *  Nanokernel boot routine patches
 */

static bool patch_nanokernel_boot(void)
{
	uint32 *lp;
	uint32 base, loc;

	// ROM boot structure patches
	lp = (uint32 *)(ROMBaseHost + 0x30d000);
	lp[0x9c >> 2] = htonl(KernelDataAddr);			// LA_InfoRecord
	lp[0xa0 >> 2] = htonl(KernelDataAddr);			// LA_KernelData
	lp[0xa4 >> 2] = htonl(KernelDataAddr + 0x1000);	// LA_EmulatorData
	lp[0xa8 >> 2] = htonl(ROMBase + 0x480000);		// LA_DispatchTable
	lp[0xac >> 2] = htonl(ROMBase + 0x460000);		// LA_EmulatorCode
	lp[0x360 >> 2] = htonl(0);						// Physical RAM base (? on NewWorld ROM, this contains -1)
	lp[0xfd8 >> 2] = htonl(ROMBase + 0x2a);		// 68k reset vector

	// Skip SR/BAT/SDR init
	loc = 0x310000;
	if (ROMType == ROMTYPE_GAZELLE || ROMType == ROMTYPE_GOSSAMER || ROMType == ROMTYPE_NEWWORLD) {
		lp = (uint32 *)(ROMBaseHost + loc);
		*lp++ = htonl(POWERPC_NOP);
		*lp = htonl(0x38000000);
	}
	static const uint8 sr_init_dat[] = {0x35, 0x4a, 0xff, 0xfc, 0x7d, 0x86, 0x50, 0x2e};
	if ((base = find_rom_data(0x3101b0, 0x3105b0, sr_init_dat, sizeof(sr_init_dat))) == 0) return false;
	D(bug("sr_init %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + loc + 8);
	*lp = htonl(0x48000000 | ((base - loc - 8) & 0x3fffffc));	// b		ROMBase+0x3101b0
	lp = (uint32 *)(ROMBaseHost + base);
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);		// lwz	r1,(pointer to Kernel Data)
	if (MachineProfileIsNewWorld()) {
		// NW parcels: seed r13/r14/r15 with REAL kernel memory layout so Init.s computes
		// valid KernelMemoryBase/End. Without this, r13=0xDEAD0000 propagates to KDP+0x638
		// and the free-list priming loop walks garbage addresses forever.
		//
		// Layout (matches Init.s expectations — HTAB at top, KDP below):
		//   [kmem_base..kmem_base+pgdesc_size) — page descriptor free list (grows upward)
		//   [sub_kdp_base..shmem_base)         — sub-KDP pool (32KB)
		//   [shmem_base..kdp+0x2000)           — KDP/shmem (8KB+)
		//   [htab_base..htab_end)              — HTAB (64KB, at KDP+0x2000)
		//
		// Init.s flow: HTAB_base = r13+r15-r14, KDP = (HTAB&~0xFFFF)-0x2000
		const uint32 kdp = KernelDataAddr;
		const uint32 sub_kdp_size = 0x8000;
		const uint32 shmem_base = kdp & ~0x3FFF;
		const uint32 sub_kdp_base = shmem_base - sub_kdp_size;
		const uint32 htab_size = 0x10000;
		const uint32 htab_base = kdp + 0x2000;
		const uint32 ram_size_bytes = RAMSize;
		const uint32 page_count = ram_size_bytes / 4096;
		const uint32 pgdesc_size = (page_count * 4 + 0xFFF) & ~0xFFF;
		const uint32 kmem_base = (sub_kdp_base - pgdesc_size) & ~0xFFFF;
		const uint32 kmem_end  = htab_base + htab_size;
		const uint32 kmem_total = kmem_end - kmem_base;
		// r13 = KernelMemoryBase
		*lp++ = htonl(0x3da00000 | (kmem_base >> 16));        // lis r13,kmem_base_hi
		// r14 = HTAB size (Init.s: HTAB_base = r13+r15-r14; also SDR1 HTABMASK)
		*lp++ = htonl(0x3dc00000 | (htab_size >> 16));        // lis r14,htab_size_hi
		// r15 = total kernel memory size (Init.s: KernelMemoryEnd = r13+r15)
		*lp = htonl(0x3de00000 | (kmem_total >> 16));         // lis r15,kmem_total_hi
	} else {
		*lp++ = htonl(0x3da0dead);		// lis	r13,0xdead	(start of kernel memory)
		*lp++ = htonl(0x3dc00010);		// lis	r14,0x0010	(size of page table)
		*lp = htonl(0x3de00010);		// lis	r15,0x0010	(size of kernel memory)
	}

	// Don't read PVR
	static const uint8 pvr_read_dat[] = {0x7d, 0x9f, 0x42, 0xa6};
	if ((base = find_rom_data(0x3103b0, 0x3108b0, pvr_read_dat, sizeof(pvr_read_dat))) == 0) return false;
	D(bug("pvr_read %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp = htonl(0x81800000 + XLM_PVR);	// lwz	r12,(theoretical PVR)

	// Set CPU specific data (even if ROM doesn't have support for that CPU).
	//
	// PARCELS layout (2001+ G4-aware ROMs, e.g. 9.0.1/9.2.x): the inline CPU-detect was
	// restructured, so lp[6] is no longer `cmpwi r12,1` (it's a BAT/SPR-clear init). On those ROMs
	// this per-CPU-data injection is UNNECESSARY: mfpvr already returns our faked PVR (default
	// 0x000c0000 = 7400; ppc-execute.cpp SPR_PVR), and the ROM's OWN CPU-detect chain (e.g. 0x310a1c
	// on 9.0.1: cmpwi r12,1/3/4/.../0xc/0xd -> common handler) has a native 7400 (0xc) entry that
	// selects the right per-CPU data itself. So SKIP the injection and derive `loc` (the per-CPU
	// handler, used by the SPRG3/MQ/MSR/DEC neutralizations below) from the ROM's own detect chain.
	// Gated on g_rom_904_lenient (set only for the 9.0.4 cksum or SS_ROM_LENIENT), and lp[6] only
	// ever differs on parcels — so the 1.1 LZSS path (lp[6] == 0x2c0c0001) is byte-identical.
	if (ntohl(lp[6]) != 0x2c0c0001) {
		if (!g_rom_904_lenient) return false;
		static const uint8 cpudet_dat[] = {0x2c, 0x0c, 0x00, 0x01};	// cmpwi r12,1 (chain head)
		uint32 cd;
		if ((cd = find_rom_data(0x310000, 0x320000, cpudet_dat, sizeof(cpudet_dat))) == 0) return false;
		uint32 beqw = ntohl(*(uint32 *)(ROMBaseHost + cd + 8));		// chain: cmpwi; addi; beq
		loc = (uint32)((cd + 8) + (int32)(int16)(beqw & 0xfffc));	// follow beq -> common handler
		fprintf(stderr, "[ROMPATCH] parcels: per-CPU-data injection SKIPPED (ROM self-handles faked "
		        "PVR=%08x); CPU-detect @%06x, handler loc=%06x\n", PVR, cd, loc);
	} else {
	uint32 ofs = ntohl(lp[7]) & 0xffff;
	D(bug("ofs %08lx\n", ofs));
	lp[8] = htonl((ntohl(lp[8]) & 0xffff) | 0x48000000);	// beq -> b
	loc = (ntohl(lp[8]) & 0xffff) + (uintptr)(lp+8) - (uintptr)ROMBaseHost;
	D(bug("loc %08lx\n", loc));
	lp = (uint32 *)(ROMBaseHost + ofs + 0x310000);
	switch (PVR >> 16) {
		case 1:		// 601
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00010040);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00400020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00080008);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x01000002);	// TLB total size/TLB assoc
			break;
		case 3:		// 603
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x2000);		// Data cache size
			lp[2] = htonl(0x2000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00020002);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00400002);	// TLB total size/TLB assoc
			break;
		case 4:		// 604
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x4000);		// Data cache size
			lp[2] = htonl(0x4000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00040004);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00800002);	// TLB total size/TLB assoc
			break;
//		case 5:		// 740?
		case 6:		// 603e
		case 7:		// 603ev
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x4000);		// Data cache size
			lp[2] = htonl(0x4000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00040004);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00400002);	// TLB total size/TLB assoc
			break;
		case 8:		// 750, 750FX
		case 0x7000:
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00080008);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00800002);	// TLB total size/TLB assoc
			break;
		case 9:		// 604e
		case 10:	// 604ev5
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00040004);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00800002);	// TLB total size/TLB assoc
			break;
//		case 11:	// X704?
		case 12:	// 7400, 7410, 7450, 7455, 7457
		case 0x800c:
		case 0x8000:
		case 0x8001:
		case 0x8002:
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00080008);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00800002);	// TLB total size/TLB assoc
			break;
		case 13:	// ???
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00000020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00080008);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x01000004);	// TLB total size/TLB assoc
			break;
//		case 50:	// 821
//		case 80:	// 860
		case 96:	// ???
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x8000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00010020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00200020);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00080008);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x00800004);	// TLB total size/TLB assoc
			break;
		case 0x39:	// 970
			lp[0] = htonl(0x1000);		// Page size
			lp[1] = htonl(0x8000);		// Data cache size
			lp[2] = htonl(0x10000);		// Inst cache size
			lp[3] = htonl(0x00200020);	// Coherency block size/Reservation granule size
			lp[4] = htonl(0x00010020);	// Unified caches/Inst cache line size
			lp[5] = htonl(0x00200020);	// Data cache line size/Data cache block size touch
			lp[6] = htonl(0x00800080);	// Inst cache block size/Data cache block size
			lp[7] = htonl(0x00020002);	// Inst cache assoc/Data cache assoc
			lp[8] = htonl(0x02000004);	// TLB total size/TLB assoc
			break;
		default:
			printf("WARNING: Unknown CPU type\n");
			break;
	}
	}

	// Don't set SPRG3, don't test MQ
	static const uint8 sprg3_mq_dat[] = {0x7d, 0x13, 0x43, 0xa6, 0x3d, 0x00, 0x00, 0x04, 0x7d, 0x00, 0x03, 0xa6, 0x39, 0x00, 0x00, 0x00, 0x7d, 0x00, 0x02, 0xa6};
	if ((base = find_rom_data(loc + 0x20, loc + 0x60, sprg3_mq_dat, sizeof(sprg3_mq_dat))) == 0) return false;
	D(bug("sprg3/mq %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	lp[0] = htonl(POWERPC_NOP);
	lp[2] = htonl(POWERPC_NOP);
	lp[4] = htonl(POWERPC_NOP);

	// Don't read MSR
	static const uint8 msr_dat[] = {0x7d, 0xc0, 0x00, 0xa6};
	if ((base = find_rom_data(loc + 0x40, loc + 0x80, msr_dat, sizeof(msr_dat))) == 0) return false;
	D(bug("msr %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp = htonl(0x39c00000);		// li	r14,0

	// Don't write to DEC
	lp = (uint32 *)(ROMBaseHost + loc + 0x70);
	*lp++ = htonl(POWERPC_NOP);
	loc = (ntohl(lp[0]) & 0xffff) + (uintptr)lp - (uintptr)ROMBaseHost;
	D(bug("loc %08lx\n", loc));

	// Don't set SPRG3
	static const uint8 sprg3_dat[] = {0x39, 0x21, 0x03, 0x60, 0x7d, 0x33, 0x43, 0xa6, 0x39, 0x01, 0x04, 0x20};
	if ((base = find_rom_data(0x310000, 0x314000, sprg3_dat, sizeof(sprg3_dat))) == 0) return false;
	D(bug("sprg3 %08lx\n", base + 4));
	lp = (uint32 *)(ROMBaseHost + base + 4);
	*lp = htonl(POWERPC_NOP);

	// Don't read PVR
	static const uint8 pvr_read2_dat[] = {0x7e, 0xff, 0x42, 0xa6, 0x56, 0xf7, 0x84, 0x3e};
	if ((base = find_rom_data(0x310000, 0x320000, pvr_read2_dat, sizeof(pvr_read2_dat))) == 0) return false;
	D(bug("pvr_read2 %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp = htonl(0x82e00000 + XLM_PVR);		// lwz	r23,(theoretical PVR)
	if ((base = find_rom_data(base + 4, 0x320000, pvr_read2_dat, sizeof(pvr_read2_dat))) != 0) {
		D(bug("pvr_read2 %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(0x82e00000 + XLM_PVR);	// lwz	r23,(theoretical PVR)
	}
	static const uint8 pvr_read3_dat[] = {0x7e, 0x5f, 0x42, 0xa6, 0x56, 0x52, 0x84, 0x3e};
	if ((base = find_rom_data(0x310000, 0x320000, pvr_read3_dat, sizeof(pvr_read3_dat))) != 0) {
		D(bug("pvr_read3 %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(0x82400000 + XLM_PVR);	// lwz	r18,(theoretical PVR)
	}
	static const uint8 pvr_read4_dat[] = {0x7d, 0x3f, 0x42, 0xa6, 0x55, 0x29, 0x84, 0x3e};
	if ((base = find_rom_data(0x310000, 0x320000, pvr_read4_dat, sizeof(pvr_read4_dat))) != 0) {
		D(bug("pvr_read4 %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(0x81200000 + XLM_PVR);	// lzw  r9,(theoritical PVR)
	}

	// Don't read SDR1 — replace mfspr with a sentinel.
	// PARCELS: skip — the trampoline pre-maps a real HTAB and initialises SDR1, so the
	// nanokernel's own mfspr SDR1 + zeroing loop should execute against real memory.
	static const uint8 sdr1_read_dat[] = {0x7d, 0x19, 0x02, 0xa6, 0x55, 0x16, 0x81, 0xde};
	if ((base = find_rom_data(0x310000, 0x320000, sdr1_read_dat, sizeof(sdr1_read_dat))) == 0) return false;
	if (g_rom_904_lenient) {
		fprintf(stderr, "[ROMPATCH] parcels: sdr1_read SKIP — real mfspr SDR1 will execute\n");
	} else {
		D(bug("sdr1_read %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp++ = htonl(0x3d00dead);		// lis	r8,0xdead		(pointer to page table)
		*lp++ = htonl(0x3ec0001f);		// lis	r22,0x001f	(size of page table)
		*lp = htonl(POWERPC_NOP);
	}

	// Don't clear page table, don't invalidate TLB
	// PARCELS: skip — let the real zeroing loop write to the mapped HTAB.
	static const uint8 pgtb_clear_dat[] = {0x36, 0xd6, 0xff, 0xfc, 0x7e, 0xe8, 0xb1, 0x2e, 0x41, 0x81, 0xff, 0xf8};
	if ((base = find_rom_data(0x310000, 0x320000, pgtb_clear_dat, sizeof(pgtb_clear_dat))) == 0) return false;
	if (g_rom_904_lenient) {
		fprintf(stderr, "[ROMPATCH] parcels: pgtb_clear SKIP — real zeroing loop will execute\n");
	} else {
		D(bug("pgtb_clear %08lx\n", base + 4));
		lp = (uint32 *)(ROMBaseHost + base + 4);
		*lp = htonl(POWERPC_NOP);
		D(bug("tblie %08lx\n", base + 12));
		lp = (uint32 *)(ROMBaseHost + base + 12);
		*lp = htonl(POWERPC_NOP);
	}

	// Don't create RAM descriptor table — NOPs `stwu r31,4(r29)` at ~0x503121D4 (decompressed).
	// This instruction is the page-descriptor store in the free-list priming loop.
	// OldWorld: NOP is correct (flat addressing, no MMU page tables needed).
	// NewWorld (SS_NW_TRAMPOLINE): must NOT be NOPped — the nanokernel's Reset.s bank scan
	// needs this stwu to populate the page descriptor array. Without it, r22=0xFFFFFFFC
	// (no pages found) and the mapping loop runs ~524K iterations doing nothing useful.
	// Byte pattern: {stwu r31,4(r29), addi r31,r31,0x1000, b -0x24}
	static const uint8 desc_create_dat[] = {0x97, 0xfd, 0x00, 0x04, 0x3b, 0xff, 0x10, 0x00, 0x4b, 0xff, 0xff, 0xdc};
	if ((base = find_rom_data(0x310000, 0x320000, desc_create_dat, sizeof(desc_create_dat))) == 0) return false;
	if (g_rom_904_lenient) {
		fprintf(stderr, "[ROMPATCH] parcels: desc_create SKIP — real stwu will build page descriptor free list\n");
	} else {
		D(bug("desc_create %08lx\n", base))
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(POWERPC_NOP);
	}

	// SPIKE: SegMap/PMDT stub — populate KDP SegMap pointers + PMDT data right before
	// CreateAreasFromPageMap runs, bypassing the unknown KDP+0x80 corruption during
	// page-init. PPC stub at ROM+0x30d600 writes minimal valid data and calls through.
	// Hypothesis test: if boot advances past CreateAreasFromPageMap, the SegMap data
	// theory is confirmed and a full implementation (IRP/KDP/EDP/ROM entries) follows.
	if (g_rom_904_lenient && MachineProfileIsNewWorld()) {
		const uint32 stub_rom_offset = 0x30d600;
		uint32 *stub = (uint32 *)(ROMBaseHost + stub_rom_offset);
		int n = 0;

		stub[n++] = htonl(0x7C0802A6);  // mflr r0
		stub[n++] = htonl(0x90010910);  // stw r0, 0x910(r1)  — save LR in KDP scratch

		// PMDT[0] at KDP+0x920: RAM 256MB {page=0, count=0xFFFF, flags=0x2 (RW)}
		stub[n++] = htonl(0x3D000000);  // lis r8, 0
		stub[n++] = htonl(0x6108FFFF);  // ori r8, r8, 0xFFFF  → r8 = 0x0000FFFF
		stub[n++] = htonl(0x91010920);  // stw r8, 0x920(r1)   — PMDT[0].page_count
		stub[n++] = htonl(0x39200002);  // li r9, 2
		stub[n++] = htonl(0x91210924);  // stw r9, 0x924(r1)   — PMDT[0].flags (type=0 normal)

		// PMDT[1] at KDP+0x928: sentinel {page=0, count=0xFFFF, flags=0x200}
		stub[n++] = htonl(0x91010928);  // stw r8, 0x928(r1)   — PMDT[1].page_count
		stub[n++] = htonl(0x39200200);  // li r9, 0x200
		stub[n++] = htonl(0x9121092C);  // stw r9, 0x92C(r1)   — PMDT[1].flags (sentinel type)

		// Empty sentinel at KDP+0x930 (for segments 1-15)
		stub[n++] = htonl(0x91010930);  // stw r8, 0x930(r1)
		stub[n++] = htonl(0x91210934);  // stw r9, 0x934(r1)

		// SegMap[0] = &PMDT[0] = KDP+0x920
		stub[n++] = htonl(0x39010920);  // addi r8, r1, 0x920
		stub[n++] = htonl(0x91010080);  // stw r8, 0x80(r1)

		// SegMap[1..15] = &empty_sentinel = KDP+0x930
		stub[n++] = htonl(0x39210930);  // addi r9, r1, 0x930
		stub[n++] = htonl(0x91210088);  // stw r9, 0x88(r1)
		stub[n++] = htonl(0x91210090);  // stw r9, 0x90(r1)
		stub[n++] = htonl(0x91210098);  // stw r9, 0x98(r1)
		stub[n++] = htonl(0x912100A0);  // stw r9, 0xA0(r1)
		stub[n++] = htonl(0x912100A8);  // stw r9, 0xA8(r1)
		stub[n++] = htonl(0x912100B0);  // stw r9, 0xB0(r1)
		stub[n++] = htonl(0x912100B8);  // stw r9, 0xB8(r1)
		stub[n++] = htonl(0x912100C0);  // stw r9, 0xC0(r1)
		stub[n++] = htonl(0x912100C8);  // stw r9, 0xC8(r1)
		stub[n++] = htonl(0x912100D0);  // stw r9, 0xD0(r1)
		stub[n++] = htonl(0x912100D8);  // stw r9, 0xD8(r1)
		stub[n++] = htonl(0x912100E0);  // stw r9, 0xE0(r1)
		stub[n++] = htonl(0x912100E8);  // stw r9, 0xE8(r1)
		stub[n++] = htonl(0x912100F0);  // stw r9, 0xF0(r1)
		stub[n++] = htonl(0x912100F8);  // stw r9, 0xF8(r1)

		// bl CreateAreasFromPageMap (0x5031f3b8)
		// stub bl is at 0x30d600 + n*4 = 0x30d678, disp = 0x31f3b8 - 0x30d678 = 0x11D40
		stub[n++] = htonl(0x48011D41);  // bl 0x5031f3b8

		stub[n++] = htonl(0x80010910);  // lwz r0, 0x910(r1)  — restore LR
		stub[n++] = htonl(0x7C0803A6);  // mtlr r0
		stub[n++] = htonl(0x4E800020);  // blr

		fprintf(stderr, "[ROMPATCH] SPIKE: SegMap/PMDT stub at ROM+%06x (%d insns)\n",
		        stub_rom_offset, n);

		// Redirect both bl CreateAreasFromPageMap call sites to our stub.
		// Site 1 (non-cr5 cold boot path): 0x3124e4, bytes 4800ced5
		static const uint8 bl_cafpm_1[] = {0x48, 0x00, 0xce, 0xd5};
		if ((base = find_rom_data(0x3124c0, 0x312500, bl_cafpm_1, sizeof(bl_cafpm_1))) != 0) {
			lp = (uint32 *)(ROMBaseHost + base);
			*lp = htonl(0x4BFFB11D);  // bl 0x5030d600 (disp = 0x30d600 - 0x3124e4 = -0x4EE4)
			fprintf(stderr, "[ROMPATCH] SPIKE: redirected bl at %06x → stub\n", base);
		}
		// Site 2 (cr5 path): 0x312568, bytes 4800ce51
		static const uint8 bl_cafpm_2[] = {0x48, 0x00, 0xce, 0x51};
		if ((base = find_rom_data(0x312540, 0x312580, bl_cafpm_2, sizeof(bl_cafpm_2))) != 0) {
			lp = (uint32 *)(ROMBaseHost + base);
			*lp = htonl(0x4BFFB099);  // bl 0x5030d600 (disp = 0x30d600 - 0x312568 = -0x4F68)
			fprintf(stderr, "[ROMPATCH] SPIKE: redirected bl at %06x → stub\n", base);
		}

		// Page-descriptor build-loop cap (KDP+0x6b4).
		//
		// The page-descriptor build loop at ROM 0x503123f4-0x312420 walks a
		// stride-8 SegMap pointer array at KDP+0x78 and stores a PMDT descriptor
		// through each pointer. Its trip count is set by a clamp at 0x503123a8:
		//
		//   r22 = r29 - r21                 ; natural span (kernel mem range)
		//   cap = [KDP+0x6b4] << 2          ; 0x3123ac lwz r8,0x6b4(r1); 0x3123b0 slwi
		//   if (r22 >= cap) r22 = cap - 4   ; 0x3123c4 blt / 0x3c8 addi r22,r8,-4
		//   r22 >>= 2                       ; loop ~ (r22 >> 16) trips
		//
		// KDP+0x6b4 is left 0 by our (skipped/faked) cold-init, so cap=0 → the
		// clamp forces r22 = -4 = 0xFFFFFFFC → ~16382 trips. The SegMap array is
		// seeded with valid pointers only for its first entries; beyond them lie
		// zeros and 0xFFFFFFFF poison (observed at KDP+0x340). The walk runs off
		// the end and stores through r8=0xFFFFFFFF at 0x5031240c → SIGSEGV.
		// Pre-M0 Path A "passed" this only because the paravirtual profile's
		// `ignoresegv` silently skipped the faulting stores; the NewWorld profile
		// aborts loudly (DEPRECATED-SCAFFOLDING-INVENTORY.md). Seeding KDP+0x6b4 at
		// init_emul_ppc time fails (cold-init zeroes the KDP afterward), so we force
		// the cap into the cap-read instruction itself: replace `lwz r8,0x6b4(r1)`
		// (0x3123ac) with `lis r8, ceil(page_count/0x10000)`, i.e. the physical page
		// count rounded up to 64K-page granularity (== page_count exactly for RAM
		// that is a 256MB multiple). The clamp then bounds the loop to a couple of
		// trips, well within the valid SegMap region, and the post-loop
		// KDP+0x6a8/0x6ac accounting lets `ble 0x3124e4` proceed to the SegMap/PMDT
		// stub + CreateAreasFromPageMap.
		// Anchor: subf r22,r21,r29 / lwz r8,0x6b4(r1) / slwi r8,r8,2 (unique).
		{
			static const uint8 pdcap_dat[] = {
				0x7e, 0xd5, 0xe8, 0x50,   // subf  r22, r21, r29
				0x81, 0x01, 0x06, 0xb4,   // lwz   r8, 0x6b4(r1)   <- patch this word
				0x55, 0x08, 0x10, 0x3a }; // slwi  r8, r8, 2
			if ((base = find_rom_data(0x312000, 0x313000, pdcap_dat, sizeof(pdcap_dat))) != 0) {
				const uint32 page_count = RAMSize / 4096;
				const uint32 cap_hi = (page_count + 0xFFFF) >> 16;  // 64K-page units, rounded up
				lp = (uint32 *)(ROMBaseHost + base + 4);            // the lwz r8,0x6b4(r1)
				*lp = htonl(0x3D000000 | (cap_hi & 0xFFFF));        // lis r8, cap_hi
				fprintf(stderr, "[ROMPATCH] parcels: page-descriptor loop cap forced — "
				        "lwz r8,0x6b4(r1)@%06x → lis r8,%u (cap=%u pages for %u MB RAM); "
				        "un-masks the 0x503123fc ceiling\n",
				        base + 4, cap_hi, cap_hi << 16, (unsigned)(RAMSize >> 20));
			} else {
				fprintf(stderr, "[ROMPATCH] parcels: page-descriptor loop cap anchor NOT FOUND "
				        "(0x503123fc ceiling will fault)\n");
			}
		}
	}

	// Don't load SRs and BATs.
	// PARCELS: the SR/BAT-load routine was restructured (its 1.1 signature `7c0004ac 839d0000
	// 938105e8` is absent even whole-image). But the ops it performs — `mtsrin` (segment-register
	// load) + `mt{i,d}bat{l,u}` (BAT setup) — are DROPPED no-ops on the aarch64 JIT (ppc-jit.cpp
	// supervisor stub), and its only real-RAM effect is saving the (ignored) BAT values into
	// KernelData (0x300-0x324). Under flat virtual=physical addressing there are no live SRs/BATs to
	// corrupt, so letting the routine RUN is harmless — we SKIP the neutralization on parcels instead
	// of re-RE'ing the routine+caller (the 1.1 `sr_load_caller` byte-pattern false-matches a
	// page-table loop on parcels, so it can't be trusted here anyway). ⚠ PATCH-ADVANCING BUT
	// RUNTIME-UNVALIDATED: confirm at boot that the nanokernel doesn't depend on the KernelData BAT
	// fields. Gated on g_rom_904_lenient; the 1.1 LZSS path runs the else branch byte-identically.
	static const uint8 sr_load[] = {0x7c, 0x00, 0x04, 0xac, 0x83, 0x9d, 0x00, 0x00, 0x93, 0x81, 0x05, 0xe8};
	if ((loc = find_rom_data(0x310000, 0x320000, sr_load, sizeof(sr_load))) == 0) {
		if (!g_rom_904_lenient) return false;
		fprintf(stderr, "[ROMPATCH] parcels: sr_load (SR/BAT-load) absent — SKIP neutralization "
		        "(SR/BAT are JIT no-ops; RUNTIME-UNVALIDATED)\n");
	} else {
		static const uint8 sr_load_caller[] = {0x3e, 0xd6, 0xff, 0xff, 0x41, 0x81, 0xff, 0xdc, 0xb2, 0xc8, 0x00, 0x02};
		if ((base = find_rom_data(0x310000, 0x320000, sr_load_caller, sizeof(sr_load_caller))) == 0) return false;
		if ((base = find_rom_powerpc_branch(base + 12, 0x320000, loc)) == 0) return false;
		D(bug("sr_load %08lx, called from %08lx\n", loc, base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(POWERPC_NOP);
	}

	// Don't mess with SRs
	static const uint8 sr_load2_dat[] = {0x83, 0xa1, 0x05, 0xe8, 0x57, 0x7c, 0x3e, 0x78, 0x7f, 0xbd, 0xe0, 0x2e};
	if ((base = find_rom_data(0x310000, 0x320000, sr_load2_dat, sizeof(sr_load2_dat))) == 0) return false;
	D(bug("sr_load2 %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp = htonl(POWERPC_BLR);

	// Don't check performance monitor
	static const uint8 pm_check_dat[] = {0x7e, 0x58, 0xeb, 0xa6, 0x7e, 0x53, 0x90, 0xf8, 0x7e, 0x78, 0xea, 0xa6};
	if ((base = find_rom_data(0x310000, 0x320000, pm_check_dat, sizeof(pm_check_dat))) == 0) return false;
	D(bug("pm_check %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	
	static const int spr_check_list[] = {
		952 /* mmcr0 */, 953 /* pmc1 */, 954 /* pmc2 */, 955 /* sia */,
		956 /* mmcr1 */, 957 /* pmc3 */, 958 /* pmc4 */, 959 /* sda */
	};

	for (int i = 0; i < sizeof(spr_check_list)/sizeof(spr_check_list[0]); i++) {
		int spr = spr_check_list[i];
		uint32 mtspr = 0x7e4003a6 | ((spr & 0x1f) << 16) | ((spr & 0x3e0) << 6);
		uint32 mfspr = 0x7e6002a6 | ((spr & 0x1f) << 16) | ((spr & 0x3e0) << 6);
		for (int ofs = 0; ofs < 64; ofs++) {
			if (ntohl(lp[ofs]) == mtspr) {
				if (ntohl(lp[ofs + 2]) != mfspr)
					return false;
				D(bug("  SPR%d %08lx\n", spr, base + 4*ofs));
				lp[ofs] = htonl(POWERPC_NOP);
				lp[ofs + 2] = htonl(POWERPC_NOP);
			}
		}
	}

	// Jump to 68k emulator.
	// 1.1: mtsprg2;mtsrr0;mtsrr1 pattern → find caller → redirect to SheepShaver's emulator.
	// Parcels: that pattern is absent. Instead, find the rfi block at 0x3126cc (lwz r4,0x648(r1);
	// lwz r8,0x5a4(r1); lwz r9,-0x964(r1); addi r8,r8,0x26e8; mtsrr0; mtsrr1; rfi) and replace
	// just the 3-instruction tail (mtsrr0;mtsrr1;rfi → mtctr r8;bctr;nop). Keeps the ROM's own
	// register setup; only swaps out the privilege transition for a direct branch.
	static const uint8 jump68k_dat[] = {0x7d, 0x92, 0x43, 0xa6, 0x7d, 0x5a, 0x03, 0xa6, 0x7d, 0x7b, 0x03, 0xa6};
	if ((loc = find_rom_data(0x310000, 0x320000, jump68k_dat, sizeof(jump68k_dat))) == 0) {
		if (g_rom_904_lenient) {
			// Parcels I/O poll patches (needed for both redirect and diagnostic paths):
			// the nanokernel polls VIA/CUDA status registers that don't exist in emulation.
			// M6a Wave 2 (ROM-PATCH-AUDIT: io_poll_beq RETIRE@M6a-Wave2):
			// M6A-DR-HANDOFF-ANALYSIS.md §5.3 — on the newworld fidelity profile the M1
			// SCC model answers Tx-ready polls honestly; NOPping the beq prevents polls
			// from ever reaching the device model, contradicting the machine layer's
			// premise.  Paravirtual profile keeps the NOP patches unchanged.
			// Search always runs (verifies targets); byte-patching gated on !newworld.
			{
				static const uint32 io_poll_beq_offsets[] = {
					0x326504, 0x3266f4, 0x326864, 0x326968, 0x326b60
				};
				int found = 0, patched = 0;
				for (unsigned i = 0; i < sizeof(io_poll_beq_offsets)/sizeof(io_poll_beq_offsets[0]); i++) {
					uint32 *p = (uint32 *)(ROMBaseHost + io_poll_beq_offsets[i]);
					if (ntohl(*p) == 0x4182fff4) {  // beq $-0xC
						found++;
						if (!MachineProfileIsNewWorld()) {
							*p = htonl(0x60000000); // nop
							patched++;
						}
					}
				}
				if (MachineProfileIsNewWorld())
					fprintf(stderr, "[M6a] io_poll_beq ROM patches retired (newworld profile): "
					        "M1 SCC answers polls honestly — %d/5 beq targets confirmed\n", found);
				else
					fprintf(stderr, "[ROMPATCH] parcels: patched %d/5 I/O poll loops (VIA/CUDA ready-wait)\n", patched);
			}

			// Path A: find the parcels rfi block and redirect via mtctr/bctr
			static const uint8 parcels_rfi_dat[] = {
				0x80, 0x81, 0x06, 0x48,  // lwz r4, 0x648(r1)  — dispatch table
				0x81, 0x01, 0x05, 0xa4,  // lwz r8, 0x5a4(r1)  — emul code base
				0x81, 0x21, 0xf6, 0x9c   // lwz r9, -0x964(r1) — UserModeMSR
			};
			uint32 rfi_loc;
			if ((rfi_loc = find_rom_data(0x310000, 0x320000, parcels_rfi_dat, sizeof(parcels_rfi_dat))) != 0) {
				D(bug("parcels jump68k rfi block at %08lx\n", rfi_loc));
				lp = (uint32 *)(ROMBaseHost + rfi_loc + 16);  // skip 4 kept instructions
				*lp++ = htonl(0x7d0903a6);  // mtctr r8  (was mtsrr0 r8)
				*lp++ = htonl(POWERPC_BCTR); // bctr      (was mtsrr1 r9)
				*lp   = htonl(POWERPC_NOP);  // nop       (was rfi)
				fprintf(stderr, "[ROMPATCH] parcels: jump68k REDIRECTED at %08x — "
				        "mtsrr0;mtsrr1;rfi → mtctr;bctr;nop (Path A)\n", rfi_loc);
				return true;
			}

			// Fallback: diagnostic skip (SS_ROM_SKIP_JUMP68K)
			if (getenv("SS_ROM_SKIP_JUMP68K")) {
				fprintf(stderr, "[ROMPATCH] parcels: jump68k NOT patched (DIAGNOSTIC) — boot will wedge "
				        "at the PPC->68k handoff; watching via [ALARM]/[HB]\n");
				return true;
			}
		}
		return false;
	}
	static const uint8 jump68k_caller_dat[] = {0x85, 0x13, 0x00, 0x08, 0x56, 0xbf, 0x50, 0x3e, 0x63, 0xff, 0x0c, 0x00};
	if ((base = find_rom_data(0x310000, 0x320000, jump68k_caller_dat, sizeof(jump68k_caller_dat))) == 0) return false;
	if ((base = find_rom_powerpc_branch(base + 12, 0x320000, loc)) == 0) return false;
	D(bug("jump68k %08lx, called from %08lx\n", loc, base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp++ = htonl(0x80610634);		// lwz	r3,0x0634(r1)	(pointer to Emulator Data)
	*lp++ = htonl(0x8081119c);		// lwz	r4,0x119c(r1)	(pointer to opcode table)
	*lp++ = htonl(0x80011184);		// lwz	r0,0x1184(r1)	(pointer to emulator init routine)
	*lp++ = htonl(0x7c0903a6);		// mtctr	r0
	*lp = htonl(POWERPC_BCTR);
	return true;
}


/*
 *  68k emulator patches
 */

static bool patch_68k_emul(void)
{
	// Lenient mode (g_rom_904_lenient): pattern misses skip-with-warning instead of aborting.
	uint32 *lp;
	uint32 base, loc;

	// Overwrite twi instructions
	static const uint8 twi_dat[] = {0x0f, 0xff, 0x00, 0x00, 0x0f, 0xff, 0x00, 0x01, 0x0f, 0xff, 0x00, 0x02};
	base = find_rom_data(0x36e600, 0x36ea00, twi_dat, sizeof(twi_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("twi %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp++ = htonl(0x48000000 + 0x36f900 - base);		// b 0x36f900 (Emulator start)
	*lp++ = htonl(0x48000000 + 0x36fa00 - base - 4);	// b 0x36fa00 (Mixed mode)
	*lp++ = htonl(0x48000000 + 0x36fb00 - base - 8);	// b 0x36fb00 (Reset/FC1E opcode)
	*lp++ = htonl(0x48000000 + 0x36fc00 - base - 12);	// FE0A opcode
	*lp++ = htonl(POWERPC_ILLEGAL);						// Interrupt
	*lp++ = htonl(0x48000000 + 0x36fd00 - base - 20);	// FE0F opcode
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp++ = htonl(POWERPC_ILLEGAL);
	*lp = htonl(POWERPC_ILLEGAL);
	} else fprintf(stderr, "[ROMPATCH] SKIP twi (absent in parcels)\n");

#if EMULATED_PPC
	// Install EMUL_RETURN, EXEC_RETURN, EXEC_NATIVE and EMUL_OP opcodes
	lp = (uint32 *)(ROMBaseHost + 0x380000 + (M68K_EMUL_RETURN << 3));
	*lp++ = htonl(POWERPC_EMUL_OP);
	*lp++ = htonl(0x4bf66e80);							// b	0x366084
	*lp++ = htonl(POWERPC_EMUL_OP | 1);
	*lp++ = htonl(0x4bf66e78);							// b	0x366084
	*lp++ = htonl(POWERPC_EMUL_OP | 2);
	*lp++ = htonl(0x4bf66e70);							// b	0x366084
	for (int i=0; i<OP_MAX; i++) {
		*lp++ = htonl(POWERPC_EMUL_OP | (i + 3));
		*lp++ = htonl(0x4bf66e68 - i*8);				// b	0x366084
	}
#else
	// Install EMUL_RETURN, EXEC_RETURN and EMUL_OP opcodes
	lp = (uint32 *)(ROMBaseHost + 0x380000 + (M68K_EMUL_RETURN << 3));
	*lp++ = htonl(0x80000000 + XLM_EMUL_RETURN_PROC);	// lwz	r0,XLM_EMUL_RETURN_PROC
	*lp++ = htonl(0x4bf705fc);							// b	0x36f800
	*lp++ = htonl(0x80000000 + XLM_EXEC_RETURN_PROC);	// lwz	r0,XLM_EXEC_RETURN_PROC
	*lp++ = htonl(0x4bf705f4);							// b	0x36f800
	*lp++ = htonl(0x00dead00);							// Let SheepShaver crash, since
	*lp++ = htonl(0x00beef00);							// no native opcode is available
	for (int i=0; i<OP_MAX; i++) {
		*lp++ = htonl(0x38a00000 + i);				// li	r5,OP_*
		*lp++ = htonl(0x4bf705ec - i*8);			// b	0x36f808
	}

	// Extra routines for EMUL_RETURN/EXEC_RETURN/EMUL_OP
	lp = (uint32 *)(ROMBaseHost + 0x36f800);
	*lp++ = htonl(0x7c0803a6);						// mtlr	r0
	*lp++ = htonl(0x4e800020);						// blr

	*lp++ = htonl(0x80000000 + XLM_EMUL_OP_PROC);	// lwz	r0,XLM_EMUL_OP_PROC
	*lp++ = htonl(0x7c0803a6);						// mtlr	r0
	*lp = htonl(0x4e800020);						// blr
#endif

	// Extra routine for 68k emulator start
	lp = (uint32 *)(ROMBaseHost + 0x36f900);
	*lp++ = htonl(0x7c2903a6);					// mtctr	r1
	*lp++ = htonl(0x80200000 + XLM_IRQ_NEST);	// lwz		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x38210001);					// addi		r1,r1,1
	*lp++ = htonl(0x90200000 + XLM_IRQ_NEST);	// stw		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);// lwz		r1,XLM_KERNEL_DATA
	*lp++ = htonl(0x90c10018);					// stw		r6,0x18(r1)
	*lp++ = htonl(0x7cc902a6);					// mfctr	r6
	*lp++ = htonl(0x90c10004);					// stw		r6,$0004(r1)
	*lp++ = htonl(0x80c1065c);					// lwz		r6,$065c(r1)
	*lp++ = htonl(0x90e6013c);					// stw		r7,$013c(r6)
	*lp++ = htonl(0x91060144);					// stw		r8,$0144(r6)
	*lp++ = htonl(0x9126014c);					// stw		r9,$014c(r6)
	*lp++ = htonl(0x91460154);					// stw		r10,$0154(r6)
	*lp++ = htonl(0x9166015c);					// stw		r11,$015c(r6)
	*lp++ = htonl(0x91860164);					// stw		r12,$0164(r6)
	*lp++ = htonl(0x91a6016c);					// stw		r13,$016c(r6)
	*lp++ = htonl(0x7da00026);					// mfcr		r13
	*lp++ = htonl(0x80e10660);					// lwz		r7,$0660(r1)
	*lp++ = htonl(0x7d8802a6);					// mflr		r12
	*lp++ = htonl(0x50e74001);					// rlwimi.	r7,r7,8,$80000000
	*lp++ = htonl(0x814105f0);					// lwz		r10,0x05f0(r1)
	*lp++ = htonl(0x7d4803a6);					// mtlr		r10
	*lp++ = htonl(0x7d8a6378);					// mr		r10,r12
	*lp++ = htonl(0x3d600002);					// lis		r11,0x0002
	*lp++ = htonl(0x616bf072);					// ori		r11,r11,0xf072 (MSR)
	*lp++ = htonl(0x50e7deb4);					// rlwimi	r7,r7,27,$00000020
	*lp = htonl(0x4e800020);					// blr

	// Extra routine for Mixed Mode
	lp = (uint32 *)(ROMBaseHost + 0x36fa00);
	*lp++ = htonl(0x7c2903a6);					// mtctr	r1
	*lp++ = htonl(0x80200000 + XLM_IRQ_NEST);	// lwz		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x38210001);					// addi		r1,r1,1
	*lp++ = htonl(0x90200000 + XLM_IRQ_NEST);	// stw		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);// lwz		r1,XLM_KERNEL_DATA
	*lp++ = htonl(0x90c10018);					// stw		r6,0x18(r1)
	*lp++ = htonl(0x7cc902a6);					// mfctr	r6
	*lp++ = htonl(0x90c10004);					// stw		r6,$0004(r1)
	*lp++ = htonl(0x80c1065c);					// lwz		r6,$065c(r1)
	*lp++ = htonl(0x90e6013c);					// stw		r7,$013c(r6)
	*lp++ = htonl(0x91060144);					// stw		r8,$0144(r6)
	*lp++ = htonl(0x9126014c);					// stw		r9,$014c(r6)
	*lp++ = htonl(0x91460154);					// stw		r10,$0154(r6)
	*lp++ = htonl(0x9166015c);					// stw		r11,$015c(r6)
	*lp++ = htonl(0x91860164);					// stw		r12,$0164(r6)
	*lp++ = htonl(0x91a6016c);					// stw		r13,$016c(r6)
	*lp++ = htonl(0x7da00026);					// mfcr		r13
	*lp++ = htonl(0x80e10660);					// lwz		r7,$0660(r1)
	*lp++ = htonl(0x7d8802a6);					// mflr		r12
	*lp++ = htonl(0x50e74001);					// rlwimi.	r7,r7,8,$80000000
	*lp++ = htonl(0x814105f4);					// lwz		r10,0x05f4(r1)
	*lp++ = htonl(0x7d4803a6);					// mtlr		r10
	*lp++ = htonl(0x7d8a6378);					// mr		r10,r12
	*lp++ = htonl(0x3d600002);					// lis		r11,0x0002
	*lp++ = htonl(0x616bf072);					// ori		r11,r11,0xf072 (MSR)
	*lp++ = htonl(0x50e7deb4);					// rlwimi	r7,r7,27,$00000020
	*lp = htonl(0x4e800020);					// blr

	// Extra routine for Reset/FC1E opcode
	lp = (uint32 *)(ROMBaseHost + 0x36fb00);
	*lp++ = htonl(0x7c2903a6);					// mtctr	r1
	*lp++ = htonl(0x80200000 + XLM_IRQ_NEST);	// lwz		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x38210001);					// addi		r1,r1,1
	*lp++ = htonl(0x90200000 + XLM_IRQ_NEST);	// stw		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);// lwz		r1,XLM_KERNEL_DATA
	*lp++ = htonl(0x90c10018);					// stw		r6,0x18(r1)
	*lp++ = htonl(0x7cc902a6);					// mfctr	r6
	*lp++ = htonl(0x90c10004);					// stw		r6,$0004(r1)
	*lp++ = htonl(0x80c1065c);					// lwz		r6,$065c(r1)
	*lp++ = htonl(0x90e6013c);					// stw		r7,$013c(r6)
	*lp++ = htonl(0x91060144);					// stw		r8,$0144(r6)
	*lp++ = htonl(0x9126014c);					// stw		r9,$014c(r6)
	*lp++ = htonl(0x91460154);					// stw		r10,$0154(r6)
	*lp++ = htonl(0x9166015c);					// stw		r11,$015c(r6)
	*lp++ = htonl(0x91860164);					// stw		r12,$0164(r6)
	*lp++ = htonl(0x91a6016c);					// stw		r13,$016c(r6)
	*lp++ = htonl(0x7da00026);					// mfcr		r13
	*lp++ = htonl(0x80e10660);					// lwz		r7,$0660(r1)
	*lp++ = htonl(0x7d8802a6);					// mflr		r12
	*lp++ = htonl(0x50e74001);					// rlwimi.	r7,r7,8,$80000000
	*lp++ = htonl(0x814105f8);					// lwz		r10,0x05f8(r1)
	*lp++ = htonl(0x7d4803a6);					// mtlr		r10
	*lp++ = htonl(0x7d8a6378);					// mr		r10,r12
	*lp++ = htonl(0x3d600002);					// lis		r11,0x0002
	*lp++ = htonl(0x616bf072);					// ori		r11,r11,0xf072 (MSR)
	*lp++ = htonl(0x50e7deb4);					// rlwimi	r7,r7,27,$00000020
	*lp = htonl(0x4e800020);					// blr

	// Extra routine for FE0A opcode (QuickDraw 3D needs this)
	lp = (uint32 *)(ROMBaseHost + 0x36fc00);
	*lp++ = htonl(0x7c2903a6);					// mtctr	r1
	*lp++ = htonl(0x80200000 + XLM_IRQ_NEST);	// lwz		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x38210001);					// addi		r1,r1,1
	*lp++ = htonl(0x90200000 + XLM_IRQ_NEST);	// stw		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);// lwz		r1,XLM_KERNEL_DATA
	*lp++ = htonl(0x90c10018);					// stw		r6,0x18(r1)
	*lp++ = htonl(0x7cc902a6);					// mfctr	r6
	*lp++ = htonl(0x90c10004);					// stw		r6,$0004(r1)
	*lp++ = htonl(0x80c1065c);					// lwz		r6,$065c(r1)
	*lp++ = htonl(0x90e6013c);					// stw		r7,$013c(r6)
	*lp++ = htonl(0x91060144);					// stw		r8,$0144(r6)
	*lp++ = htonl(0x9126014c);					// stw		r9,$014c(r6)
	*lp++ = htonl(0x91460154);					// stw		r10,$0154(r6)
	*lp++ = htonl(0x9166015c);					// stw		r11,$015c(r6)
	*lp++ = htonl(0x91860164);					// stw		r12,$0164(r6)
	*lp++ = htonl(0x91a6016c);					// stw		r13,$016c(r6)
	*lp++ = htonl(0x7da00026);					// mfcr		r13
	*lp++ = htonl(0x80e10660);					// lwz		r7,$0660(r1)
	*lp++ = htonl(0x7d8802a6);					// mflr		r12
	*lp++ = htonl(0x50e74001);					// rlwimi.	r7,r7,8,$80000000
	*lp++ = htonl(0x814105fc);					// lwz		r10,0x05fc(r1)
	*lp++ = htonl(0x7d4803a6);					// mtlr		r10
	*lp++ = htonl(0x7d8a6378);					// mr		r10,r12
	*lp++ = htonl(0x3d600002);					// lis		r11,0x0002
	*lp++ = htonl(0x616bf072);					// ori		r11,r11,0xf072 (MSR)
	*lp++ = htonl(0x50e7deb4);					// rlwimi	r7,r7,27,$00000020
	*lp = htonl(0x4e800020);					// blr

	// Extra routine for FE0F opcode (power management)
	lp = (uint32 *)(ROMBaseHost + 0x36fd00);
	*lp++ = htonl(0x7c2903a6);					// mtctr	r1
	*lp++ = htonl(0x80200000 + XLM_IRQ_NEST);	// lwz		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x38210001);					// addi		r1,r1,1
	*lp++ = htonl(0x90200000 + XLM_IRQ_NEST);	// stw		r1,XLM_IRQ_NEST
	*lp++ = htonl(0x80200000 + XLM_KERNEL_DATA);// lwz		r1,XLM_KERNEL_DATA
	*lp++ = htonl(0x90c10018);					// stw		r6,0x18(r1)
	*lp++ = htonl(0x7cc902a6);					// mfctr	r6
	*lp++ = htonl(0x90c10004);					// stw		r6,$0004(r1)
	*lp++ = htonl(0x80c1065c);					// lwz		r6,$065c(r1)
	*lp++ = htonl(0x90e6013c);					// stw		r7,$013c(r6)
	*lp++ = htonl(0x91060144);					// stw		r8,$0144(r6)
	*lp++ = htonl(0x9126014c);					// stw		r9,$014c(r6)
	*lp++ = htonl(0x91460154);					// stw		r10,$0154(r6)
	*lp++ = htonl(0x9166015c);					// stw		r11,$015c(r6)
	*lp++ = htonl(0x91860164);					// stw		r12,$0164(r6)
	*lp++ = htonl(0x91a6016c);					// stw		r13,$016c(r6)
	*lp++ = htonl(0x7da00026);					// mfcr		r13
	*lp++ = htonl(0x80e10660);					// lwz		r7,$0660(r1)
	*lp++ = htonl(0x7d8802a6);					// mflr		r12
	*lp++ = htonl(0x50e74001);					// rlwimi.	r7,r7,8,$80000000
	*lp++ = htonl(0x81410604);					// lwz		r10,0x0604(r1)
	*lp++ = htonl(0x7d4803a6);					// mtlr		r10
	*lp++ = htonl(0x7d8a6378);					// mr		r10,r12
	*lp++ = htonl(0x3d600002);					// lis		r11,0x0002
	*lp++ = htonl(0x616bf072);					// ori		r11,r11,0xf072 (MSR)
	*lp++ = htonl(0x50e7deb4);					// rlwimi	r7,r7,27,$00000020
	*lp = htonl(0x4e800020);					// blr

	// Patch DR emulator to jump to right address when an interrupt occurs
	{
	bool dr_found_flag = false;
	lp = (uint32 *)(ROMBaseHost + 0x370000);
	while (lp < (uint32 *)(ROMBaseHost + 0x380000)) {
		if (ntohl(*lp) == 0x4ca80020) {		// bclr		5,8
			dr_found_flag = true;
			break;
		}
		lp++;
	}
	if (!dr_found_flag) {
		if (!g_rom_904_lenient) {
			D(bug("DR emulator patch location not found\n"));
			return false;
		}
		fprintf(stderr, "[ROMPATCH] SKIP dr_emulator (bclr 5,8 absent in parcels)\n");
	} else {
	lp++;
	loc = (uintptr)lp - (uintptr)ROMBaseHost;
	if ((base = rom_powerpc_branch_target(loc)) == 0) base = loc;
	static const uint8 dr_ret_dat[] = {0x80, 0xbf, 0x08, 0x14, 0x53, 0x19, 0x4d, 0xac, 0x7c, 0xa8, 0x03, 0xa6};
	base = find_rom_data(base, 0x380000, dr_ret_dat, sizeof(dr_ret_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("dr_ret %08lx\n", base));
	if (base != loc) {
		// OldWorld ROMs contain an absolute branch
		D(bug(" patching absolute branch at %08x\n", loc));
		*lp = htonl(0x48000000 + 0xf000 - (loc & 0xffff));				// b	DR_CACHE_BASE+0x1f000
		lp = (uint32 *)(ROMBaseHost + 0x37f000);
		*lp++ = htonl(0x3c000000 + ((ROMBase + base) >> 16));			// lis	r0,xxx
		*lp++ = htonl(0x60000000 + ((ROMBase + base) & 0xffff));		// ori	r0,r0,xxx
		*lp++ = htonl(0x7c0803a6);										// mtlr	r0
		*lp = htonl(POWERPC_BLR);										// blr
	}
	} else fprintf(stderr, "[ROMPATCH] SKIP dr_ret (absent in parcels)\n");
	}
	}
	return true;
}


/*
 *  Nanokernel patches
 */

static bool patch_nanokernel(void)
{
	// Lenient mode (g_rom_904_lenient): pattern misses skip-with-warning instead of aborting.
	uint32 *lp;
	uint32 base, loc;

	// Patch Mixed Mode trap
	static const uint8 virt2phys_dat[] = {0x7d, 0x1b, 0x43, 0x78, 0x3b, 0xa1, 0x03, 0x20};
	base = find_rom_data(0x313000, 0x314000, virt2phys_dat, sizeof(virt2phys_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("virt2phys %08lx\n", base + 8));
	lp = (uint32 *)(ROMBaseHost + base + 8);	// Don't translate virtual->physical
	lp[0] = htonl(0x7f7fdb78);					// mr		r31,r27
	lp[2] = htonl(POWERPC_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP virt2phys (absent in parcels)\n");

	// ppc_excp_tbl: sets XLM_RUN_MODE=MODE_NATIVE when entering PPC exception table.
	// Absent in 9.0.1+ ROMs — MODE_NATIVE is permanently dead for NewWorld.
	static const uint8 ppc_excp_tbl_dat[] = {0x39, 0x01, 0x04, 0x20, 0x7d, 0x13, 0x43, 0xa6};
	base = find_rom_data(0x313000, 0x314000, ppc_excp_tbl_dat, sizeof(ppc_excp_tbl_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("ppc_excp_tbl %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);		// Don't activate PPC exception table
	*lp++ = htonl(0x39000000 + MODE_NATIVE);	// li	r8,MODE_NATIVE
	*lp = htonl(0x91000000 + XLM_RUN_MODE);		// stw	r8,XLM_RUN_MODE
	} else fprintf(stderr, "[ROMPATCH] SKIP ppc_excp_tbl (absent in parcels)\n");

	static const uint8 save_fpu_dat[] = {0x7d, 0x00, 0x00, 0xa6, 0x61, 0x08, 0x20, 0x00, 0x7d, 0x00, 0x01, 0x24};
	base = find_rom_data(0x310000, 0x314000, save_fpu_dat, sizeof(save_fpu_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("save_fpu %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);		// Don't modify MSR to turn on FPU
	if (ntohl(lp[4]) != 0x556b04e2) return false;
	loc = base;
#if 1
	// FIXME: is that really intended?
	*lp++ = htonl(POWERPC_NOP);
	lp++;
	*lp++ = htonl(POWERPC_NOP);
	lp++;
	*lp = htonl(POWERPC_NOP);
#else
	lp[0] = htonl(POWERPC_NOP);
	lp[1] = htonl(POWERPC_NOP);
	lp[2] = htonl(POWERPC_NOP);
	lp[3] = htonl(POWERPC_NOP);
#endif

	static const uint8 save_fpu_caller_dat[] = {0x93, 0xa6, 0x01, 0xec, 0x93, 0xc6, 0x01, 0xf4, 0x93, 0xe6, 0x01, 0xfc, 0x40};
	if ((base = find_rom_data(0x310000, 0x314000, save_fpu_caller_dat, sizeof(save_fpu_caller_dat))) == 0) return false;
	D(bug("save_fpu_caller %08lx\n", base + 12));
	if (rom_powerpc_branch_target(base + 12) != loc) return false;
	lp = (uint32 *)(ROMBaseHost + base + 12);	// Always save FPU state
	*lp = htonl(0x48000000 | (ntohl(*lp) & 0xffff));	// bl	0x00312e88
	} else fprintf(stderr, "[ROMPATCH] SKIP save_fpu/save_fpu_caller (absent in parcels)\n");

	static const uint8 mdec_dat[] = {0x7f, 0xf6, 0x02, 0xa6, 0x2c, 0x08, 0x00, 0x00, 0x93, 0xe1, 0x06, 0x68, 0x7d, 0x16, 0x03, 0xa6};
	base = find_rom_data(0x310000, 0x314000, mdec_dat, sizeof(mdec_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	if (MachineProfileIsNewWorld()) {
		// M2 (ROM-PATCH-AUDIT: mdec_dat RETIRE@M2): the virtual clock honors
		// mtspr/mfspr DEC, so the nanokernel's decrementer programming must run
		// and reach the clock - the patch would make the M2 clock dead code.
		// Paravirtual keeps the patch. NOTE: this pattern exists only in
		// 1.1/OldWorld NKs (ROM ~0x312c24); 9.0.x parcels ROMs never match
		// (base==0 -> the SKIP note below) - the gate is live only for
		// 1.1-ROM newworld configs.
		fprintf(stderr, "[M2] mdec nanokernel patch retired (newworld profile): guest mtdec/mfdec reach the virtual clock\n");
	} else {
	D(bug("mdec %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);		// Don't modify DEC
	lp[0] = htonl(0x3be00000);					// li	r31,0
#if 1
	lp[3] = htonl(POWERPC_NOP);
	lp[4] = htonl(POWERPC_NOP);
#else
	lp[3] = htonl(0x39000040);					// li	r8,0x40
	lp[4] = htonl(0x990600e4);					// stb	r8,0xe4(r6)
#endif
	}
	} else fprintf(stderr, "[ROMPATCH] SKIP mdec (decrementer neutralize — absent in 9.0.x parcels ROMs; pattern is OldWorld/1.1-NK)\n");

	static const uint8 restore_fpu_caller_dat[] = {0x81, 0x06, 0x00, 0xf4, 0x81, 0x46, 0x00, 0xfc, 0x7d, 0x09, 0x03, 0xa6, 0x40};
	base = find_rom_data(0x310000, 0x314000, restore_fpu_caller_dat, sizeof(restore_fpu_caller_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("restore_fpu_caller %08lx\n", base + 12));
	lp = (uint32 *)(ROMBaseHost + base + 12);	// Always restore FPU state
	*lp = htonl(0x48000000 | (ntohl(*lp) & 0xffff));	// bl	0x00312ddc
	} else fprintf(stderr, "[ROMPATCH] SKIP restore_fpu_caller (absent in parcels)\n");

	// m68k_excp_tbl: sets XLM_RUN_MODE=MODE_68K when entering 68k exception table.
	// Absent in 9.0.1+ ROMs — paired with ppc_excp_tbl (both dead for NewWorld).
	static const uint8 m68k_excp_tbl_dat[] = {0x81, 0x21, 0x06, 0x58, 0x39, 0x01, 0x03, 0x60, 0x7d, 0x13, 0x43, 0xa6};
	base = find_rom_data(0x310000, 0x314000, m68k_excp_tbl_dat, sizeof(m68k_excp_tbl_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("m68k_excp %08lx\n", base + 4));
	lp = (uint32 *)(ROMBaseHost + base + 4);	// Don't activate 68k exception table
	*lp++ = htonl(0x39000000 + MODE_68K);		// li	r8,MODE_68K
	*lp = htonl(0x91000000 + XLM_RUN_MODE);		// stw	r8,XLM_RUN_MODE
	} else fprintf(stderr, "[ROMPATCH] SKIP m68k_excp_tbl (absent in parcels)\n");

	// Patch 68k emulator trap routine
	static const uint8 restore_fpu_caller2_dat[] = {0x81, 0x86, 0x00, 0x8c, 0x80, 0x66, 0x00, 0x94, 0x80, 0x86, 0x00, 0x9c, 0x40};
	base = find_rom_data(0x310000, 0x314000, restore_fpu_caller2_dat, sizeof(restore_fpu_caller2_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("restore_fpu_caller2 %08lx\n", base + 12));
	loc = rom_powerpc_branch_target(base + 12);
	lp = (uint32 *)(ROMBaseHost + base + 12);	// Always restore FPU state
	*lp = htonl(0x48000000 | (ntohl(*lp) & 0xffff));	// bl	0x00312dd4

	static const uint8 restore_fpu_dat[] = {0x55, 0x68, 0x04, 0xa5, 0x4c, 0x82, 0x00, 0x20, 0x81, 0x06, 0x00, 0xe4};
	if ((base = find_rom_data(0x310000, 0x314000, restore_fpu_dat, sizeof(restore_fpu_dat))) == 0) return false;
	D(bug("restore_fpu %08lx\n", base));
	if (base != loc) return false;
	lp = (uint32 *)(ROMBaseHost + base + 4);	// Don't modify MSR to turn on FPU
	*lp++ = htonl(POWERPC_NOP);
	lp += 2;
	*lp++ = htonl(POWERPC_NOP);
	lp++;
	*lp++ = htonl(POWERPC_NOP);
	*lp++ = htonl(POWERPC_NOP);
	*lp = htonl(POWERPC_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP restore_fpu_caller2/restore_fpu (absent in parcels)\n");

	// Disable suspend (FE0F opcode)
	// TODO: really suspend SheepShaver?
	static const uint8 suspend_dat[] = {0x7c, 0x88, 0x68, 0x39, 0x41, 0x9d};
	base = find_rom_data(0x315000, 0x316000, suspend_dat, sizeof(suspend_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("suspend %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base + 4);
	*lp = htonl((ntohl(*lp) & 0xffff) | 0x48000000);	// bgt -> b
	} else fprintf(stderr, "[ROMPATCH] SKIP suspend (absent in 9.0.4)\n");

	// Patch trap return routine
	static const uint8 trap_return_dat[] = {0x80, 0xc1, 0x00, 0x18, 0x80, 0x21, 0x00, 0x04, 0x4c, 0x00, 0x00, 0x64};
	base = find_rom_data(0x312000, 0x320000, trap_return_dat, sizeof(trap_return_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("trap_return %08lx\n", base + 8));
	lp = (uint32 *)(ROMBaseHost + base + 8);	// Replace rfi
	*lp = htonl(POWERPC_BCTR);

	// M8 Task A: riser_stub_off is THE stub-location constant (single source —
	// g_exc_riser_window below derives from it and from this patch's own
	// emission extent; no second hand-copied copy of the window exists).
	const uint32 riser_stub_off = 0x318000;
	while (ntohl(*lp) != 0x7d5a03a6) lp--;
	*lp++ = htonl(0x7d4903a6);					// mtctr	r10
	*lp++ = htonl(0x7daff120);					// mtcr	r13
	*lp = htonl(0x48000000 + ((riser_stub_off - ((uintptr)lp - (uintptr)ROMBaseHost)) & 0x03fffffc));	// b		ROMBase+0x318000
	uint32 npc = (uintptr)(lp + 1) - (uintptr)ROMBaseHost;

	lp = (uint32 *)(ROMBaseHost + riser_stub_off);
	*lp++ = htonl(0x81400000 + XLM_IRQ_NEST);	// lwz	r10,XLM_IRQ_NEST
	*lp++ = htonl(0x394affff);					// subi	r10,r10,1
	*lp++ = htonl(0x91400000 + XLM_IRQ_NEST);	// stw	r10,XLM_IRQ_NEST
	const char *riser_env = getenv("SS_NW_EE_RISER");
	if (MachineProfileIsNewWorld() && !(riser_env && strcmp(riser_env, "0") == 0)) {
		// W2-4 step 1: the EE riser — NEWWORLD DEFAULT since the M7 Task C
		// cluster flip (with SS_NW_DEC_PUBLISHED + SS_NW_HOST_IRQ; this is
		// the flip W2-4 reserved as its final acceptance). Opt-out with
		// SS_NW_EE_RISER=0 (explicit-"0"-only, the SS_NW_SC_SURFACE polarity
		// — NOT MachineEnvFlag, which reads unset as off).
		// EE-only MSR compose consuming r11, the SRR1
		// image the raw tail would have latched via mtspr SRR1,r11 (EE-CHAIN-RECON
		// "W2-4 entry decision" D-1, [PROBE check] r11=0xd032 survives to this stub).
		// MSR := (MSR & ~0x8000) | (r11 & 0x8000) — per coordinator sign-off item 3
		// (EE-only first; full-image mtmsr r11 is the follow-up shape). r10 is dead
		// here (value already in CTR via the patched mtctr r10; nest use above is
		// its last read) and the reload region at npc reloads r10-r13 from ctx(r6)
		// [PATCH evidence: 0x3244fc lwz r10,0x154(r6) / 0x324500 lwz r11,0x15c(r6)].
		// mtmsr is interpreter-executed, block-ending, and carries the EE 0->1
		// edge re-raise (ppc-execute.cpp execute_mtmsr) — delivery rides the exact
		// machinery W2-0/W2-1 proved; no new delivery mechanism.
		// Word budget (sign-off item 5, verified against rom901_inventory.bin
		// [RAW-ROM]): 0x318000 sits in the NK AltiVec element-load thunk table
		// (8-byte lvebx vN + b pairs, 0x317e20..0x318628); the upstream 4-word stub
		// already clobbers the v28/v29 entries, these 3 words extend into v30 +
		// the first word of v31 — same dispatcher, same reachability class as the
		// long-accepted upstream clobber.
		*lp++ = htonl(0x7d4000a6);				// mfmsr	r10
		*lp++ = htonl(0x516a0420);				// rlwimi	r10,r11,0,16,16 (insert MSR[EE]=0x8000 from r11)
		*lp++ = htonl(0x7d400124);				// mtmsr	r10
		g_exc_riser_window.armed = 1;			// M8 Task A: riser-armed, recorded at the gate's ONE eval site
		fprintf(stderr, "[ROMPATCH] trap_return EE riser ARMED (newworld default; opt-out SS_NW_EE_RISER=0): stub 0x318000 = 7 words, EE-only compose from r11\n");
	}
	*lp = htonl(0x48000000 + ((npc - ((uintptr)lp - (uintptr)ROMBaseHost)) & 0x03fffffc));	// b		reload region (npc)
	// M8 slot-4 consumption Task A: export the riser/tail windows for the
	// deferred-EE-edge rfi-atomicity emulation (single source per the
	// coordinator ACK: derived from the values emitted RIGHT HERE — the stub
	// extent [riser_stub_off, end-of-emission), the reload region [npc,
	// base+12) whose exclusive end is one word PAST the bctr that replaced
	// the rfi at base+8). Guest addresses. Primary copy only — the staged
	// +0x100000 NK copy has zero visits (EE-CHAIN-RECON.md D-1), no staged
	// window; revisit only if a staged-tail visit is ever observed.
	g_exc_riser_window.stub_base    = ROMBase + riser_stub_off;
	g_exc_riser_window.stub_end     = ROMBase + (uint32)((uintptr)(lp + 1) - (uintptr)ROMBaseHost);
	g_exc_riser_window.reload_start = ROMBase + npc;
	g_exc_riser_window.reload_end   = ROMBase + base + 12;
	// Log only when the consumer is enabled — gated-off boots keep a
	// byte-identical behavior-line set vs the Task-0 entry baseline.
	if (g_exc_riser_window.armed && ExcIrqConsumeEnabled())
		fprintf(stderr, "[ROMPATCH] riser windows (deferred-EE-edge source of truth): stub %08x-%08x reload %08x-%08x\n",
		        g_exc_riser_window.stub_base, g_exc_riser_window.stub_end,
		        g_exc_riser_window.reload_start, g_exc_riser_window.reload_end);
	} else fprintf(stderr, "[ROMPATCH] SKIP trap_return (absent in parcels)\n");

	// M8 Task B (Q-C3, slot-4 consumption plan): the from-emulator interrupt-post
	// leg's image-selection defect. The NK post body (9.0.1 NK, [RAW-ROM]==[PATCH]
	// verified in this window, rom901.bin md5 d1a267a9 == rom901_inventory.bin):
	//   0x325518  rlwinm. r8,r7,0,10,10     ; from-emulator test (bit 0x00200000)
	//   0x32551c  beq     0x32562c          ; NOT from emulator -> deferred leg
	//   0x325520  sth     r28,0(r23)        ; post halfword (level|0x8000) -> [[KDP+0x67c]]
	//   0x325524  or      r13,r13,r31       ; CR arm -> VOLATILE working r13 ONLY  <- defect
	//   0x325528  bgt     cr7,0x325530
	//   0x32552c  and     r13,r13,r29       ; level-0 and-clear leg
	// Any ctx-reloading exit path (scheduler restore lwz r13,0xdc(r6) @0x324658/
	// 0x3246f8) DISCARDS the working-r13 arm — Task-0 Q-C3: shape B's lost arm, and
	// post-Task-A the blocking defect (clean deliveries land outside the DR, the arm
	// never reaches a live DR context). The NK's OWN deferred leg (the donor,
	// 0x325668-0x32568c) survives such exits: it stages the deferred pair
	// ([KDP-0x440] CR mask | [KDP-0x43c] halfword) AND sets task-flag bit 0x10
	// (+0x64 of [KDP-0x8f0]), which the scheduler-restore drain 0x324720-0x324750
	// consumes (re-posts the halfword, re-ORs the mask into the freshly reloaded
	// r13, resets the 0xffff sentinel). Fix (ordering, donor-faithful — Task-0's
	// pinned mechanism, coordinator-ACKed Task B scope): detour the from-emulator
	// leg's `or r13,r13,r31` through a patch-space stub that ALSO stages the
	// deferred pair + task flag, mirroring the donor word-for-word (register
	// discipline incl. the r31 task-ptr reuse is the donor's own; r8 is dead at
	// the return point — reloaded at 0x325534 before any read; cr0 is dead after
	// the beq at 0x32551c consumed it). Direct post + working-r13 OR stay (the
	// fast non-reloading exits still need them); on a reloading exit the drain
	// re-applies the SAME halfword value (idempotent) + the mask. Known design
	// note: on a fast exit the staged pair stays pending until the flagged task's
	// next scheduler switch-in, whose drain re-posts an already-retired doorbell
	// once (spurious-but-benign duplicate poll; the VIA chain dismisses an empty
	// poll). GUEST-side writes only — the fake-poke fence is untouched (no host
	// writes to per-event state; this changes what the GUEST's own post leg does).
	if (MachineProfileIsNewWorld() && ExcIrqConsumeEnabled()) {
		// verify-EXPECTED-first (tmtask precedent): the exact 6-word leg, incl.
		// both branch displacements — any layout drift = loud skip, no patch.
		static const uint8 irq_post_leg_dat[] = {
			0x54, 0xe8, 0x02, 0x95,		// rlwinm. r8,r7,0,10,10
			0x41, 0x82, 0x01, 0x10,		// beq     +0x110 (deferred leg)
			0xb3, 0x97, 0x00, 0x00,		// sth     r28,0(r23)
			0x7d, 0xad, 0xfb, 0x78,		// or      r13,r13,r31
			0x41, 0x9d, 0x00, 0x08,		// bgt     cr7,+8
			0x7d, 0xad, 0xe8, 0x38};	// and     r13,r13,r29
		base = find_rom_data(0x320000, 0x328000, irq_post_leg_dat, sizeof(irq_post_leg_dat));
		if (base == 0) {
			fprintf(stderr, "[ROMPATCH] irq_post_stage GUARDED-SKIP (from-emulator post leg pattern absent — NK layout moved; Q-C3 arm loss stands on this ROM)\n");
		} else if (!check_rom_patch_space(IRQ_POST_PATCH_SPACE, 0x40)) {
			fprintf(stderr, "[ROMPATCH] irq_post_stage GUARDED-SKIP (patch space 0x%x not free — layout moved)\n", IRQ_POST_PATCH_SPACE);
		} else {
			D(bug("irq_post_stage %08lx\n", base + 12));
			// The stub: displaced original + deferred-pair staging + task flag
			// (donor 0x325668-0x32568c word-for-word, retargeted scratch r8).
			lp = (uint32 *)(ROMBaseHost + IRQ_POST_PATCH_SPACE);
			*lp++ = htonl(0x7dadfb78);		// or    r13,r13,r31      (displaced original)
			*lp++ = htonl(0x8101fbc0);		// lwz   r8,-0x440(r1)    ([KDP-0x440] deferred CR mask)
			*lp++ = htonl(0x7d08fb78);		// or    r8,r8,r31
			*lp++ = htonl(0x9101fbc0);		// stw   r8,-0x440(r1)
			*lp++ = htonl(0xb381fbc4);		// sth   r28,-0x43c(r1)   ([KDP-0x43c] deferred halfword)
			*lp++ = htonl(0x83e1f710);		// lwz   r31,-0x8f0(r1)   (current task ptr — donor's own idiom @0x325678)
			*lp++ = htonl(0x2c1f0000);		// cmpwi r31,0            (defensive: no task yet -> skip flag; cr0 dead here)
			*lp++ = htonl(0x41820010);		// beq   +0x10            (-> the branch back)
			*lp++ = htonl(0x811f0064);		// lwz   r8,0x64(r31)
			*lp++ = htonl(0x61080010);		// ori   r8,r8,0x10       (task-flag bit 0x10 = the drain's enable, 0x3246a4)
			*lp++ = htonl(0x911f0064);		// stw   r8,0x64(r31)
			*lp = htonl(0x48000000 + (((base + 16) - ((uintptr)lp - (uintptr)ROMBaseHost)) & 0x03fffffc));	// b  back (bgt cr7 @base+16)
			// The detour: replace the or at base+12 LAST (stub is complete first)
			lp = (uint32 *)(ROMBaseHost + base + 12);
			*lp = htonl(0x48000000 + ((IRQ_POST_PATCH_SPACE - (base + 12)) & 0x03fffffc));	// b  stub
			fprintf(stderr, "[ROMPATCH] irq_post_stage ARMED (Q-C3): from-emulator post leg @%08x detours via 0x%x — stages deferred pair [KDP-0x440]/[KDP-0x43c] + task-flag 0x10 (donor 0x325668-0x32568c)\n",
			        (uint32)(base + 12), IRQ_POST_PATCH_SPACE);
		}
	}

	// Patch FEOA opcode, selector 0x0A (virtual->physical page index)
	static const uint8 fe0a_0a_dat[] = {0x55, 0x23, 0xa3, 0x3e, 0x4b};
	base = find_rom_data(0x314000, 0x318000, fe0a_0a_dat, sizeof(fe0a_0a_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	loc = rom_powerpc_branch_target(base - 8);
	static const uint8 fe0a_dat[] = {0x7e, 0x04, 0x48, 0x40, 0x81, 0xe1, 0x06, 0xb0, 0x54, 0x88, 0x10, 0x3a, 0x40, 0x90};
	if (find_rom_data(loc, 0x318000, fe0a_dat, sizeof(fe0a_dat)) != loc) return false;
	D(bug("fe0a_0a %08lx\n", base - 8));
	lp = (uint32 *)(ROMBaseHost + base - 8);
	*lp++ = htonl(0x7c832378);					// mr	r3,r4
	*lp++ = htonl(POWERPC_NOP);
	*lp = htonl(POWERPC_NOP);

	// Disable FE0A opcode, selector 0x11 (init page tables?)
	static const uint8 fe0a_11_dat[] = {0x56, 0x07, 0x06, 0x74, 0x2c, 0x07, 0x00, 0x60, 0x40};
	if ((base = find_rom_data(0x314000, 0x318000, fe0a_11_dat, sizeof(fe0a_11_dat))) == 0) return false;
	loc = rom_powerpc_branch_target(base - 4);
	if (find_rom_data(0x314000, 0x318000, fe0a_dat, sizeof(fe0a_dat)) != loc) return false;
	D(bug("fe0a_11 %08lx\n", base - 4));
	lp = (uint32 *)(ROMBaseHost + base - 4);
	*lp++ = htonl(POWERPC_NOP);
	*lp++ = htonl(POWERPC_NOP);
	*lp++ = htonl(POWERPC_NOP);
	*lp = htonl(ntohl(*lp) | 0x02800000);		// bf => ba
	} else fprintf(stderr, "[ROMPATCH] SKIP fe0a_0a/fe0a_11 (absent in parcels)\n");

	// Patch FE0A opcode to fake a page table entry so that V=P for RAM and ROM
	static const uint8 pg_lookup_dat[] = {0x7e, 0x0f, 0x40, 0x6e, 0x81, 0xc1, 0x06, 0xa4, 0x7e, 0x00, 0x71, 0x20};
	base = find_rom_data(0x310000, 0x320000, pg_lookup_dat, sizeof(pg_lookup_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("fe0a_pgtb_lookup %08lx\n", base - 12));
	lp = (uint32 *)(ROMBaseHost + base - 12);
	if (ntohl(lp[0]) != 0x81e106b0)				// lwz	r15,$06b0(r1)
		return false;
	lp[0] = htonl(0x54906026);					// slwi	r16,r4,12
	lp[3] = htonl(0x62100121);					// ori	r16,r16,0x121
	} else fprintf(stderr, "[ROMPATCH] SKIP fe0a_pgtb_lookup (absent in parcels)\n");

	// Patch FE0A opcode to not write to kernel memory
	static const uint8 krnl_write_dat[] = {0x38, 0xe0, 0x00, 0x01, 0x7e, 0x10, 0x38, 0x78, 0x92, 0x0f, 0x00, 0x00};
	base = find_rom_data(0x310000, 0x320000, krnl_write_dat, sizeof(krnl_write_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("fe0a_krnl_write %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	lp[2] = htonl(POWERPC_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP fe0a_krnl_write (absent in parcels)\n");

/*
	// Disable FE0A/FE06 opcodes
	lp = (uint32 *)(ROMBase + 0x3144ac);
	*lp++ = htonl(POWERPC_NOP);
	*lp += 8;
*/
	return true;
}


/*
 *  68k boot routine patches
 */

static bool patch_68k(void)
{
	// Lenient mode (g_rom_904_lenient): pattern misses skip-with-warning instead of aborting.
	uint32 *lp;
	uint16 *wp;
	uint8 *bp;
	uint32 base, loc;

	// Remove 68k RESET instruction
	static const uint8 reset_dat[] = {0x4e, 0x70};
	base = find_rom_data(0xc8, 0x120, reset_dat, sizeof(reset_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("reset %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP reset (absent in parcels)\n");

	// Fake reading PowerMac ID (via Universal)
	static const uint8 powermac_id_dat[] = {0x45, 0xf9, 0x5f, 0xff, 0xff, 0xfc, 0x20, 0x12, 0x72, 0x00};
	base = find_rom_data(0xe000, 0x15000, powermac_id_dat, sizeof(powermac_id_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("powermac_id %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x203c);			// move.l	#id,d0
	*wp++ = htons(0);
//	if (ROMType == ROMTYPE_NEWWORLD)
//		*wp++ = htons(0x3035);		// (PowerMac 9500 ID)
//	else
		*wp++ = htons(0x3020);		// (PowerMac 9500 ID)
	*wp++ = htons(0xb040);			// cmp.w	d0,d0
	*wp = htons(0x4ed6);			// jmp	(a6)
	} else fprintf(stderr, "[ROMPATCH] SKIP powermac_id (absent in parcels)\n");

	// Patch UniversalInfo
	if (ROMType == ROMTYPE_NEWWORLD) {
		static const uint8 univ_info_dat[] = {0x3f, 0xff, 0x04, 0x00};
		base = find_rom_data(0x14000, 0x18000, univ_info_dat, sizeof(univ_info_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("universal_info %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base - 0x14);
		lp[0x00 >> 2] = htonl(ADDR_MAP_PATCH_SPACE - (base - 0x14));
		{
			if (MachineEnvFlag("SS_NW_MODEL")) {
				lp[0x10 >> 2] = htonl(0xcc009611);	// BoxFlag byte = 0x96 → $0CB2:$0CB3 = 0x0196 = 406
				lp[0x60 >> 2] = htonl(0x00000196);	// gestaltMachineType = 406
				fprintf(stderr, "[NW-MODEL] UniversalInfo BoxFlag=0x96 + gestaltMachineType=406\n");
			} else {
				lp[0x10 >> 2] = htonl(0xcc003d11);	// PowerMac 9500 UniversalInfo
				lp[0x60 >> 2] = htonl(0x0000003d);
			}
		}
		lp[0x14 >> 2] = htonl(0x3fff0401);
		lp[0x18 >> 2] = htonl(0x0300001c);
		lp[0x1c >> 2] = htonl(0x000108c4);
		lp[0x24 >> 2] = htonl(0xc301bf26);
		lp[0x28 >> 2] = htonl(0x00000861);
		lp[0x58 >> 2] = htonl(0x30200000);
		} else fprintf(stderr, "[ROMPATCH] SKIP universal_info (absent in parcels)\n");
	} else if (ROMType == ROMTYPE_ZANZIBAR) {
		base = 0x12b70;
		lp = (uint32 *)(ROMBaseHost + base - 0x14);
		lp[0x00 >> 2] = htonl(ADDR_MAP_PATCH_SPACE - (base - 0x14));
		lp[0x10 >> 2] = htonl(0xcc003d11);		// Make it like the PowerMac 9500 UniversalInfo
		lp[0x14 >> 2] = htonl(0x3fff0401);
		lp[0x18 >> 2] = htonl(0x0300001c);
		lp[0x1c >> 2] = htonl(0x000108c4);
		lp[0x24 >> 2] = htonl(0xc301bf26);
		lp[0x28 >> 2] = htonl(0x00000861);
		lp[0x58 >> 2] = htonl(0x30200000);
		lp[0x60 >> 2] = htonl(0x0000003d);
	} else if (ROMType == ROMTYPE_GOSSAMER) {
		base = 0x12d20;
		lp = (uint32 *)(ROMBaseHost + base - 0x14);
		lp[0x00 >> 2] = htonl(ADDR_MAP_PATCH_SPACE - (base - 0x14));
		lp[0x10 >> 2] = htonl(0xcc003d11);		// Make it like the PowerMac 9500 UniversalInfo
		lp[0x14 >> 2] = htonl(0x3fff0401);
		lp[0x18 >> 2] = htonl(0x0300001c);
		lp[0x1c >> 2] = htonl(0x000108c4);
		lp[0x24 >> 2] = htonl(0xc301bf26);
		lp[0x28 >> 2] = htonl(0x00000861);
		lp[0x58 >> 2] = htonl(0x30410000);
		// SS_NW_MODEL: present gestaltMachineType 406 (0x196) — the universal NewWorld
		// machine type — so Mac OS 9.2's startup disk check accepts this ROM.
		// Default: 0x3d (PowerMac 9500 era). 406 = all NewWorld Macs (B&W G3+).
		lp[0x60 >> 2] = htonl(0x0000003d);
	}

	// Construct AddrMap for NewWorld ROM
	if (ROMType == ROMTYPE_NEWWORLD || ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_GOSSAMER) {
		lp = (uint32 *)(ROMBaseHost + ADDR_MAP_PATCH_SPACE);
		memset(lp - 10, 0, 0x128);
		lp[-10] = htonl(0x0300001c);
		lp[-9] = htonl(0x000108c4);
		lp[-4] = htonl(0x00300000);
		lp[-2] = htonl(0x11010000);
		lp[-1] = htonl(0xf8000000);
		lp[0] = htonl(0xffc00000);
		lp[2] = htonl(0xf3016000);
		lp[3] = htonl(0xf3012000);
		lp[4] = htonl(0xf3012000);
		lp[24] = htonl(0xf3018000);
		lp[25] = htonl(0xf3010000);
		lp[34] = htonl(0xf3011000);
		lp[38] = htonl(0xf3015000);
		lp[39] = htonl(0xf3014000);
		lp[43] = htonl(0xf3000000);
		lp[48] = htonl(0xf8000000);
	}

	// Don't initialize VIA (via Universal)
	// M6a Wave 2 #4 (ROM-PATCH-AUDIT via_init cluster, was RETIRE@M3): retired
	// early on the fidelity profile. The audit's SPIKE-S3 note anticipated exactly
	// this trigger — "if the boot-time VIA init matters, the via_init cluster moves
	// to RETIRE@M1" — and the 68k boot's ~135k reads/s VIA poll spin demonstrates
	// it now matters: the spin may depend on init-programmed VIA state these skip
	// patches prevent from ever being written. The recon (M6A-WAVE2-SHIM-RECON.md
	// queue #4) verified the init's register offsets all land inside the via6522
	// model region (base 0xf3016000, +0x2000). Paravirtual keeps the skips.
	// Same idiom as scc_init: searches still run (find/skip telemetry stays
	// consistent), only the patching is gated.
	if (MachineProfileIsNewWorld()) {
		fprintf(stderr, "[M6a] via_init/via_init2/via_init3 ROM patches retired (newworld profile): "
		        "guest boot-time VIA init will run against the via6522 model\n");
	}
	static const uint8 via_init_dat[] = {0x08, 0x00, 0x00, 0x02, 0x67, 0x00, 0x00, 0x2c, 0x24, 0x68, 0x00, 0x08};
	base = find_rom_data(0xe000, 0x15000, via_init_dat, sizeof(via_init_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
	// retired ([M6a] line above)
	} else if (base) {
	D(bug("via_init %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base + 4);
	*wp = htons(0x6000);			// bra
	} else fprintf(stderr, "[ROMPATCH] SKIP via_init (absent in parcels)\n");

	static const uint8 via_init2_dat[] = {0x24, 0x68, 0x00, 0x08, 0x00, 0x12, 0x00, 0x30, 0x4e, 0x71};
	base = find_rom_data(0xa000, 0x10000, via_init2_dat, sizeof(via_init2_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
	// retired ([M6a] line above)
	} else if (base) {
	D(bug("via_init2 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(0x4ed6);			// jmp	(a6)
	} else fprintf(stderr, "[ROMPATCH] SKIP via_init2 (absent in parcels)\n");

	static const uint8 via_init3_dat[] = {0x22, 0x68, 0x00, 0x08, 0x28, 0x3c, 0x20, 0x00, 0x01, 0x00};
	base = find_rom_data(0xa000, 0x10000, via_init3_dat, sizeof(via_init3_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
	// retired ([M6a] line above)
	} else if (base) {
	D(bug("via_init3 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(0x4ed6);			// jmp	(a6)
	} else fprintf(stderr, "[ROMPATCH] SKIP via_init3 (absent in parcels)\n");

	// Don't RunDiags, get BootGlobs pointer directly
	if (ROMType == ROMTYPE_NEWWORLD) {
		static const uint8 run_diags_dat[] = {0x60, 0xff, 0x00, 0x0c};
		base = find_rom_data(0x110, 0x128, run_diags_dat, sizeof(run_diags_dat));
		bool run_diags_parcels_alt = false;
		if (base == 0 && g_rom_904_lenient) {
			// Parcels 9.0.x: BRA.L displacement differs (0x000A8C88 vs 0x000CA46E),
			// but the moveq/move.l pair 8 bytes after the BRA.L is the same as OldWorld.
			static const uint8 run_diags_alt[] = {0x74, 0x00, 0x2f, 0x0e};
			base = find_rom_data(0xd0, 0xf0, run_diags_alt, sizeof(run_diags_alt));
			if (base) {
				base -= 8; // BRA.L is 8 bytes before the moveq pattern (movea.l between)
				run_diags_parcels_alt = true;
				fprintf(stderr, "[ROMPATCH] parcels: run_diags via OldWorld-style pattern, BRA.L at %08lx\n", (unsigned long)base);
			}
		}
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("run_diags %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(0x4df9);			// lea	xxx,a6
		*wp++ = htons((RAMBase + RAMSize - 0x1c) >> 16);
		*wp = htons((RAMBase + RAMSize - 0x1c) & 0xffff);
		// [M6a Wave-2 #3] Parcels boot-code layout (M6A-WAVE2-SHIM-RECON.md queue row 3,
		// confirmed by static RE of /tmp/rom901.bin @0xd2-0x104): the BRA.L we just replaced
		// is followed at base+6 by `movea.l (a7),a6` (0x2C57) — RunDiags' contract there is
		// "BootGlobs at (a7)", so with RunDiags skipped it reloads a6 from zeroed low RAM
		// ([0x2600] = 0), clobbering the lea above. The 0 then flows: pushed at 0xe0,
		// re-read at 0xfa (`movea.l 0xc(a7),a6`), and the BootGlobs movem at 0xfe
		// (`movem.l d0-d1/d3-d4,-0x5a(a6)`) wild-writes ea=0xffffffa6 (the post-Hnfo crash,
		// pc=0x50468910). NOP the clobber so the lea's BootGlobs pointer survives.
		if (run_diags_parcels_alt) {
			wp = (uint16 *)(ROMBaseHost + base + 6);
			if (ntohs(*wp) == 0x2c57) {		// movea.l (a7),a6
				*wp = htons(M68K_NOP);
				fprintf(stderr, "[ROMPATCH] parcels: run_diags a6-clobber (movea.l (a7),a6) NOPed at %08lx\n", (unsigned long)(base + 6));
			} else
				fprintf(stderr, "[ROMPATCH] WARNING: run_diags alt layout mismatch at %08lx (expected 0x2c57, got %04x) — a6 clobber NOT patched\n", (unsigned long)(base + 6), ntohs(*wp));
		}
		} else fprintf(stderr, "[ROMPATCH] SKIP run_diags (absent in 9.0.4; sets 68k stack — boot likely needs this)\n");
	} else {
		static const uint8 run_diags_dat[] = {0x74, 0x00, 0x2f, 0x0e};
		base = find_rom_data(0xd0, 0xf0, run_diags_dat, sizeof(run_diags_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("run_diags %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base - 6);
		*wp++ = htons(0x4df9);			// lea	xxx,a6
		*wp++ = htons((RAMBase + RAMSize - 0x1c) >> 16);
		*wp = htons((RAMBase + RAMSize - 0x1c) & 0xffff);
		} else fprintf(stderr, "[ROMPATCH] SKIP run_diags (absent in parcels)\n");
	}

	// Replace NVRAM routines
	static const uint8 nvram1_dat[] = {0x48, 0xe7, 0x01, 0x0e, 0x24, 0x68, 0x00, 0x08, 0x08, 0x83, 0x00, 0x1f};
	base = find_rom_data(0x7000, 0xc000, nvram1_dat, sizeof(nvram1_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("nvram1 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_EMUL_OP_XPRAM1);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP nvram1 (absent in parcels)\n");

	if (ROMType == ROMTYPE_NEWWORLD) {
		static const uint8 nvram2_dat[] = {0x48, 0xe7, 0x1c, 0xe0, 0x4f, 0xef, 0xff, 0xb4};
		base = find_rom_data(0xa000, 0xd000, nvram2_dat, sizeof(nvram2_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram2 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(M68K_EMUL_OP_XPRAM2);
		*wp = htons(0x4ed3);			// jmp	(a3)
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram2 (absent in parcels)\n");

		static const uint8 nvram3_dat[] = {0x48, 0xe7, 0xdc, 0xe0, 0x4f, 0xef, 0xff, 0xb4};
		base = find_rom_data(0xa000, 0xd000, nvram3_dat, sizeof(nvram3_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram3 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(M68K_EMUL_OP_XPRAM3);
		*wp = htons(0x4ed3);			// jmp	(a3)
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram3 (absent in parcels)\n");

		static const uint8 nvram4_dat[] = {0x4e, 0x56, 0xff, 0xa8, 0x48, 0xe7, 0x1f, 0x38, 0x16, 0x2e, 0x00, 0x13};
		base = find_rom_data(0xa000, 0xd000, nvram4_dat, sizeof(nvram4_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram4 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base + 16);
		*wp++ = htons(0x1a2e);			// move.b	($000f,a6),d5
		*wp++ = htons(0x000f);
		*wp++ = htons(M68K_EMUL_OP_NVRAM3);
		*wp++ = htons(0x4cee);			// movem.l	($ff88,a6),d3-d7/a2-a4
		*wp++ = htons(0x1cf8);
		*wp++ = htons(0xff88);
		*wp++ = htons(0x4e5e);			// unlk	a6
		*wp = htons(M68K_RTS);
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram4 (absent in parcels)\n");

		static const uint8 nvram5_dat[] = {0x0c, 0x80, 0x03, 0x00, 0x00, 0x00, 0x66, 0x0a, 0x70, 0x00, 0x21, 0xf8, 0x02, 0x0c, 0x01, 0xe4};
		base = find_rom_data(0xa000, 0xd000, nvram5_dat, sizeof(nvram5_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram5 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base + 6);
		*wp = htons(M68K_NOP);
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram5 (absent in parcels)\n");

		static const uint8 nvram6_dat[] = {0x2f, 0x0a, 0x24, 0x48, 0x4f, 0xef, 0xff, 0xa0, 0x20, 0x0f};
		base = find_rom_data(0x9000, 0xb000, nvram6_dat, sizeof(nvram6_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram6 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(0x7000);			// moveq	#0,d0
		*wp++ = htons(0x2080);			// move.l	d0,(a0)
		*wp++ = htons(0x4228);			// clr.b	4(a0)
		*wp++ = htons(0x0004);
		*wp = htons(M68K_RTS);
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram6 (absent in parcels)\n");

		static const uint8 nvram7_dat[] = {0x42, 0x2a, 0x00, 0x04, 0x4f, 0xef, 0x00, 0x60, 0x24, 0x5f, 0x4e, 0x75, 0x4f, 0xef, 0xff, 0xa0, 0x20, 0x0f};
		base = find_rom_data(0x9000, 0xb000, nvram7_dat, sizeof(nvram7_dat));
		if (base) {
			D(bug("nvram7 %08lx\n", base));
			wp = (uint16 *)(ROMBaseHost + base + 12);
			*wp = htons(M68K_RTS);
		}
	} else {
		static const uint8 nvram2_dat[] = {0x4e, 0xd6, 0x06, 0x41, 0x13, 0x00};
		base = find_rom_data(0x7000, 0xb000, nvram2_dat, sizeof(nvram2_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram2 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base + 2);
		*wp++ = htons(M68K_EMUL_OP_XPRAM2);
		*wp = htons(0x4ed3);			// jmp	(a3)
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram2 (absent in parcels)\n");

		static const uint8 nvram3_dat[] = {0x4e, 0xd3, 0x06, 0x41, 0x13, 0x00};
		base = find_rom_data(0x7000, 0xb000, nvram3_dat, sizeof(nvram3_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("nvram3 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base + 2);
		*wp++ = htons(M68K_EMUL_OP_XPRAM3);
		*wp = htons(0x4ed3);			// jmp	(a3)
		} else fprintf(stderr, "[ROMPATCH] SKIP nvram3 (absent in parcels)\n");

		static const uint32 nvram4_loc[] = {0x582f0, 0xa0a0, 0x7e50, 0xa1d0, 0x538d0, 0};
		wp = (uint16 *)(ROMBaseHost + nvram4_loc[ROMType]);
		*wp++ = htons(0x202f);			// move.l	4(sp),d0
		*wp++ = htons(0x0004);
		*wp++ = htons(M68K_EMUL_OP_NVRAM1);
		if (ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_GAZELLE)
			*wp = htons(M68K_RTS);
		else {
			*wp++ = htons(0x1f40);			// move.b	d0,8(sp)
			*wp++ = htons(0x0008);
			*wp++ = htons(0x4e74);			// rtd	#4
			*wp = htons(0x0004);
		}

		static const uint32 nvram5_loc[] = {0x58460, 0xa0f0, 0x7f40, 0xa220, 0x53a20, 0};
		wp = (uint16 *)(ROMBaseHost + nvram5_loc[ROMType]);
		if (ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_GAZELLE) {
			*wp++ = htons(0x202f);			// move.l	4(sp),d0
			*wp++ = htons(0x0004);
			*wp++ = htons(0x122f);			// move.b	11(sp),d1
			*wp++ = htons(0x000b);
			*wp++ = htons(M68K_EMUL_OP_NVRAM2);
			*wp = htons(M68K_RTS);
		} else {
			*wp++ = htons(0x202f);			// move.l	6(sp),d0
			*wp++ = htons(0x0006);
			*wp++ = htons(0x122f);			// move.b	4(sp),d1
			*wp++ = htons(0x0004);
			*wp++ = htons(M68K_EMUL_OP_NVRAM2);
			*wp++ = htons(0x4e74);			// rtd	#6
			*wp = htons(0x0006);
		}
	}

	// Fix MemTop/BootGlobs during system startup
	static const uint8 mem_top_dat[] = {0x2c, 0x6c, 0xff, 0xec, 0x2a, 0x4c, 0xdb, 0xec, 0xff, 0xf4};
	base = find_rom_data(0x120, 0x180, mem_top_dat, sizeof(mem_top_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("mem_top %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_EMUL_OP_FIX_MEMTOP);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP mem_top (absent in parcels)\n");

	// Don't initialize SCC (via 0x1ac)
	// M1 (ROM-PATCH-AUDIT: scc_init RETIRE@M1): on the fidelity profile the guest's
	// own SCC init must run and reach the SCC 8530 model through the bus - the patch
	// would make the device model dead code. Paravirtual keeps the patch.
	// MachineUsesMMIOBus() is deliberately NOT the gate: the SS_MMIO_BUS third config
	// is still paravirtual and the rest of the paravirtual patch set expects the
	// patched init - only the full fidelity (newworld) profile retires it.
	static const uint8 scc_init_caller_dat[] = {0x21, 0xce, 0x01, 0x08, 0x22, 0x78, 0x0d, 0xd8};
	base = find_rom_data(0x180, 0x1f0, scc_init_caller_dat, sizeof(scc_init_caller_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
	fprintf(stderr, "[M1] scc_init ROM patch retired (newworld profile): guest SCC init will run\n");
	} else if (base) {
	D(bug("scc_init_caller %08lx\n", base + 12));
	wp = (uint16 *)(ROMBaseHost + base + 12);
	loc = ntohs(wp[1]) + ((uintptr)wp - (uintptr)ROMBaseHost) + 2;
	static const uint8 scc_init_dat[] = {0x20, 0x78, 0x01, 0xdc, 0x22, 0x78, 0x01, 0xd8};
	if ((base = find_rom_data(loc, loc + 0x80, scc_init_dat, sizeof(scc_init_dat))) == 0) return false;
	D(bug("scc_init %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_EMUL_OP_RESET);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP scc_init (absent in parcels)\n");

	// SS_COMPAT_92X: SCC serial monitor patches — REMOVED (non-working).
	// Five patches tried (bset NOP, read bypass, write bypass, table init skip, tst NOP)
	// all treated symptoms of the serial debug monitor stall, not the root cause.
	// The serial monitor is a ROM debugger entered via an exception/trap path;
	// the fix must address WHY 9.2.1 enters it (8.6 never does). See
	// docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md §5.

	// Don't EnableExtCache (via 0x1f6) and don't DisableIntSources(via 0x1fc)
	static const uint8 ext_cache_dat[] = {0x4e, 0x7b, 0x00, 0x02};
	base = find_rom_data(0x1d0, 0x230, ext_cache_dat, sizeof(ext_cache_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("ext_cache %08lx\n", base));
	loc = ReadMacInt32(ROMBase + base + 6);
	wp = (uint16 *)(ROMBaseHost + loc + base + 6);
	*wp = htons(M68K_RTS);
	loc = ReadMacInt32(ROMBase + base + 12);
	wp = (uint16 *)(ROMBaseHost + loc + base + 12);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP ext_cache (absent in parcels)\n");

	// Fake CPU speed test (SetupTimeK)
	static const uint8 timek_dat[] = {0x0c, 0x38, 0x00, 0x04, 0x01, 0x2f, 0x6d, 0x3c};
	base = find_rom_data(0x400, 0x500, timek_dat, sizeof(timek_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("timek %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0d00);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeSCCDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0d02);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeSCSIDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0b24);
	*wp++ = htons(0x31fc);			// move.w	#xxx,TimeRAMDBRA
	*wp++ = htons(100);
	*wp++ = htons(0x0cea);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP timek (absent in parcels)\n");

	// Relocate jump tables ($2000..)
	static const uint8 jump_tab_dat[] = {0x41, 0xfa, 0x00, 0x0e, 0x21, 0xc8, 0x20, 0x10, 0x4e, 0x75};
	base = find_rom_data(0x3000, 0x6000, jump_tab_dat, sizeof(jump_tab_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("jump_tab %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base + 16);
	for (;;) {
		D(bug(" %08lx\n", (uintptr)lp - (uintptr)ROMBaseHost));
		while ((ntohl(*lp) & 0xff000000) == 0xff000000) {
			*lp = htonl((ntohl(*lp) & (ROM_SIZE-1)) + ROMBase);
			lp++;
		}
		while (!ntohl(*lp)) lp++;
		if (ntohl(*lp) != 0x41fa000e)
			break;
		lp += 4;
	}
	} else fprintf(stderr, "[ROMPATCH] SKIP jump_tab (absent in parcels)\n");

	// Create SysZone at start of Mac RAM (SetSysAppZone, via 0x22a)
	static const uint8 sys_zone_dat[] = {0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x40, 0x00};
	base = find_rom_data(0x600, 0x900, sys_zone_dat, sizeof(sys_zone_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("sys_zone %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);
	*lp++ = htonl(RAMBase ? RAMBase : 0x3000);
	*lp = htonl(RAMBase ? RAMBase + 0x1800 : 0x4800);
	} else fprintf(stderr, "[ROMPATCH] SKIP sys_zone (absent in parcels)\n");

	// Set boot stack at RAMBase+4MB and fix logical/physical RAM size (CompBootStack)
	// The RAM size fix must be done after InitMemMgr!
	static const uint8 boot_stack_dat[] = {0x08, 0x38, 0x00, 0x06, 0x24, 0x0b};
	base = find_rom_data(0x580, 0x800, boot_stack_dat, sizeof(boot_stack_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("boot_stack %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x207c);			// move.l	#RAMBase+0x3ffffe,a0
	*wp++ = htons((RAMBase + 0x3ffffe) >> 16);
	*wp++ = htons((RAMBase + 0x3ffffe) & 0xffff);
	*wp++ = htons(M68K_EMUL_OP_FIX_MEMSIZE);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP boot_stack (absent in parcels)\n");

	// Get PowerPC page size (InitVMemMgr, via 0x240)
	static const uint8 page_size_dat[] = {0x20, 0x30, 0x81, 0xf2, 0x5f, 0xff, 0xef, 0xd8, 0x00, 0x10};
	base = find_rom_data(0xb000, 0x12000, page_size_dat, sizeof(page_size_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("page_size %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x203c);			// move.l	#$1000,d0
	*wp++ = htons(0);
	*wp++ = htons(0x1000);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP page_size (absent in 9.0.4)\n");

	// Gestalt PowerPC page size, CPU type, RAM size (InitGestalt, via 0x25c)
	static const uint8 page_size2_dat[] = {0x26, 0x79, 0x5f, 0xff, 0xef, 0xd8, 0x25, 0x6b, 0x00, 0x10, 0x00, 0x1e};
	base = find_rom_data(0x50000, 0x70000, page_size2_dat, sizeof(page_size2_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("page_size2 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x257c);			// move.l	#$1000,$1e(a2)
	*wp++ = htons(0);
	*wp++ = htons(0x1000);
	*wp++ = htons(0x001e);
	*wp++ = htons(0x157c);			// move.b	#PVR,$1d(a2)
	uint32 cput = (PVR >> 16);
	if (cput == 0x7000)
		cput |= 0x20;
	else if (cput >= 0x8000 && cput <= 0x8002)
		cput |= 0x10;
	cput &= 0xff;
	*wp++ = htons(cput);
	*wp++ = htons(0x001d);
	*wp++ = htons(0x263c);			// move.l	#RAMSize,d3
	*wp++ = htons(RAMSize >> 16);
	*wp++ = htons(RAMSize & 0xffff);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	if (ROMType == ROMTYPE_NEWWORLD)
		wp = (uint16 *)(ROMBaseHost + base + 0x4a);
	else
		wp = (uint16 *)(ROMBaseHost + base + 0x28);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP page_size2/CPU-type gestalt (absent in 9.0.4; ROM does its own)\n");

	// Gestalt CPU/bus clock speed (InitGestalt, via 0x25c)
	if (ROMType == ROMTYPE_ZANZIBAR) {
		wp = (uint16 *)(ROMBaseHost + 0x5d87a);
		*wp++ = htons(0x203c);			// move.l	#Hz,d0
		*wp++ = htons(BusClockSpeed >> 16);
		*wp++ = htons(BusClockSpeed & 0xffff);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + 0x5d888);
		*wp++ = htons(0x203c);			// move.l	#Hz,d0
		*wp++ = htons(CPUClockSpeed >> 16);
		*wp++ = htons(CPUClockSpeed & 0xffff);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
	}

	// Don't write to GC interrupt mask register (via 0x262)
	if (ROMType != ROMTYPE_NEWWORLD) {
		static const uint8 gc_mask_dat[] = {0x83, 0xa8, 0x00, 0x24, 0x4e, 0x71};
		base = find_rom_data(0x13000, 0x20000, gc_mask_dat, sizeof(gc_mask_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("gc_mask %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + base + 0x40);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + base + 0x78);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		wp = (uint16 *)(ROMBaseHost + base + 0x96);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		} else fprintf(stderr, "[ROMPATCH] SKIP gc_mask (absent in parcels)\n");

		static const uint8 gc_mask2_dat[] = {0x02, 0xa8, 0x00, 0x00, 0x00, 0x80, 0x00, 0x24};
		base = find_rom_data(0x13000, 0x20000, gc_mask2_dat, sizeof(gc_mask2_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("gc_mask2 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		if (ROMType == ROMTYPE_GOSSAMER) {
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
		}
		for (int i=0; i<5; i++) {
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			wp += 2;
		}
		if (ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_GOSSAMER) {
			for (int i=0; i<6; i++) {
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				wp += 2;
			}
		}
		} else fprintf(stderr, "[ROMPATCH] SKIP gc_mask2 (absent in parcels)\n");
	}

	// Don't initialize Cuda (via 0x274)
	// M3b Task 3 (ROM-PATCH-AUDIT cuda_init_dat row, RETIRE@M3): retired on the
	// newworld profile WITH the Cuda model — dev_cuda now answers the VIA SR/ORB
	// surface, so the guest's own Cuda init must run (the audit's loud-stub
	// tension note anticipated exactly this landing). m7 framing (plan rev 2):
	// on the 9.0.1 parcels ROM this pattern already MISSES its search window
	// (cuda_init @0x9be2, outside 0xa000..0x12000) — the init already ran
	// unpatched there, which is exactly why the boot reached the TREQ poll; the
	// retirement is a 1.1/OldWorld-window-ROM behavior change only. Paravirtual
	// keeps the patch forever. Same idiom as via_init/scc_init: the search still
	// runs (find/skip telemetry stays consistent), only the patching is gated.
	static const uint8 cuda_init_dat[] = {0x08, 0xa9, 0x00, 0x04, 0x16, 0x00, 0x4e, 0x71, 0x13, 0x7c, 0x00, 0x84, 0x1c, 0x00, 0x4e, 0x71};
	base = find_rom_data(0xa000, 0x12000, cuda_init_dat, sizeof(cuda_init_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
		// Retired; banner reports whether the retirement changed anything on
		// THIS ROM (Task-3 review minor: on 9.0.1 the pattern misses, no-op).
		fprintf(stderr, "[M3b] cuda_init ROM patch retired (newworld profile, "
		        "pattern %s): guest Cuda init runs against the dev_cuda model\n",
		        base ? "found - patch suppressed" : "absent - no-op on this ROM");
	} else if (base) {
	D(bug("cuda_init %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP cuda_init (absent in parcels)\n");

	// Patch GetCPUSpeed (via 0x27a) (some ROMs have two of them)
	static const uint8 cpu_speed_dat[] = {0x20, 0x30, 0x81, 0xf2, 0x5f, 0xff, 0xef, 0xd8, 0x00, 0x04, 0x4c, 0x7c};
	base = find_rom_data(0x6000, 0xa000, cpu_speed_dat, sizeof(cpu_speed_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("cpu_speed %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x203c);			// move.l	#(MHz<<16)|MHz,d0
	*wp++ = htons(CPUClockSpeed / 1000000);
	*wp++ = htons(CPUClockSpeed / 1000000);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP cpu_speed (absent in 9.0.4)\n");
	if (base && (base = find_rom_data(base, 0xa000, cpu_speed_dat, sizeof(cpu_speed_dat))) != 0) {
		D(bug("cpu_speed2 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);
		*wp++ = htons(0x203c);			// move.l	#(MHz<<16)|MHz,d0
		*wp++ = htons(CPUClockSpeed / 1000000);
		*wp++ = htons(CPUClockSpeed / 1000000);
		*wp = htons(M68K_RTS);
	}

	// Don't poke VIA in InitTimeMgr (via 0x298)
	// M6a Wave 2 #4 (ROM-PATCH-AUDIT time_via_dat, was RETIRE@M3): retired with
	// the via_init cluster. The audit row flagged "possible early retirement if
	// the VIA timer/IFR surface proves sufficient" — the M2 clock + scheduler VIA
	// timers (T1/T2 state machines, eager IFR latch) are live, so InitTimeMgr's
	// VIA pokes now reach the model. Paravirtual keeps the early-return patch.
	static const uint8 time_via_dat[] = {0x40, 0xe7, 0x00, 0x7c, 0x07, 0x00, 0x28, 0x78, 0x01, 0xd4, 0x43, 0xec, 0x10, 0x00};
	base = find_rom_data(0x30000, 0x40000, time_via_dat, sizeof(time_via_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
	fprintf(stderr, "[M6a] time_via ROM patch retired (newworld profile): InitTimeMgr VIA calibration reaches the M2 timer model\n");
	} else if (base) {
	D(bug("time_via %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x4cdf);			// movem.l	(sp)+,d0-d5/a0-a4
	*wp++ = htons(0x1f3f);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP time_via (absent in 9.0.4)\n");

	// Don't read from 0xff800000 (Name Registry, Open Firmware?) (via 0x2a2)
	// Remove this if FE03 works!!
	static const uint8 open_firmware_dat[] = {0x2f, 0x79, 0xff, 0x80, 0x00, 0x00, 0x00, 0xfc};
	base = find_rom_data(0x48000, 0x58000, open_firmware_dat, sizeof(open_firmware_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("open_firmware %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x2f7c);			// move.l		#deadbeef,0xfc(a7)
	*wp++ = htons(0xdead);
	*wp++ = htons(0xbeef);
	*wp = htons(0x00fc);
	wp = (uint16 *)(ROMBaseHost + base + 0x1a);
	*wp++ = htons(M68K_NOP);		// (FE03 opcode, tries to jump to 0xdeadbeef)
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP open_firmware (absent in 9.0.4)\n");

	// Don't EnableExtCache (via 0x2b2)
	static const uint8 ext_cache2_dat[] = {0x4f, 0xef, 0xff, 0xec, 0x20, 0x4f, 0x10, 0xbc, 0x00, 0x01, 0x11, 0x7c, 0x00, 0x1b};
	base = find_rom_data(0x13000, 0x20000, ext_cache2_dat, sizeof(ext_cache2_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("ext_cache2 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP ext_cache2 (absent in parcels)\n");

	// Don't install Time Manager task for 60Hz interrupt (Enable60HzInts, via 0x2b8)
	// M6a Wave 2 #4: deliberately NOT retired on the newworld profile (stays
	// RETIRE@M3 per ROM-PATCH-AUDIT tm_task_dat). Its retirement needs M3b's real
	// delivery chain (VIA timer -> PIC -> CPU exception) to replace the host's
	// injected 60 Hz ticks; removing the host injection now would break
	// paravirtual-pattern timing with nothing delivering the interrupts.
	if (ROMType == ROMTYPE_NEWWORLD || ROMType == ROMTYPE_GOSSAMER) {
		static const uint8 tm_task_dat[] = {0x30, 0x3c, 0x4e, 0x2b, 0xa9, 0xc9};
		base = find_rom_data(0x2a0, 0x320, tm_task_dat, sizeof(tm_task_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("tm_task %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base + 28);
		// Verify-EXPECTED-first guard (SLIDE-WALL-RECON.md Q-SL1, tmtask-fix):
		// the 6 NOPs were calibrated against the 1.1 ROM, where base = 0x2f8
		// (in the declared window) and base+28 holds exactly two bsr.l install
		// calls [RAW-ROM 1.1, offline decode md5 5895907db063ef0112f19d930fdc9344]:
		//   +28: 61ff 0001 5db2   bsr.l  (Enable60HzInts install, -> 0x500160c8)
		//   +34: 61ff 0001 5d74   bsr.l  (second install call,    -> 0x50016090)
		// Pin the opcode class (both slots must START a bsr.l, 0x61ff), not the
		// displacements, so a Gossamer layout with drifted targets still passes.
		// On 9.0.1 the anchor lenient-relocates to 0x262 and +28 = 0x27e lands
		// MID-INSTRUCTION (the displacement low word of the boot sequencer's
		// bsr.l 0x5000060a, value 0x038e) — the unguarded write retargeted that
		// bsr.l to odd 0x500050ed and beheaded the SetApplLimit sequence (the
		// 0x505bb060 slide). Guard is default-on; SS_NW_TM_TASK_FORCE=1 restores
		// the unguarded write for A/B.
		if (ntohs(wp[0]) != 0x61ff || ntohs(wp[3]) != 0x61ff) {
			if (MachineEnvFlag("SS_NW_TM_TASK_FORCE")) {
				fprintf(stderr, "[ROMPATCH] tm_task FORCED write at %05x+28 despite expected-bytes mismatch (SS_NW_TM_TASK_FORCE=1)\n", (unsigned)base);
			} else {
				fprintf(stderr, "[ROMPATCH] tm_task GUARDED-SKIP (9.0.1 misalignment): base=%05x, +28 bytes %04x %04x %04x %04x %04x %04x != two bsr.l (61ff .... .... 61ff .... ....) — 60Hz TM task install left intact; SS_NW_TM_TASK_FORCE=1 restores the old write\n",
				        (unsigned)base, ntohs(wp[0]), ntohs(wp[1]), ntohs(wp[2]), ntohs(wp[3]), ntohs(wp[4]), ntohs(wp[5]));
				wp = NULL;
			}
		}
		if (wp) {
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		}
		} else fprintf(stderr, "[ROMPATCH] SKIP tm_task (absent in parcels)\n");
	} else {
		static const uint8 tm_task_dat[] = {0x20, 0x3c, 0x73, 0x79, 0x73, 0x61};
		base = find_rom_data(0x280, 0x300, tm_task_dat, sizeof(tm_task_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("tm_task %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base - 6);
		*wp++ = htons(M68K_NOP);
		*wp++ = htons(M68K_NOP);
		*wp = htons(M68K_NOP);
		} else fprintf(stderr, "[ROMPATCH] SKIP tm_task (absent in parcels)\n");
	}

	// Don't read PVR from 0x5fffef80 in DriverServicesLib (via 0x316)
	if (ROMType != ROMTYPE_NEWWORLD && ROMType != ROMTYPE_GOSSAMER) {
		uint32 dsl_offset = find_rom_resource(FOURCC('n','l','i','b'), -16401);
		if (ROMType == ROMTYPE_ZANZIBAR) {
			static const uint8 dsl_pvr_dat[] = {0x40, 0x82, 0x00, 0x40, 0x38, 0x60, 0xef, 0x80, 0x3c, 0x63, 0x60, 0x00, 0x80, 0x83, 0x00, 0x00, 0x54, 0x84, 0x84, 0x3e};
			base = find_rom_data(dsl_offset, dsl_offset + 0x6000, dsl_pvr_dat, sizeof(dsl_pvr_dat));
		} else {
			static const uint8 dsl_pvr_dat[] = {0x3b, 0xc3, 0x00, 0x00, 0x30, 0x84, 0xff, 0xa0, 0x40, 0x82, 0x00, 0x44, 0x80, 0x84, 0xef, 0xe0, 0x54, 0x84, 0x84, 0x3e};
			base = find_rom_data(dsl_offset, dsl_offset + 0x6000, dsl_pvr_dat, sizeof(dsl_pvr_dat));
		}
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("dsl_pvr %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base + 12);
		*lp = htonl(0x3c800000 | (PVR >> 16));	// lis	r4,PVR
		} else fprintf(stderr, "[ROMPATCH] SKIP dsl_pvr (absent in parcels)\n");

		// Don't read bus clock from 0x5fffef88 in DriverServicesLib (via 0x316)
		if (ROMType == ROMTYPE_ZANZIBAR) {
			static const uint8 dsl_bus_dat[] = {0x81, 0x07, 0x00, 0x00, 0x39, 0x20, 0x42, 0x40, 0x81, 0x62, 0xff, 0x20};
			base = find_rom_data(dsl_offset, dsl_offset + 0x6000, dsl_bus_dat, sizeof(dsl_bus_dat));
			if (base == 0 && !g_rom_904_lenient) return false;
			if (base) {
			D(bug("dsl_bus %08lx\n", base));
			lp = (uint32 *)(ROMBaseHost + base);
			*lp = htonl(0x81000000 + XLM_BUS_CLOCK);	// lwz	r8,(bus clock speed)
			} else fprintf(stderr, "[ROMPATCH] SKIP dsl_bus (absent in parcels)\n");
		} else {
			static const uint8 dsl_bus_dat[] = {0x80, 0x83, 0xef, 0xe8, 0x80, 0x62, 0x00, 0x10, 0x7c, 0x04, 0x03, 0x96};
			base = find_rom_data(dsl_offset, dsl_offset + 0x6000, dsl_bus_dat, sizeof(dsl_bus_dat));
			if (base == 0 && !g_rom_904_lenient) return false;
			if (base) {
			D(bug("dsl_bus %08lx\n", base));
			lp = (uint32 *)(ROMBaseHost + base);
			*lp = htonl(0x80800000 + XLM_BUS_CLOCK);	// lwz	r4,(bus clock speed)
			} else fprintf(stderr, "[ROMPATCH] SKIP dsl_bus (absent in parcels)\n");
		}
	}

	// Don't open InterruptTreeTNT in MotherBoardHAL init in DriverServicesLib init
	if (ROMType == ROMTYPE_ZANZIBAR) {
		lp = (uint32 *)(ROMBaseHost + find_rom_resource(FOURCC('n','l','i','b'), -16408) + 0x16c);
		*lp = htonl(0x38600000);		// li	r3,0
	}

	// Don't read from MacPgm in WipeOutMACPGMINFOProcPtrs (StdCLib)
	if (1) {
		uint32 hpchk_offset = find_rom_resource(FOURCC('n','l','i','b'), 10);
		static const uint8 hpchk_dat[] = {0x80, 0x80, 0x03, 0x16, 0x94, 0x21, 0xff, 0xb0, 0x83, 0xc4, 0x00, 0x04};
		base = find_rom_data(hpchk_offset, hpchk_offset + 0x3000, hpchk_dat, sizeof(hpchk_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("macpgm %08lx\n", base));
		lp = (uint32 *)(ROMBaseHost + base);
		*lp = htonl(0x80800000 + XLM_ZERO_PAGE);		// lwz	r4,(zero page)
		} else fprintf(stderr, "[ROMPATCH] SKIP macpgm (absent in parcels)\n");
	}

	// Patch Name Registry
	static const uint8 name_reg_dat[] = {0x70, 0xff, 0xab, 0xeb};
	base = find_rom_data(0x300, 0x380, name_reg_dat, sizeof(name_reg_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("name_reg %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp = htons(M68K_EMUL_OP_NAME_REGISTRY);
	} else fprintf(stderr, "[ROMPATCH] SKIP name_reg (absent in parcels)\n");

#if DISABLE_SCSI
	// Fake SCSI Manager
	// Remove this if SCSI Manager works!!
	static const uint8 scsi_mgr_a_dat[] = {0x4e, 0x56, 0x00, 0x00, 0x20, 0x3c, 0x00, 0x00, 0x04, 0x0c, 0xa7, 0x1e};
	static const uint8 scsi_mgr_b_dat[] = {0x4e, 0x56, 0x00, 0x00, 0x2f, 0x0c, 0x20, 0x3c, 0x00, 0x00, 0x04, 0x0c, 0xa7, 0x1e};
	if ((base = find_rom_data(0x1c000, 0x28000, scsi_mgr_a_dat, sizeof(scsi_mgr_a_dat))) == 0)
		base = find_rom_data(0x1c000, 0x28000, scsi_mgr_b_dat, sizeof(scsi_mgr_b_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("scsi_mgr %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x21fc);			// move.l	#xxx,0x624	(SCSIAtomic)
	*wp++ = htons((ROMBase + base + 18) >> 16);
	*wp++ = htons((ROMBase + base + 18) & 0xffff);
	*wp++ = htons(0x0624);
	*wp++ = htons(0x21fc);			// move.l	#xxx,0xe54	(SCSIDispatch)
	*wp++ = htons((ROMBase + base + 22) >> 16);
	*wp++ = htons((ROMBase + base + 22) & 0xffff);
	*wp++ = htons(0x0e54);
	*wp++ = htons(M68K_RTS);
	*wp++ = htons(M68K_EMUL_OP_SCSI_ATOMIC);
	*wp++ = htons(M68K_RTS);
	*wp++ = htons(M68K_EMUL_OP_SCSI_DISPATCH);
	*wp = htons(0x4ed0);			// jmp		(a0)
	wp = (uint16 *)(ROMBaseHost + base + 0x20);
	*wp++ = htons(0x7000);			// moveq	#0,d0
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP scsi_mgr (absent in parcels)\n");
#endif

#if DISABLE_SCSI
	// Don't access SCSI variables
	// Remove this if SCSI Manager works!!
	if (ROMType == ROMTYPE_NEWWORLD) {
		static const uint8 scsi_var_dat[] = {0x70, 0x01, 0xa0, 0x89, 0x4a, 0x6e, 0xfe, 0xac, 0x4f, 0xef, 0x00, 0x10, 0x66, 0x00};
		if ((base = find_rom_data(0x1f500, 0x1f600, scsi_var_dat, sizeof(scsi_var_dat))) != 0) {
			D(bug("scsi_var %08lx\n", base));
			wp = (uint16 *)(ROMBaseHost + base + 12);
			*wp = htons(0x6000);	// bra
		}

		static const uint8 scsi_var2_dat[] = {0x4e, 0x56, 0xfc, 0x58, 0x48, 0xe7, 0x1f, 0x38};
		if ((base = find_rom_data(0x1f700, 0x1f800, scsi_var2_dat, sizeof(scsi_var2_dat))) != 0) {
			D(bug("scsi_var2 %08lx\n", base));
			wp = (uint16 *)(ROMBaseHost + base);
			*wp++ = htons(0x7000);	// moveq #0,d0
			*wp = htons(M68K_RTS);
		}
	}
	else if (ROMType == ROMTYPE_GOSSAMER) {
		static const uint8 scsi_var_dat[] = {0x70, 0x01, 0xa0, 0x89, 0x4a, 0x6e, 0xfe, 0xac, 0x4f, 0xef, 0x00, 0x10, 0x66, 0x00};
		if ((base = find_rom_data(0x1d700, 0x1d800, scsi_var_dat, sizeof(scsi_var_dat))) != 0) {
			D(bug("scsi_var %08lx\n", base));
			wp = (uint16 *)(ROMBaseHost + base + 12);
			*wp = htons(0x6000);	// bra
		}

		static const uint8 scsi_var2_dat[] = {0x4e, 0x56, 0xfc, 0x5a, 0x48, 0xe7, 0x1f, 0x38};
		if ((base = find_rom_data(0x1d900, 0x1da00, scsi_var2_dat, sizeof(scsi_var2_dat))) != 0) {
			D(bug("scsi_var2 %08lx\n", base));
			wp = (uint16 *)(ROMBaseHost + base);
			*wp++ = htons(0x7000);	// moveq #0,d0
			*wp = htons(M68K_RTS);
		}
	}
#endif

	// Don't wait in ADBInit (via 0x36c)
	// M3b Task 3 (ROM-PATCH-AUDIT adb_init_dat row, RETIRE@M3): retired with
	// cuda_init — the ADBInit wait now gets real TREQ responses from the Cuda
	// model + ADB stub (kbd@2/mouse@3 Talk R3, Listen R3 address-move) instead
	// of spinning on the M1 loud stub. m7 framing (plan rev 2): on the 9.0.1
	// parcels ROM this pattern already MISSES its window (adb_init @0x2b780,
	// outside 0x31000..0x3d000), so the retirement only changes behavior on
	// 1.1/OldWorld-window ROMs. Paravirtual keeps the patch forever.
	static const uint8 adb_init_dat[] = {0x08, 0x2b, 0x00, 0x05, 0x01, 0x5d, 0x66, 0xf8};
	base = find_rom_data(0x31000, 0x3d000, adb_init_dat, sizeof(adb_init_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (MachineProfileIsNewWorld()) {
		// Retired; banner reports pattern presence (no-op on 9.0.1 — see above).
		fprintf(stderr, "[M3b] adb_init ROM patch retired (newworld profile, "
		        "pattern %s): ADBInit wait runs against the dev_cuda model + adb_stub\n",
		        base ? "found - patch suppressed" : "absent - no-op on this ROM");
	} else if (base) {
	D(bug("adb_init %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base + 6);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP adb_init (absent in parcels)\n");

	// Modify check in InitResources() so that addresses >0x80000000 work
	static const uint8 init_res_dat[] = {0x4a, 0xb8, 0x0a, 0x50, 0x6e, 0x20};
	base = find_rom_data(0x78000, 0x8c000, init_res_dat, sizeof(init_res_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("init_res %08lx\n", base));
	bp = (uint8 *)(ROMBaseHost + base + 4);
	*bp = 0x66;
	} else fprintf(stderr, "[ROMPATCH] SKIP init_res (absent in parcels)\n");

	// Modify vCheckLoad() so that we can patch resources (68k Resource Manager)
	static const uint8 check_load_dat[] = {0x20, 0x78, 0x07, 0xf0, 0x4e, 0xd0};
	base = find_rom_data(0x78000, 0x8c000, check_load_dat, sizeof(check_load_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("check_load %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(M68K_JMP);
	*wp++ = htons((ROMBase + CHECK_LOAD_PATCH_SPACE) >> 16);
	*wp = htons((ROMBase + CHECK_LOAD_PATCH_SPACE) & 0xffff);
	wp = (uint16 *)(ROMBaseHost + CHECK_LOAD_PATCH_SPACE);
	*wp++ = htons(0x2f03);			// move.l	d3,-(a7)
	*wp++ = htons(0x2078);			// move.l	$07f0,a0
	*wp++ = htons(0x07f0);
	*wp++ = htons(M68K_JSR_A0);
	*wp++ = htons(M68K_EMUL_OP_CHECKLOAD);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP check_load (absent in parcels)\n");

	// Replace .Sony driver
	sony_offset = find_rom_resource(FOURCC('D','R','V','R'), 4);
	if (ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_NEWWORLD)
		sony_offset = find_rom_resource(FOURCC('D','R','V','R'), 4, true);		// First DRVR 4 is .MFMFloppy
	if (sony_offset == 0) {
		sony_offset = find_rom_resource(FOURCC('n','d','r','v'), -20196);		// NewWorld 1.6 has "PCFloppy" ndrv
		if (sony_offset == 0) {
			// 9.0.1 parcels ROM: no DRVR 4 and no PCFloppy ndrv (verified against the
			// raw dump's resource map). This used to be a hard `return false` that
			// silently dropped the ENTIRE remaining EMUL_OP tail (Time Manager, ADBOp,
			// PowerOff, scrap, ...) — TRAP-TABLE-RECON.md Q2. Lenient mode: skip the
			// driver replacement only and resume the tail. Every write site in the
			// resumed tail below is verify-target-first (a find_rom_trap/find_rom_resource
			// result of 0 SKIPs with a banner, never writes at ROM offset 0).
			if (!g_rom_904_lenient)
				return false;
			fprintf(stderr, "[ROMPATCH] SKIP sony driver replacement (no DRVR 4 / PCFloppy ndrv in parcels) — EMUL_OP tail resumes\n");
		} else {
			lp = (uint32 *)(ROMBaseHost + rsrc_ptr + 8);
			*lp = htonl(FOURCC('D','R','V','R'));
			wp = (uint16 *)(ROMBaseHost + rsrc_ptr + 12);
			*wp = htons(4);
		}
	}
	if (sony_offset) {
	D(bug("sony_offset %08lx\n", sony_offset));
	memcpy((void *)(ROMBaseHost + sony_offset), sony_driver, sizeof(sony_driver));

	// Install .Disk and .AppleCD drivers
	memcpy((void *)(ROMBaseHost + sony_offset + 0x100), disk_driver, sizeof(disk_driver));
	memcpy((void *)(ROMBaseHost + sony_offset + 0x200), cdrom_driver, sizeof(cdrom_driver));

	// Install serial drivers
	gen_ain_driver( ROMBase + sony_offset + 0x300);
	gen_aout_driver(ROMBase + sony_offset + 0x400);
	gen_bin_driver( ROMBase + sony_offset + 0x500);
	gen_bout_driver(ROMBase + sony_offset + 0x600);

	// Copy icons to ROM
	SonyDiskIconAddr = ROMBase + sony_offset + 0x800;
	memcpy(ROMBaseHost + sony_offset + 0x800, SonyDiskIcon, sizeof(SonyDiskIcon));
	SonyDriveIconAddr = ROMBase + sony_offset + 0xa00;
	memcpy(ROMBaseHost + sony_offset + 0xa00, SonyDriveIcon, sizeof(SonyDriveIcon));
	DiskIconAddr = ROMBase + sony_offset + 0xc00;
	memcpy(ROMBaseHost + sony_offset + 0xc00, DiskIcon, sizeof(DiskIcon));
	CDROMIconAddr = ROMBase + sony_offset + 0xe00;
	memcpy(ROMBaseHost + sony_offset + 0xe00, CDROMIcon, sizeof(CDROMIcon));
	} // sony_offset

	// Patch driver install routine
	static const uint8 drvr_install_dat[] = {0xa7, 0x1e, 0x21, 0xc8, 0x01, 0x1c, 0x4e, 0x75};
	base = find_rom_data(0xb00, 0xd00, drvr_install_dat, sizeof(drvr_install_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("drvr_install %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base + 8);
	*wp++ = htons(M68K_EMUL_OP_INSTALL_DRIVERS);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP drvr_install (absent in parcels)\n");

	// Don't install serial drivers from ROM
	if (ROMType == ROMTYPE_ZANZIBAR || ROMType == ROMTYPE_NEWWORLD || ROMType == ROMTYPE_GOSSAMER) {
		uint32 serd_offset = find_rom_resource(FOURCC('S','E','R','D'), 0);
		if (serd_offset) {
			wp = (uint16 *)(ROMBaseHost + serd_offset);
			*wp = htons(M68K_RTS);
		} else
			// 9.0.1 parcels has no SERD 0 — the unguarded write would land M68K_RTS
			// at ROM offset 0 (the banked SERD-0 hazard, TRAP-TABLE-RECON.md).
			fprintf(stderr, "[ROMPATCH] SERD-0 GUARDED-SKIP (resource absent — would clobber ROM offset 0)\n");
	} else {
		uint32 sl05_offset = find_rom_resource(FOURCC('s','l','0','5'), 2);
		if (sl05_offset) {
			wp = (uint16 *)(ROMBaseHost + sl05_offset + 0xc4);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp++ = htons(M68K_NOP);
			*wp = htons(0x7000);			// moveq	#0,d0
			wp = (uint16 *)(ROMBaseHost + sl05_offset + 0x8ee);
			*wp = htons(M68K_NOP);
		} else
			fprintf(stderr, "[ROMPATCH] sl05-2 GUARDED-SKIP (resource absent — would clobber ROM offset 0xc4)\n");
	}
	uint32 nsrd_offset = find_rom_resource(FOURCC('n','s','r','d'), 1);
	if (nsrd_offset) {
		lp = (uint32 *)(ROMBaseHost + rsrc_ptr + 8);
		*lp = htonl(FOURCC('x','s','r','d'));
	}

	// Replace ADBOp()
	{
		uint32 adbop_offset = find_rom_trap(0xa07c);
		if (adbop_offset)
			memcpy(ROMBaseHost + adbop_offset, adbop_patch, sizeof(adbop_patch));
		else
			fprintf(stderr, "[ROMPATCH] ADBOp GUARDED-SKIP (find_rom_trap(0xa07c)=0 — would clobber ROM offset 0)\n");
	}

	// Replace Time Manager.
	// Two cases per trap:
	//  (a) find_rom_trap() nonzero (1.1/OldWorld/Zanzibar/Gossamer): upstream's in-place
	//      stub write over the ROM's own TM body (bodies verbatim from kanjitalk755/macemu
	//      rom_patches.cpp; unchanged behavior).
	//  (b) find_rom_trap() == 0: the 9.0.1 parcels ROM genuinely ships NULL trap-table-image
	//      entries for the whole TM cluster {0x58 InsTime/InsXTime, 0x59 RmvTime,
	//      0x5a PrimeTime, 0x93 Microseconds} — on real HW the TM is native-runtime-installed
	//      (parcels world, parked). TRAP-TABLE-RECON.md Q2/Q5. The unguarded upstream write
	//      would clobber ROM offset 0. Instead (SS_NW_TM_TRAPS, newworld): place upstream's
	//      exact stub bodies in TIME_MANAGER_PATCH_SPACE and populate the ROM-embedded
	//      trap-table IMAGE entries (at [ROM+0x22] + 0x1000 + 4*trap#) verify-zero-first;
	//      the guest's OWN installer (ROM 0xe0c8) then installs them into lowmem 0x400 at
	//      boot — both DR dispatch levels consume that same table (recon Q1).
	{
		// SS_NW_TM_TRAPS: NEWWORLD DEFAULT since the instime-fix acceptance battery
		// (FLIP-LAST: implemented default-OFF f808a7fb, flipped after acceptance went
		// green — [0x560]=ROMBase+0x2fd240 installed by the guest installer, InsXTime
		// dispatches into the stub, Enable60HzInts completes and returns to the boot
		// sequencer, SysError-12 park cleared; gated-off A/B reproduces the baseline
		// park signature exactly). Explicit-"0"-only opt-out, the
		// SS_NW_DR_R0_INVARIANT/SS_NW_SC_SURFACE polarity precedent (getenv+strcmp).
		const char *tm_traps_env = getenv("SS_NW_TM_TRAPS");
		const bool tm_traps_on = MachineProfileIsNewWorld() &&
			!(tm_traps_env && strcmp(tm_traps_env, "0") == 0);
		bool tm_space_ready = false;	// stub bodies written to patch space?
		uint32 stub_ofs[4] = {0, 0, 0, 0};	// ROM offsets of the 4 stub bodies
		struct { uint16 trap; uint8 kind; const char *name; } tm_traps[4] = {
			{0xa058, 0, "InsTime"},		// kind 0: EMUL_OP + rts
			{0xa059, 1, "RmvTime"},		// kind 1: sr-masked EMUL_OP
			{0xa05a, 2, "PrimeTime"},	// kind 2: sr-masked EMUL_OP
			{0xa093, 3, "Microseconds"},	// kind 3: EMUL_OP + rts
		};
		const uint16 tm_emul_op[4] = {M68K_EMUL_OP_INSTIME, M68K_EMUL_OP_RMVTIME,
		                              M68K_EMUL_OP_PRIMETIME, M68K_EMUL_OP_MICROSECONDS};
		for (int i = 0; i < 4; i++) {
			uint32 ofs = find_rom_trap(tm_traps[i].trap);
			if (ofs) {
				// (a) upstream in-place write (body verbatim)
				wp = (uint16 *)(ROMBaseHost + ofs);
				if (tm_traps[i].kind == 1 || tm_traps[i].kind == 2) {
					*wp++ = htons(0x40e7);		// move	sr,-(sp)
					*wp++ = htons(0x007c);		// ori	#$0700,sr
					*wp++ = htons(0x0700);
					*wp++ = htons(tm_emul_op[i]);
					*wp++ = htons(0x46df);		// move	(sp)+,sr
					*wp = htons(M68K_RTS);
				} else {
					*wp++ = htons(tm_emul_op[i]);
					*wp = htons(M68K_RTS);
				}
				continue;
			}
			if (!tm_traps_on) {
				fprintf(stderr, "[ROMPATCH] %s GUARDED-SKIP (find_rom_trap(0x%04x)=0, SS_NW_TM_TRAPS off — would clobber ROM offset 0)\n",
				        tm_traps[i].name, tm_traps[i].trap);
				continue;
			}
			// (b) image population path
			if (!tm_space_ready) {
				if (!check_rom_patch_space(TIME_MANAGER_PATCH_SPACE, 0x40)) {
					fprintf(stderr, "[ROMPATCH] tm_traps GUARDED-SKIP (patch space 0x%x not free — layout moved)\n",
					        TIME_MANAGER_PATCH_SPACE);
					continue;
				}
				// Stub bodies, upstream-verbatim (kanjitalk755/macemu rom_patches.cpp
				// "Replace Time Manager"), relocated into patch space:
				wp = (uint16 *)(ROMBaseHost + TIME_MANAGER_PATCH_SPACE);
				stub_ofs[0] = TIME_MANAGER_PATCH_SPACE + 0x00;	// InsTime/InsXTime (d1 = trap word)
				*wp++ = htons(M68K_EMUL_OP_INSTIME);
				*wp++ = htons(M68K_RTS);
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				stub_ofs[1] = TIME_MANAGER_PATCH_SPACE + 0x08;	// RmvTime
				*wp++ = htons(0x40e7);		// move	sr,-(sp)
				*wp++ = htons(0x007c);		// ori	#$0700,sr
				*wp++ = htons(0x0700);
				*wp++ = htons(M68K_EMUL_OP_RMVTIME);
				*wp++ = htons(0x46df);		// move	(sp)+,sr
				*wp++ = htons(M68K_RTS);
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				stub_ofs[2] = TIME_MANAGER_PATCH_SPACE + 0x18;	// PrimeTime
				*wp++ = htons(0x40e7);		// move	sr,-(sp)
				*wp++ = htons(0x007c);		// ori	#$0700,sr
				*wp++ = htons(0x0700);
				*wp++ = htons(M68K_EMUL_OP_PRIMETIME);
				*wp++ = htons(0x46df);		// move	(sp)+,sr
				*wp++ = htons(M68K_RTS);
				*wp++ = htons(M68K_NOP);
				*wp++ = htons(M68K_NOP);
				stub_ofs[3] = TIME_MANAGER_PATCH_SPACE + 0x28;	// Microseconds
				*wp++ = htons(M68K_EMUL_OP_MICROSECONDS);
				*wp = htons(M68K_RTS);
				tm_space_ready = true;
			}
			// Populate the trap-table image entry, verify-zero-first (entries proven
			// NULL on the raw 9.0.1 dump; nonzero means the layout moved — never overwrite).
			uint32 img = ReadMacInt32(ROMBase + 0x22);
			uint32 ent = img + 0x1000 + 4 * (tm_traps[i].trap & 0xff);	// OS table leg
			lp = (uint32 *)(ROMBaseHost + ent);
			if (ntohl(*lp) != 0) {
				fprintf(stderr, "[ROMPATCH] tm_traps %s GUARDED-SKIP (image entry @ROM+0x%x = %08x, expected 0 — layout moved)\n",
				        tm_traps[i].name, ent, ntohl(*lp));
				continue;
			}
			*lp = htonl(stub_ofs[tm_traps[i].kind]);
			fprintf(stderr, "[ROMPATCH] tm_traps: OS trap #0x%02x %s -> ROM+0x%x (EMUL_OP stub; guest installer 0xe0c8 installs at boot)\n",
			        tm_traps[i].trap & 0xff, tm_traps[i].name, stub_ofs[tm_traps[i].kind]);
		}
	}

	// Disable Egret Manager
	static const uint8 egret_dat[] = {0x2f, 0x30, 0x81, 0xe2, 0x20, 0x10, 0x00, 0x18};
	base = find_rom_data(0xa000, 0x10000, egret_dat, sizeof(egret_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("egret %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	*wp++ = htons(0x7000);
	*wp = htons(M68K_RTS);
	} else fprintf(stderr, "[ROMPATCH] SKIP egret (absent in parcels)\n");

	// Don't call FE0A opcode in Shutdown Manager
	static const uint8 shutdown_dat[] = {0x40, 0xe7, 0x00, 0x7c, 0x07, 0x00, 0x48, 0xe7, 0x3f, 0x00, 0x2c, 0x00, 0x2e, 0x01};
	base = find_rom_data(0x30000, 0x40000, shutdown_dat, sizeof(shutdown_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("shutdown %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);
	if (ROMType == ROMTYPE_ZANZIBAR)
		*wp = htons(M68K_RTS);
	else if (ntohs(wp[-4]) == 0x61ff)
		*wp = htons(M68K_RTS);
	else if (ntohs(wp[-2]) == 0x6700)
		wp[-2] = htons(0x6000);	// bra
	} else fprintf(stderr, "[ROMPATCH] SKIP shutdown (absent in parcels)\n");

	// Patch PowerOff() → trigger clean host exit via OP_POWEROFF
	{
		uint32 poweroff_offset = find_rom_trap(0xa05b);	// PowerOff()
		if (poweroff_offset) {
			wp = (uint16 *)(ROMBaseHost + poweroff_offset);
			*wp = htons(M68K_EMUL_OP_POWEROFF);
		} else
			fprintf(stderr, "[ROMPATCH] PowerOff GUARDED-SKIP (find_rom_trap(0xa05b)=0 — would clobber ROM offset 0)\n");
	}

	// Patch VIA interrupt handler
	static const uint8 via_int_dat[] = {0x70, 0x7f, 0xc0, 0x29, 0x1a, 0x00, 0xc0, 0x29, 0x1c, 0x00};
	base = find_rom_data(0x13000, 0x1c000, via_int_dat, sizeof(via_int_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	uint32 level1_int = 0;
	if (base) {
	D(bug("via_int %08lx\n", base));
	level1_int = ROMBase + base;
	wp = (uint16 *)(ROMBaseHost + base);	// Level 1 handler
	*wp++ = htons(0x7002);			// moveq	#2,d0 (60Hz interrupt)
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp++ = htons(M68K_NOP);
	*wp = htons(M68K_NOP);
	} else fprintf(stderr, "[ROMPATCH] SKIP via_int (absent in parcels)\n");

	static const uint8 via_int2_dat[] = {0x13, 0x7c, 0x00, 0x02, 0x1a, 0x00, 0x4e, 0x71, 0x52, 0xb8, 0x01, 0x6a};
	base = find_rom_data(0x10000, 0x18000, via_int2_dat, sizeof(via_int2_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	D(bug("via_int2 %08lx\n", base));
	wp = (uint16 *)(ROMBaseHost + base);	// 60Hz handler
	*wp++ = htons(M68K_EMUL_OP_IRQ);
	*wp++ = htons(0x4a80);			// tst.l	d0
	*wp++ = htons(0x6700);			// beq		xxx
	*wp = htons(0xffe8);
	} else fprintf(stderr, "[ROMPATCH] SKIP via_int2 (absent in parcels)\n");

	if (ROMType == ROMTYPE_NEWWORLD && level1_int) {
		static const uint8 via_int3_dat[] = {0x48, 0xe7, 0xf0, 0xf0, 0x76, 0x01, 0x60, 0x26};
		base = find_rom_data(0x15000, 0x19000, via_int3_dat, sizeof(via_int3_dat));
		if (base == 0 && !g_rom_904_lenient) return false;
		if (base) {
		D(bug("via_int3 %08lx\n", base));
		wp = (uint16 *)(ROMBaseHost + base);	// CHRP level 1 handler
		*wp++ = htons(M68K_JMP);
		*wp++ = htons((level1_int - 12) >> 16);
		*wp = htons((level1_int - 12) & 0xffff);
		} else fprintf(stderr, "[ROMPATCH] SKIP via_int3 (absent in parcels)\n");
	}

	// SS_NW_VIA_IFR: M9 gate — VIA-IFR surface.
	//
	// The previous approach (replacing 0x5000ed08 with OP_IRQ_NW+rte) is WRONG:
	// those 8 bytes are read as DATA by the NK during boot (pattern scan); corrupting
	// them → NK fails to initialize → dec_expiries=5 stall.  The stall is the sole
	// symptom; removing the patch restores dec_expiries to baseline levels.
	//
	// M9 finding (session-4 probe campaign): the 68k handler at 0x5000ed08 does NOT
	// fire in the current boot, even in baseline.  Root cause chain (CORRECTED M10):
	//   1. NK EXT handler (0x50314880) checks r11.bit16 = EE bit (0x8000 mask), NOT PR.
	//      With M7 EE_RISER active, EE=1 during 68k execution → CGRP path IS reachable.
	//      (Session-4 PR-bit analysis was incorrect; Task A / SS_M6A_USER_MSR not needed.)
	//   2. CGRP+0x20 = 1 (a function pointer, never initialized by NK cold-start to a
	//      valid address) — CGRP delivery bails immediately.  This is the sole blocker.
	//
	// SS_NW_VIA_IFR=1 is now a no-op (gate kept; SS_M10_CGRP=1 is the M10 gate).
	//
	// Analysis: docs/archive/2026-06/machine/VIA-IFR-RECON.md; docs/HANDOFF.md §Session 4+5.
	if (ROMType == ROMTYPE_NEWWORLD && getenv("SS_NW_VIA_IFR") &&
	    strcmp(getenv("SS_NW_VIA_IFR"), "0") != 0) {
		fprintf(stderr,
		        "[VIA-IFR] SS_NW_VIA_IFR=1 — gate is currently a NO-OP.\n"
		        "[VIA-IFR]   M10 gate is SS_M10_CGRP=1 (CGRP initialization).\n"
		        "[VIA-IFR]   See: docs/HANDOFF.md §Session 4+5.\n");
	}

	// SS_M10_CGRP: M10 gate — CGRP initialization for 68k interrupt delivery.
	//
	// The NK EXT handler (0x50314880) routes timer interrupts to the 68k via a CGRP
	// struct at *(KDP-0x338) = 0x68ffc1c0.  Mac OS normally initializes CGRP during
	// System startup, but we stall before that point.  This patch:
	//   1. Writes a 3-instruction RFI_target stub into trampoline space (0x429da0):
	//        lis  r24, 0x5000
	//        ori  r24, r24, 0xed08    → r24 = 0x5000ed08 (68k handler PC)
	//        b    0x5046e9d8          → DR warm-entry (resumes 68k dispatch from r24)
	//   2. Builds a TABLE_BASE array (10 entries) and a descriptor struct pointing to
	//      the stub; entry[9] is used (NK EXT posts source_index=9 for timer).
	//   3. Builds a STACK_TABLE (10 entries), each pointing to a sentinel word so
	//      the NK's stack-switch leaves a breadcrumb but the DR ignores it.
	//   4. Writes five CGRP fields via WriteMacInt32 into guest RAM.
	//
	// Layout in trampoline space (all zero before this patch):
	//   0x50429da0  NW-DR-R0 writes its 3-word r0-invariant stub here (in PatchROM,
	//               AFTER patch_68k returns).  Do NOT use this address.
	//   0x50429dac  TABLE_BASE array   (10 words = 40 bytes)
	//   0x50429dd4  STACK_TABLE array  (10 words = 40 bytes)
	//   0x50429dfc  descriptor struct  (2 words:  {RFI_target, 0})
	//   0x50429e04  sentinel stack     (1 word)
	//   0x50429e08  RFI_target stub    (3 words = 12 bytes) — safely past NW-DR-R0
	//
	// Gate: SS_M10_CGRP=1 (default OFF).
	// Acceptance: SS_PROBE_68K=0x5000ed08:5 fires within 30s of boot.
	// Analysis: docs/planning/superpowers/plans/2026-06-12-m10-cgrp-user-mode.md §Task-0.
	if (ROMType == ROMTYPE_NEWWORLD && getenv("SS_M10_CGRP") &&
	    strcmp(getenv("SS_M10_CGRP"), "0") != 0) {

		// All structures in guest RAM (CGRP inline extension).
		// NK cold-init zeroes these; the EXT shim restores them before each delivery.
		// Inline layout (CGRP base = 0x68ffc1c0):
		//   CGRP+0x50 = 0x68ffc210  TABLE_BASE  (10 × 4 = 40 bytes)
		//   CGRP+0x78 = 0x68ffc238  STACK_TABLE (10 × 4 = 40 bytes)
		//   CGRP+0xa0 = 0x68ffc260  descriptor  (2 × 4 =  8 bytes)
		//   CGRP+0xa8 = 0x68ffc268  RFI_target STUB (13 words, indirect CTR branch)
		//
		// STUB uses `lis/ori r0,DR_WARM; mtctr; bctr` to avoid the ±32MB
		// direct-branch limit from RAM (0x68ffc268) to DR (0x5046e9d8, ~413MB away).
		//
		// CGRP+0x20 NOT written here — deferred to first [DR68K] dispatch in
		// probe68k_check() so NK cold-init completes before 68k regs are live.
		const uint32 CGRP_BASE  = 0x68ffc1c0;
		const uint32 TABLE_ADDR = CGRP_BASE + 0x50;  // 0x68ffc210
		const uint32 STACK_ADDR = CGRP_BASE + 0x78;  // 0x68ffc238
		const uint32 DESC_ADDR  = CGRP_BASE + 0xa0;  // 0x68ffc260
		const uint32 STUB_ADDR  = CGRP_BASE + 0xa8;  // 0x68ffc268
		// DR_WARM is the DR emulator's COLD warm-entry trampoline, NOT the
		// per-instruction dispatch loop.  --- DEAD-END WARNING (M13, 2026-06-13;
		// full findings in docs/planning/M13-FINDINGS-interrupt-delivery.md) ---
		// The 68k DR is a *recompiler*: steady-state 68k executes in a dynamic code
		// cache at 0x17fa0000-0x17ffffff, and 0x5046e9d8 is visited exactly ONCE per
		// re-entry (probe: 0 visits across 25949 dec-expiries of live DR execution).
		// So you CANNOT inject 68k interrupts by patching/polling at this address as
		// if it were an instruction-boundary loop (Approach B, falsified), and you
		// cannot vector here from an arbitrary PC with a coherent register file.
		// Worse: re-entering DR_WARM via this STUB runs with r29 = the COLD ROM
		// dispatch table (~0x504920f8), while the live warm DR uses r29 = a RAM
		// table (0x17ffeb20) inside the code cache.  Dispatching through the cold
		// table lands in uncompiled dead-fill (0xDEADBEEF -> stfdu @ ~0x100259dc)
		// => the intermittent SIGTRAP.  Restoring r29/r30 to the warm RAM values
		// does NOT fix it either (the non-volatile 68k regs r14-r31 are also stale).
		// The only architecturally-sound delivery is via the NK's own CGRP/EXT path
		// (which resumes the recompiled world at a safe point) — see the bake-off doc.
		const uint32 DR_WARM    = 0x5046e9d8;

		// 1. TABLE_BASE: 10 entries → DESC_ADDR.
		const uint32 STACK_PLACEHOLDER = 0x17ffe000;
		for (int i = 0; i < 10; i++)
			WriteMacInt32(TABLE_ADDR + i*4, DESC_ADDR);

		// 2. STACK_TABLE: placeholder (STUB overwrites r1 anyway).
		for (int i = 0; i < 10; i++)
			WriteMacInt32(STACK_ADDR + i*4, STACK_PLACEHOLDER);

		// 3. Descriptor: {STUB_ADDR, 0}
		WriteMacInt32(DESC_ADDR,     STUB_ADDR);
		WriteMacInt32(DESC_ADDR + 4, 0);

		// 4. Padding word at 0x429e04
		WriteMacInt32(ROMBase + 0x429e04, 0);

		// 5. RFI_target STUB at STUB_ADDR=0x68ffc268 (guest RAM, 16 words = 64 bytes):
		//
		//   After rfi, we are in PPC user mode.  r16 = context_block (set by the
		//   delivery function at 0x503143a0; r6 = *(SPRG0-0x14)).
		//   NK sets r1=2 (NK-internal value) on RFI, not the old 68k A7.
		//
		//   STUB: load old_A7 = KDP+4 (SPRG1 save = pre-interrupt r1 = 68k A7).
		//   Guard: if A7 < 32KB, stack not yet initialized — return to NK.
		//   Then: read interrupted PC from r16+0x1c4, push 6-byte 68k exception
		//   frame on the 68k stack (SR=0, PC=interrupted_PC), update r16+0x1c4 to
		//   the handler address, set r24=0x5000ed08 and r1=new_A7, then branch to
		//   DR_WARM so the DR emulator picks up the handler with the frame in place.
		//
		//   r5  = scratch (old A7 / new A7 base)
		//   r12 = scratch (interrupted PC from live r24 — DR's 68k PC register)
		//   r1  = new 68k A7 (old_A7 - 6) — DR_WARM uses live r1 as 68k A7
		//
		//   r24 at STUB entry = interrupted 68k PC (NK saves/restores all GPRs
		//   including r24 across the exception delivery).  Reading r16+0x1c4 is
		//   UNRELIABLE — different NK context blocks have garbage there.
		WriteMacInt32(STUB_ADDR,      0x3CA06900u);  // lis    r5, 0x6900
		WriteMacInt32(STUB_ADDR +  4, 0x80A5E004u);  // lwz    r5, -0x1ffc(r5) → *(0x68FFE004)=KDP+4=A7
		WriteMacInt32(STUB_ADDR +  8, 0x28058000u);  // cmplwi r5, 0x8000 (32KB threshold)
		WriteMacInt32(STUB_ADDR + 12, 0x4D800020u);  // bltlr  (A7<32KB → return to NK)
		WriteMacInt32(STUB_ADDR + 16, 0x7F0CC378u);  // mr     r12, r24  → r12 = interrupted 68k PC
		WriteMacInt32(STUB_ADDR + 20, 0x3825FFFAu);  // addi   r1, r5, -6  → r1 = old_A7-6 (new A7)
		WriteMacInt32(STUB_ADDR + 24, 0x91810002u);  // stw    r12, 2(r1) → frame PC at r1+2
		WriteMacInt32(STUB_ADDR + 28, 0x39800000u);  // li     r12, 0 (SR=0)
		WriteMacInt32(STUB_ADDR + 32, 0xB1810000u);  // sth    r12, 0(r1) → frame SR=0 at r1+0
		WriteMacInt32(STUB_ADDR + 36, 0x3F005000u);  // lis    r24, 0x5000
		WriteMacInt32(STUB_ADDR + 40, 0x6318ED08u);  // ori    r24, r24, 0xed08 → r24=0x5000ed08
		WriteMacInt32(STUB_ADDR + 44, 0x931001C4u);  // stw    r24, 0x1c4(r16) → update context
		WriteMacInt32(STUB_ADDR + 48, 0x3C005046u);  // lis    r0, 0x5046
		WriteMacInt32(STUB_ADDR + 52, 0x6000E9D8u);  // ori    r0, r0, 0xe9d8  → DR_WARM
		WriteMacInt32(STUB_ADDR + 56, 0x7C0903A6u);  // mtctr  r0
		WriteMacInt32(STUB_ADDR + 60, 0x4E800420u);  // bctr

		// 6. CGRP structural fields in guest RAM (inline extension at CGRP+0x38..+0x4c).
		//    IMPORTANT: *(KDP-0x338) (the CGRP base pointer at 0x68ffdcc8) is
		//    intentionally left as 0 here.  Both *(KDP-0x338) and CGRP+0x20 are
		//    written together at first [DR68K] dispatch in ppc-cpu.cpp.
		WriteMacInt32(CGRP_BASE + 0x38, 1);           // non-zero guard
		WriteMacInt32(CGRP_BASE + 0x3c, TABLE_ADDR);  // TABLE_BASE ptr (0x68ffc210)
		WriteMacInt32(CGRP_BASE + 0x40, STACK_ADDR);  // STACK_TABLE ptr (0x68ffc238)
		WriteMacInt32(CGRP_BASE + 0x44, 10);          // count; src_idx 9 < 10
		// +0x4c must equal *(SPRG0-0x1c) at delivery time.  Pre-fill with the
		// value that works at NK cold-init; the EXT shim re-syncs before each delivery.
		WriteMacInt32(CGRP_BASE + 0x4c, 0x68ffc0e0);

	}

	// Patch ZeroScrap() for clipboard exchange with host OS
	uint32 zero_scrap = find_rom_trap(0xa9fc);	// ZeroScrap()
	if (zero_scrap) {
		wp = (uint16 *)(ROMBaseHost + ZERO_SCRAP_PATCH_SPACE);
		*wp++ = htons(M68K_EMUL_OP_ZERO_SCRAP);
		*wp++ = htons(M68K_JMP);
		*wp++ = htons((ROMBase + zero_scrap) >> 16);
		*wp++ = htons((ROMBase + zero_scrap) & 0xffff);
		base = ROMBase + ReadMacInt32(ROMBase + 0x22);
		WriteMacInt32(base + 4 * (0xa9fc & 0x3ff), ZERO_SCRAP_PATCH_SPACE);
	} else
		fprintf(stderr, "[ROMPATCH] ZeroScrap GUARDED-SKIP (find_rom_trap(0xa9fc)=0 — stub would jmp ROMBase+0)\n");

	// Patch PutScrap() for clipboard exchange with host OS
	uint32 put_scrap = find_rom_trap(0xa9fe);	// PutScrap()
	if (put_scrap) {
		wp = (uint16 *)(ROMBaseHost + PUT_SCRAP_PATCH_SPACE);
		*wp++ = htons(M68K_EMUL_OP_PUT_SCRAP);
		*wp++ = htons(M68K_JMP);
		*wp++ = htons((ROMBase + put_scrap) >> 16);
		*wp++ = htons((ROMBase + put_scrap) & 0xffff);
		base = ROMBase + ReadMacInt32(ROMBase + 0x22);
		WriteMacInt32(base + 4 * (0xa9fe & 0x3ff), PUT_SCRAP_PATCH_SPACE);
	} else
		fprintf(stderr, "[ROMPATCH] PutScrap GUARDED-SKIP (find_rom_trap(0xa9fe)=0 — stub would jmp ROMBase+0)\n");

	// Patch GetScrap() for clipboard exchange with host OS
	uint32 get_scrap = find_rom_trap(0xa9fd);	// GetScrap()
	if (get_scrap) {
		wp = (uint16 *)(ROMBaseHost + GET_SCRAP_PATCH_SPACE);
		*wp++ = htons(M68K_EMUL_OP_GET_SCRAP);
		*wp++ = htons(M68K_JMP);
		*wp++ = htons((ROMBase + get_scrap) >> 16);
		*wp++ = htons((ROMBase + get_scrap) & 0xffff);
		base = ROMBase + ReadMacInt32(ROMBase + 0x22);
		WriteMacInt32(base + 4 * (0xa9fd & 0x3ff), GET_SCRAP_PATCH_SPACE);
	} else
		fprintf(stderr, "[ROMPATCH] GetScrap GUARDED-SKIP (find_rom_trap(0xa9fd)=0 — stub would jmp ROMBase+0)\n");

	// Patch SynchIdleTime()
	if (PrefsFindBool("idlewait")) {
		base = find_rom_trap(0xabf7);							// SynchIdleTime()
		if (base) {
			base += 4;
			wp = (uint16 *)(ROMBaseHost + base);
			D(bug("SynchIdleTime at %08lx\n", base));
			if (ntohs(*wp) == 0x2078) {							// movea.l	ExpandMem,a0
				*wp++ = htons(M68K_EMUL_OP_IDLE_TIME);
				*wp = htons(M68K_NOP);
			}
			else if (ntohs(*wp) == 0x70fe)						// moveq	#-2,d0
				*wp++ = htons(M68K_EMUL_OP_IDLE_TIME_2);
			else {
				D(bug("SynchIdleTime patch not installed\n"));
			}
		} else
			fprintf(stderr, "[ROMPATCH] SynchIdleTime GUARDED-SKIP (find_rom_trap(0xabf7)=0 — would patch ROM offset 4)\n");
	}

	// Construct list of all sifters used by sound components in ROM
	D(bug("Searching for sound components with type sdev in ROM\n"));
	uint32 thing = find_rom_resource(FOURCC('t','h','n','g'));
	while (thing) {
		thing += ROMBase;
		D(bug(" found %c%c%c%c %c%c%c%c\n", ReadMacInt8(thing), ReadMacInt8(thing + 1), ReadMacInt8(thing + 2), ReadMacInt8(thing + 3), ReadMacInt8(thing + 4), ReadMacInt8(thing + 5), ReadMacInt8(thing + 6), ReadMacInt8(thing + 7)));
		if (ReadMacInt32(thing) == FOURCC('s','d','e','v') && ReadMacInt32(thing + 4) == FOURCC('s','i','n','g')) {
			WriteMacInt32(thing + 4, FOURCC('a','w','g','c'));
			D(bug(" found sdev component at offset %08x in ROM\n", thing));
			AddSifter(ReadMacInt32(thing + componentResType), ReadMacInt16(thing + componentResID));
			if (ReadMacInt32(thing + componentPFCount))
				AddSifter(ReadMacInt32(thing + componentPFResType), ReadMacInt16(thing + componentPFResID));
		}
		thing = find_rom_resource(FOURCC('t','h','n','g'), 4711, true);
	}

	// Patch component code
	D(bug("Patching sifters in ROM\n"));
	for (int i=0; i<num_sifters; i++) {
		if ((thing = find_rom_resource(sifter_list[i].type, sifter_list[i].id)) != 0) {
			D(bug(" patching type %08x, id %d\n", sifter_list[i].type, sifter_list[i].id));
			// Install 68k glue code
			uint16 *wp = (uint16 *)(ROMBaseHost + thing);
			*wp++ = htons(0x4e56); *wp++ = htons(0x0000);	// link a6,#0
			*wp++ = htons(0x48e7); *wp++ = htons(0x8018);	// movem.l d0/a3-a4,-(a7)
			*wp++ = htons(0x266e); *wp++ = htons(0x000c);	// movea.l $c(a6),a3
			*wp++ = htons(0x286e); *wp++ = htons(0x0008);	// movea.l $8(a6),a4
			*wp++ = htons(M68K_EMUL_OP_AUDIO_DISPATCH);
			*wp++ = htons(0x2d40); *wp++ = htons(0x0010);	// move.l d0,$10(a6)
			*wp++ = htons(0x4cdf); *wp++ = htons(0x1801);	// movem.l (a7)+,d0/a3-a4
			*wp++ = htons(0x4e5e);							// unlk a6
			*wp++ = htons(0x4e74); *wp++ = htons(0x0008);	// rtd #8
		}
	}
	
	return true;
}


/*
 *  Install .Sony, disk and CD-ROM drivers
 */

void InstallDrivers(void)
{
	D(bug("Installing drivers...\n"));
	M68kRegisters r;
	SheepArray<SIZEOF_IOParam> pb_var;
	const uintptr pb = pb_var.addr();
	// NOTE: SS_FORCE_ALTIVEC probe was here but InstallDrivers is TOO EARLY — Gestalt('ppcf')
	// returns gestaltUndefSelectorErr (0xEA51) because the System registers 'ppcf' later in
	// boot. The probe now lives at OP_IDLE_TIME (emul_op.cpp), which fires only once the System
	// is fully up and idle. See task #26.

#if DISABLE_SCSI
	// Setup fake SCSI Globals
	r.d[0] = 0x1000;
	Execute68kTrap(0xa71e, &r);		// NewPtrSysClear()
	uint32 scsi_globals = r.a[0];
	D(bug("Fake SCSI globals at %08lx\n", scsi_globals));
	WriteMacInt32(0xc0c, scsi_globals);	// Set SCSIGlobals
#endif

	// Install floppy driver
	if (ROMType == ROMTYPE_NEWWORLD || ROMType == ROMTYPE_GOSSAMER) {

		// Force installation of floppy driver with NewWorld and Gossamer ROMs
		r.a[0] = ROMBase + sony_offset;
		r.d[0] = (uint32)SonyRefNum;
		Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
		r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~SonyRefNum * 4);	// Get driver handle from Unit Table
		Execute68kTrap(0xa029, &r);		// HLock()
		uint32 dce = ReadMacInt32(r.a[0]);
		WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset);
		WriteMacInt16(dce + dCtlFlags, SonyDriverFlags);
	}

	// Open .Sony driver
	SheepString sony_str("\005.Sony");
	WriteMacInt8(pb + ioPermssn, 0);
	WriteMacInt32(pb + ioNamePtr, sony_str.addr());
	r.a[0] = pb;
	Execute68kTrap(0xa000, &r);		// Open()

	// Install disk driver
	r.a[0] = ROMBase + sony_offset + 0x100;
	r.d[0] = (uint32)DiskRefNum;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~DiskRefNum * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	uint32 dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x100);
	WriteMacInt16(dce + dCtlFlags, DiskDriverFlags);

	// Open disk driver
	SheepString disk_str("\005.Disk");
	WriteMacInt32(pb + ioNamePtr, disk_str.addr());
	r.a[0] = pb;
	Execute68kTrap(0xa000, &r);		// Open()

	// Install CD-ROM driver unless nocdrom option given
	if (!PrefsFindBool("nocdrom")) {

		// Install CD-ROM driver
		r.a[0] = ROMBase + sony_offset + 0x200;
		r.d[0] = (uint32)CDROMRefNum;
		Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
		r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~CDROMRefNum * 4);	// Get driver handle from Unit Table
		Execute68kTrap(0xa029, &r);		// HLock()
		dce = ReadMacInt32(r.a[0]);
		WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x200);
		WriteMacInt16(dce + dCtlFlags, CDROMDriverFlags);

		// Open CD-ROM driver
		SheepString apple_cd("\010.AppleCD");
		WriteMacInt32(pb + ioNamePtr, apple_cd.addr());
		r.a[0] = pb;
		Execute68kTrap(0xa000, &r);		// Open()
	}

	// Install serial drivers
	r.a[0] = ROMBase + sony_offset + 0x300;
	r.d[0] = (uint32)-6;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~(-6) * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x300);
	WriteMacInt16(dce + dCtlFlags, 0x4d00);

	r.a[0] = ROMBase + sony_offset + 0x400;
	r.d[0] = (uint32)-7;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~(-7) * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x400);
	WriteMacInt16(dce + dCtlFlags, 0x4e00);

	r.a[0] = ROMBase + sony_offset + 0x500;
	r.d[0] = (uint32)-8;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~(-8) * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x500);
	WriteMacInt16(dce + dCtlFlags, 0x4d00);

	r.a[0] = ROMBase + sony_offset + 0x600;
	r.d[0] = (uint32)-9;
	Execute68kTrap(0xa43d, &r);		// DrvrInstallRsrvMem()
	r.a[0] = ReadMacInt32(ReadMacInt32(0x11c) + ~(-9) * 4);	// Get driver handle from Unit Table
	Execute68kTrap(0xa029, &r);		// HLock()
	dce = ReadMacInt32(r.a[0]);
	WriteMacInt32(dce + dCtlDriver, ROMBase + sony_offset + 0x600);
	WriteMacInt16(dce + dCtlFlags, 0x4e00);
}
