/*
 *  ppc-cpu.cpp - PowerPC CPU definition
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
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

#include "sysdeps.h"
#include <stdlib.h>
#include <assert.h>
#include "vm_alloc.h"
#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-cpu.hpp"
#ifdef SHEEPSHAVER
#include "machine_profile.h"   /* M3a Task 4: newworld gate in check_spcflags */
#include "mmio_bus.h"          /* M6a Wave 1: MMIO region counters on the heartbeat */
#include "dev_via6522.h"       /* M6a Wave 2 #4: top-2 VIA read registers on the heartbeat */
#else
#include "basic-kernel.hpp"
#endif

#if PPC_ENABLE_JIT
#include "cpu/jit/dyngen-exec.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
// Boot-stall watchdog (emul_op.cpp): alarms when the guest wedges pre-idle. See call site below.
extern "C" void ss_boot_stall_check(double now_s, unsigned compiled, double rate_mhz);
extern uint32 ROMBase;
extern uint8 *ROMBaseHost;
#include "cpu/jit/aarch64/ppc-jit.h"
#include "cpu/jit/aarch64/jit-heartbeat.hpp"
#include <cstddef>
#include <unordered_map>
#include <mach/mach_time.h>

/* M6a Wave 1 telemetry (M6A-DR-HANDOFF-ANALYSIS.md §5.5a / [PROBE-5]): MMIO
 * region read counters on the heartbeat.  SIGTERM/alarm boot-killers skip the
 * atexit MMIO stats dump (verified), so the per-region counts ride the periodic
 * [HB] line instead — same NULL-suffix idiom as exc= (newworld-gated by the
 * caller; paravirtual heartbeat lines stay byte-identical).  Appends
 * " mmio=S:<scc reads>/V:<via reads>" to buf; no-op when the bus is inactive. */
static void hb_append_mmio_suffix(char *buf, size_t buflen)
{
	if (!mmio_bus_active)
		return;
	char nm[32]; uint32_t base, size; MMIORegionStats st;
	uint64_t scc_reads = 0, via_reads = 0;
	bool found = false;
	for (int i = 0; MMIOBusGetStats(i, nm, &base, &size, &st); i++) {
		if (strcmp(nm, "scc8530") == 0) { scc_reads = st.reads; found = true; }
		else if (strcmp(nm, "via6522") == 0) { via_reads = st.reads; found = true; }
	}
	if (!found)
		return;
	size_t n = strlen(buf);
	if (n < buflen)
		snprintf(buf + n, buflen - n, " mmio=S:%llu/V:%llu",
		         (unsigned long long)scc_reads, (unsigned long long)via_reads);
	/* M6a Wave 2 #4: WHICH VIA registers a poll loop hammers — top-2 histogram
	 * entries, e.g. " mmio=S:0/V:8123456(IFR=8000000,T2CL=123456)". Unlocked
	 * counter loads (single-copy-atomic on AArch64; see dev_via6522.h note).
	 * Alarm-killed boots skip the atexit [VIA] reads dump, so this rides the
	 * periodic [HB] line instead. Empty when no VIA reads happened. */
	if (via_reads) {
		n = strlen(buf);
		if (n < buflen)
			VIAFormatTopReads(buf + n, buflen - n);
	}
}

// B1 execution-weighted profiler: per-PC block execution counts.
// Lightweight: one hash-map increment per block dispatch. Guarded by
// SS_JIT_PROFILE=1 env var to avoid any overhead when not profiling.
static bool jit_profile_enabled = false;
static bool jit_profile_checked = false;
static std::unordered_map<uint32_t, uint64_t> jit_profile_counts;
static uint64_t jit_profile_total = 0;

// Fallback trace: per-PC fallback counts (blocks that went to interpreter instead of JIT)
static std::unordered_map<uint32_t, uint64_t> jit_fallback_counts;
static uint64_t jit_fallback_total = 0;

// P3 flame chart: per-PC cumulative execution time (mach_absolute_time ticks)
static std::unordered_map<uint32_t, uint64_t> jit_profile_time;
static mach_timebase_info_data_t jit_timebase = {0, 0};

static double jit_ticks_to_ns(uint64_t ticks) {
	if (jit_timebase.denom == 0) mach_timebase_info(&jit_timebase);
	return (double)ticks * jit_timebase.numer / jit_timebase.denom;
}

// SS_PROBE_PC: dump registers/memory at specified guest PCs without recompilation.
// Format: SS_PROBE_PC=0xADDR[:field1,field2,...][;0xADDR2[:fields]]
// Fields: rN (GPR), [0xADDR] (guest mem 4-byte read), [rN:SIZE] (SIZE bytes from gpr(N)), or omit for full dump.
// [rN:SIZE]: SIZE in hex (0x prefix optional), clamped to 4KB. Output: hex words 4 bytes each, 8 per line.
// Logarithmic sampling: visit 1, 10, 100, 1000, 10000, 100000, ...
enum probe_field_type { PROBE_GPR, PROBE_MEM, PROBE_MEM_REG };
struct probe_field {
	probe_field_type type;
	uint32_t value;  /* reg# (PROBE_GPR/PROBE_MEM_REG) or guest addr (PROBE_MEM) */
	uint32_t size;   /* byte count for PROBE_MEM_REG; unused for other types */
};
struct probe_entry {
	uint32_t pc;
	bool dump_all;
	int n_fields;
	probe_field fields[16];
	uint64_t visits;
};
#define PROBE_MAX 8
static probe_entry s_probes[PROBE_MAX];
static int s_probe_count = -1; // -1 = not yet parsed

static bool probe_should_log(uint64_t v) {
	// Log at powers of 10: 1, 10, 100, 1000, ...
	if (v == 0) return false;
	uint64_t p = 1;
	while (p <= v) {
		if (v == p) return true;
		p *= 10;
	}
	return false;
}

static int parse_probes(const char *env) {
	if (!env || !*env) return 0;
	char *buf = strdup(env);
	int count = 0;
	// Split on ';' for multiple probes
	char *saveptr1 = NULL;
	char *tok = strtok_r(buf, ";", &saveptr1);
	while (tok && count < PROBE_MAX) {
		probe_entry *p = &s_probes[count];
		p->visits = 0;
		p->n_fields = 0;
		p->dump_all = true;
		// Split on ':' — first part is PC, optional second part is field list
		char *colon = strchr(tok, ':');
		if (colon) {
			*colon = '\0';
			p->pc = (uint32_t)strtoul(tok, NULL, 16);
			p->dump_all = false;
			// Parse comma-separated fields
			char *saveptr2 = NULL;
			char *field = strtok_r(colon + 1, ",", &saveptr2);
			while (field && p->n_fields < 16) {
				// Skip leading whitespace
				while (*field == ' ') field++;
				if (field[0] == 'r' && field[1] >= '0' && field[1] <= '9') {
					// GPR: r0..r31
					p->fields[p->n_fields].type = PROBE_GPR;
					p->fields[p->n_fields].value = (uint32_t)atoi(field + 1);
					if (p->fields[p->n_fields].value <= 31)
						p->n_fields++;
				} else if (field[0] == '[') {
					// Memory: [0xADDR] or register-indirect range [rN:SIZE]
					char *inner = field + 1;
					char *bracket = strchr(inner, ']');
					if (bracket) *bracket = '\0';
					// Detect [rN:SIZE] vs [0xADDR]
					if ((inner[0] == 'r' || inner[0] == 'R') &&
					    inner[1] >= '0' && inner[1] <= '9') {
						// Register-indirect memory range: [rN:SIZE]
						char *colon2 = strchr(inner, ':');
						if (colon2) {
							*colon2 = '\0';
							uint32_t regnum = (uint32_t)atoi(inner + 1);
							uint32_t sz = (uint32_t)strtoul(colon2 + 1, NULL, 16);
							if (sz > 0x1000) sz = 0x1000;  // clamp to 4KB
							if (sz == 0) sz = 4;             // minimum one word
							if (regnum <= 31) {
								p->fields[p->n_fields].type  = PROBE_MEM_REG;
								p->fields[p->n_fields].value = regnum;
								p->fields[p->n_fields].size  = sz;
								p->n_fields++;
							}
						}
					} else {
						// Absolute guest address: [0xADDR]
						p->fields[p->n_fields].type  = PROBE_MEM;
						p->fields[p->n_fields].value = (uint32_t)strtoul(inner, NULL, 16);
						p->fields[p->n_fields].size  = 0;
						p->n_fields++;
					}
				}
				field = strtok_r(NULL, ",", &saveptr2);
			}
		} else {
			p->pc = (uint32_t)strtoul(tok, NULL, 16);
		}
		count++;
		tok = strtok_r(NULL, ";", &saveptr1);
	}
	free(buf);
	return count;
}

// ---------------------------------------------------------------------------
// SS_SEED_MEM: no-recompile guest-memory poke knob (developer aid). Mirrors the
// SS_PROBE_PC style. Two forms:
//   SS_SEED_MEM=0xADDR=0xVAL[;...]         immediate: applied once at the
//                                          NW-trampoline-end "post-init" point
//                                          (ss_seed_mem_apply_immediate).
//   SS_SEED_MEM=0xPC:0xADDR=0xVAL[;...]    PC-triggered: applied at the FIRST
//                                          block-entry visit of 0xPC, paralleling
//                                          the SS_PROBE_PC block-entry hook.
// Each seed is a single 32-bit write. MMIO-range addresses are refused (like the
// probe). Up to SEED_MAX entries. Born from the NK spike where a KDP field had to
// be re-seeded AFTER the nanokernel zeroed it — the PC form does that with no
// rebuild. NOTE: like SS_PROBE_PC, the block-entry hook lives only in the JIT
// dispatch path, so the PC-triggered form fires in JIT execution (boot + the
// SS_TEST_JIT harness loop), not in pure interpreter execution. The immediate form
// fires at NW-trampoline-end, which a boot reaches but the SS_TEST_HEX harness does
// not.
struct seed_entry {
	bool     has_pc;   // true => PC-triggered; false => immediate
	uint32_t pc;       // trigger PC (valid when has_pc)
	uint32_t addr;     // guest address to write
	uint32_t val;      // 32-bit value to write
	bool     fired;    // one-shot: applied already
};
#define SEED_MAX 16
static seed_entry s_seeds[SEED_MAX];
static int s_seed_count = -1;   // -1 = not yet parsed

static int parse_seeds(const char *env) {
	if (!env || !*env) return 0;
	char *buf = strdup(env);
	int count = 0;
	char *saveptr = NULL;
	char *tok = strtok_r(buf, ";", &saveptr);
	while (tok && count < SEED_MAX) {
		while (*tok == ' ') tok++;
		// Each token is "ADDR=VAL" or "PC:ADDR=VAL".
		char *eq = strchr(tok, '=');
		if (!eq) { tok = strtok_r(NULL, ";", &saveptr); continue; }
		*eq = '\0';
		const char *valstr = eq + 1;
		seed_entry *s = &s_seeds[count];
		s->fired = false;
		s->val = (uint32_t)strtoul(valstr, NULL, 16);
		char *colon = strchr(tok, ':');
		if (colon) {
			*colon = '\0';
			s->has_pc = true;
			s->pc   = (uint32_t)strtoul(tok, NULL, 16);
			s->addr = (uint32_t)strtoul(colon + 1, NULL, 16);
		} else {
			s->has_pc = false;
			s->pc   = 0;
			s->addr = (uint32_t)strtoul(tok, NULL, 16);
		}
		count++;
		tok = strtok_r(NULL, ";", &saveptr);
	}
	free(buf);
	return count;
}

static void seed_apply(seed_entry *s) {
	if (s->fired) return;
	s->fired = true;
	if (vm_is_mmio(s->addr)) {
		fprintf(stderr, "[SEED] addr=0x%08x val=0x%08x REFUSED (MMIO range)\n",
		        s->addr, s->val);
		fflush(stderr);
		return;
	}
	vm_write_memory_4(s->addr, s->val);
	if (s->has_pc)
		fprintf(stderr, "[SEED] pc=0x%08x addr=0x%08x val=0x%08x applied\n",
		        s->pc, s->addr, s->val);
	else
		fprintf(stderr, "[SEED] pc=immediate addr=0x%08x val=0x%08x applied\n",
		        s->addr, s->val);
	fflush(stderr);
}

static inline void ss_seed_mem_ensure_parsed() {
	if (__builtin_expect(s_seed_count < 0, false)) {
		s_seed_count = parse_seeds(getenv("SS_SEED_MEM"));
		if (s_seed_count > 0)
			fprintf(stderr, "[SEED] parsed %d seed(s) from SS_SEED_MEM\n", s_seed_count);
	}
}

// Immediate seeds (no PC): apply once at the post-init point. Non-static: called
// from the NW-trampoline-end in sheepshaver_glue.cpp.
void ss_seed_mem_apply_immediate(void) {
	ss_seed_mem_ensure_parsed();
	if (s_seed_count <= 0) return;
	for (int i = 0; i < s_seed_count; i++)
		if (!s_seeds[i].has_pc) seed_apply(&s_seeds[i]);
}

// PC-triggered seeds: apply on the first block-entry visit of `pc`. Non-static:
// called from the JIT block-entry hooks here and the SS_TEST_JIT harness loop.
void ss_seed_mem_check_pc(uint32_t pc) {
	ss_seed_mem_ensure_parsed();
	if (__builtin_expect(s_seed_count <= 0, true)) return;
	for (int i = 0; i < s_seed_count; i++)
		if (s_seeds[i].has_pc && !s_seeds[i].fired && s_seeds[i].pc == pc)
			seed_apply(&s_seeds[i]);
}

// Instruction mix: aggregate execution counts by primary opcode
extern "C" void jit_profile_opcode_mix_json(char *buf, int bufsz) {
	if (!jit_profile_enabled || jit_profile_counts.empty()) {
		snprintf(buf, bufsz, "{\"enabled\":false,\"opcodes\":[]}");
		return;
	}
	// Aggregate counts by primary opcode (bits 0-5 of the PPC instruction)
	uint64_t opc_counts[64] = {0};
	for (const auto &p : jit_profile_counts) {
		uint32_t pc_addr = p.first;
		uint32_t opc = 0;
		if (pc_addr < RAMSize) {
			opc = ntohl(*(uint32_t*)(RAMBaseHost + pc_addr));
		} else if (pc_addr >= ROMBase && pc_addr < ROMBase + 0x500000 && ROMBaseHost) {
			opc = ntohl(*(uint32_t*)(ROMBaseHost + (pc_addr - ROMBase)));
		}
		int primary = (opc >> 26) & 0x3F;
		opc_counts[primary] += p.second;
	}

	// Sort by count descending
	struct OpcEntry { int opc; uint64_t count; };
	std::vector<OpcEntry> sorted;
	for (int i = 0; i < 64; i++) {
		if (opc_counts[i] > 0) sorted.push_back({i, opc_counts[i]});
	}
	std::sort(sorted.begin(), sorted.end(), [](const OpcEntry &a, const OpcEntry &b) { return a.count > b.count; });

	int pos = 0;
	pos += snprintf(buf + pos, bufsz - pos, "{\"enabled\":true,\"total\":%llu,\"opcodes\":[",
	                (unsigned long long)jit_profile_total);
	for (size_t i = 0; i < sorted.size() && pos < bufsz - 100; i++) {
		if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
		double pct = jit_profile_total > 0 ? (100.0 * sorted[i].count / jit_profile_total) : 0;
		// PPC primary opcode names (common ones)
		const char *name = "?";
		switch (sorted[i].opc) {
			case 14: name = "addi"; break;
			case 15: name = "addis"; break;
			case 16: name = "bc"; break;
			case 18: name = "b"; break;
			case 19: name = "cr/bclr/bcctr"; break;
			case 21: name = "rlwinm"; break;
			case 24: name = "ori"; break;
			case 28: name = "andi."; break;
			case 31: name = "alu/xo"; break;
			case 32: name = "lwz"; break;
			case 33: name = "lwzu"; break;
			case 34: name = "lbz"; break;
			case 36: name = "stw"; break;
			case 37: name = "stwu"; break;
			case 38: name = "stb"; break;
			case 40: name = "lhz"; break;
			case 42: name = "lha"; break;
			case 44: name = "sth"; break;
			case 46: name = "lmw"; break;
			case 47: name = "stmw"; break;
			case 48: name = "lfs"; break;
			case 50: name = "lfd"; break;
			case 52: name = "stfs"; break;
			case 54: name = "stfd"; break;
			case 59: name = "fp-single"; break;
			case 63: name = "fp-double"; break;
			case 4:  name = "altivec"; break;
			default: break;
		}
		pos += snprintf(buf + pos, bufsz - pos,
			"{\"opc\":%d,\"name\":\"%s\",\"count\":%llu,\"pct\":%.2f}",
			sorted[i].opc, name, (unsigned long long)sorted[i].count, pct);
	}
	pos += snprintf(buf + pos, bufsz - pos, "]}");
}

// Heat map: aggregate execution counts by 64K region
extern "C" void jit_profile_heatmap_json(char *buf, int bufsz) {
	if (!jit_profile_enabled || jit_profile_counts.empty()) {
		snprintf(buf, bufsz, "{\"enabled\":false,\"regions\":[]}");
		return;
	}
	std::unordered_map<uint32_t, uint64_t> region_counts;
	for (const auto &p : jit_profile_counts) {
		uint32_t region = p.first & 0xFFFF0000; // 64K alignment
		region_counts[region] += p.second;
	}
	std::vector<std::pair<uint32_t, uint64_t>> sorted(region_counts.begin(), region_counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

	int pos = 0;
	pos += snprintf(buf + pos, bufsz - pos, "{\"enabled\":true,\"total\":%llu,\"regions\":[",
	                (unsigned long long)jit_profile_total);
	for (size_t i = 0; i < sorted.size() && i < 50 && pos < bufsz - 100; i++) {
		if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
		double pct = jit_profile_total > 0 ? (100.0 * sorted[i].second / jit_profile_total) : 0;
		const char *label = "RAM";
		if (sorted[i].first >= ROMBase && sorted[i].first < ROMBase + 0x500000) label = "ROM";
		else if (sorted[i].first >= 0x50460000 && sorted[i].first < 0x50500000) label = "DR";
		pos += snprintf(buf + pos, bufsz - pos,
			"{\"base\":\"0x%08x\",\"count\":%llu,\"pct\":%.2f,\"label\":\"%s\"}",
			sorted[i].first, (unsigned long long)sorted[i].second, pct, label);
	}
	pos += snprintf(buf + pos, bufsz - pos, "]}");
}

// Returns top N blocks by time as JSON
extern "C" void jit_profile_time_get_json(char *buf, int bufsz, int top_n) {
	if (!jit_profile_enabled || jit_profile_time.empty()) {
		snprintf(buf, bufsz, "{\"enabled\":false,\"blocks\":[]}");
		return;
	}
	std::vector<std::pair<uint32_t, uint64_t>> sorted(jit_profile_time.begin(), jit_profile_time.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	if (top_n > (int)sorted.size()) top_n = sorted.size();
	if (top_n > 100) top_n = 100;

	uint64_t total_time = 0;
	for (const auto &p : jit_profile_time) total_time += p.second;

	int pos = 0;
	pos += snprintf(buf + pos, bufsz - pos,
		"{\"enabled\":true,\"totalTimeNs\":%.0f,\"blocks\":[", jit_ticks_to_ns(total_time));
	for (int i = 0; i < top_n && pos < bufsz - 200; i++) {
		if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
		double ns = jit_ticks_to_ns(sorted[i].second);
		double pct = total_time > 0 ? (100.0 * sorted[i].second / total_time) : 0;
		uint64_t count = jit_profile_counts.count(sorted[i].first) ? jit_profile_counts[sorted[i].first] : 0;
		double avg_ns = count > 0 ? ns / count : 0;
		pos += snprintf(buf + pos, bufsz - pos,
			"{\"pc\":\"0x%08x\",\"timeNs\":%.0f,\"pct\":%.2f,\"count\":%llu,\"avgNs\":%.1f}",
			sorted[i].first, ns, pct, (unsigned long long)count, avg_ns);
	}
	pos += snprintf(buf + pos, bufsz - pos, "]}");
}

extern "C" void jit_fallback_get_json(char *buf, int bufsz, int top_n) {
	if (!jit_profile_enabled || jit_fallback_counts.empty()) {
		snprintf(buf, bufsz, "{\"enabled\":false,\"total\":0,\"blocks\":[]}");
		return;
	}
	std::vector<std::pair<uint32_t, uint64_t>> sorted(jit_fallback_counts.begin(), jit_fallback_counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	if (top_n > (int)sorted.size()) top_n = sorted.size();
	if (top_n > 100) top_n = 100;

	int pos = 0;
	pos += snprintf(buf + pos, bufsz - pos, "{\"enabled\":true,\"total\":%llu,\"uniqueBlocks\":%zu,\"blocks\":[",
	                (unsigned long long)jit_fallback_total, jit_fallback_counts.size());
	for (int i = 0; i < top_n && pos < bufsz - 200; i++) {
		if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
		// Read the opcode at this PC for identification
		uint32_t opc = 0;
		uint32_t pc_addr = sorted[i].first;
		if (pc_addr < RAMSize) {
			opc = ntohl(*(uint32_t*)(RAMBaseHost + pc_addr));
		} else if (pc_addr >= ROMBase && pc_addr < ROMBase + 0x500000 && ROMBaseHost) {
			opc = ntohl(*(uint32_t*)(ROMBaseHost + (pc_addr - ROMBase)));
		}
		int primary = (opc >> 26) & 0x3F;
		int xo = (opc >> 1) & 0x3FF;
		pos += snprintf(buf + pos, bufsz - pos,
			"{\"pc\":\"0x%08x\",\"count\":%llu,\"opcode\":\"0x%08x\",\"primary\":%d,\"xo\":%d}",
			pc_addr, (unsigned long long)sorted[i].second, opc, primary, xo);
	}
	pos += snprintf(buf + pos, bufsz - pos, "]}");
}

// Returns the top N hot blocks as a JSON string for the Inspector
extern "C" void jit_profile_get_json(char *buf, int bufsz, int top_n) {
	if (!jit_profile_enabled || jit_profile_counts.empty()) {
		snprintf(buf, bufsz, "{\"enabled\":false,\"total\":0,\"blocks\":[]}");
		return;
	}
	// Sort by count descending
	std::vector<std::pair<uint32_t, uint64_t>> sorted(jit_profile_counts.begin(), jit_profile_counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	if (top_n > (int)sorted.size()) top_n = sorted.size();
	if (top_n > 100) top_n = 100;

	int pos = 0;
	pos += snprintf(buf + pos, bufsz - pos, "{\"enabled\":true,\"total\":%llu,\"uniqueBlocks\":%zu,\"blocks\":[",
	                (unsigned long long)jit_profile_total, jit_profile_counts.size());
	for (int i = 0; i < top_n && pos < bufsz - 100; i++) {
		if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
		double pct = jit_profile_total > 0 ? (100.0 * sorted[i].second / jit_profile_total) : 0;
		pos += snprintf(buf + pos, bufsz - pos,
			"{\"pc\":\"0x%08x\",\"count\":%llu,\"pct\":%.2f}",
			sorted[i].first, (unsigned long long)sorted[i].second, pct);
	}
	pos += snprintf(buf + pos, bufsz - pos, "]}");
}
/* Compile-time verification of the spcflags mask offset used by the JIT's
 * block-entry interrupt poll.  PPCR_SPCFLAGS in ppc-jit.cpp must match.
 * basic_spcflags has `uint32 mask` as its FIRST member, so
 * &spcflags == &spcflags.mask; the poll reads a W-word at this offset. */
static_assert(offsetof(powerpc_registers, spcflags) == 1056,
              "spcflags offset changed — update PPCR_SPCFLAGS in ppc-jit.cpp");
#if KPX_MAX_CPUS == 1
/* lwarx/stwcx. reservation fields, addressed by the JIT (P3a) via RSTATE+offset.
 * PPCR_RESERVE_VALID / PPCR_RESERVE_ADDR in ppc-jit.cpp must match. */
static_assert(offsetof(powerpc_registers, reserve_valid) == 1060,
              "reserve_valid offset changed — update PPCR_RESERVE_VALID in ppc-jit.cpp");
static_assert(offsetof(powerpc_registers, reserve_addr) == 1064,
              "reserve_addr offset changed — update PPCR_RESERVE_ADDR in ppc-jit.cpp");
#endif

#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static double jit_elapsed_s() {
	static struct timespec t0 = {0,0};
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t0.tv_sec == 0) t0 = t;
	return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}
static FILE *jit_log_file = nullptr;
/* Open the diagnostic log.
 *
 * Path resolution:
 *   - SS_JIT_DIAG_LOG, if set and non-empty, is used verbatim (no symlink touched).
 *   - Otherwise a per-instance file /tmp/jit_diag.<YYYYMMDD-HHMMSS>.<pid>.log is
 *     created and the stable alias /tmp/jit_diag.log is re-pointed (symlink) at it.
 *
 * WHY per-instance files: the default used to be a single shared /tmp/jit_diag.log
 * opened with "w", so ANY concurrent SheepShaver process reaching a heartbeat would
 * truncate another instance's log mid-run — silently corrupting a concurrent
 * boot-time measurement.  Timestamp+pid naming makes each run's log unique; the
 * symlink keeps `tail -f /tmp/jit_diag.log` and jit-analyze.py defaults working.
 * Note: harness test vectors (SS_TEST_HEX) exit in milliseconds and never reach
 * the first 5s heartbeat, so they never create log files. */
static void jit_diag_log_open(void) {
	if (jit_log_file) return;
	const char *path = getenv("SS_JIT_DIAG_LOG");
	char ts_path[128];
	bool make_link = false;
	if (!path || !*path) {
		time_t now = time(NULL);
		struct tm tmv;
		localtime_r(&now, &tmv);
		snprintf(ts_path, sizeof(ts_path), "/tmp/jit_diag.%04d%02d%02d-%02d%02d%02d.%d.log",
		         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)getpid());
		path = ts_path;
		make_link = true;
	}
	jit_log_file = fopen(path, "w");
	fprintf(stderr, "[JIT] diagnostic log: %s\n", path);
	if (jit_log_file && make_link) {
		/* Refresh the latest-run alias.  unlink() also replaces a stale regular
		 * file left behind by a pre-symlink build.  Failure is non-fatal: the
		 * real log still works, only the convenience alias is lost. */
		unlink("/tmp/jit_diag.log");
		if (symlink(path, "/tmp/jit_diag.log") != 0)
			fprintf(stderr, "[JIT] warning: could not update /tmp/jit_diag.log symlink: %s\n",
			        strerror(errno));
	}
}
/* Notable events (init, interrupts, stuck, flush) → stderr */
#define JIT_LOG(fmt, ...) fprintf(stderr, "[JIT %.2fs] " fmt "\n", jit_elapsed_s(), ##__VA_ARGS__)
/* Verbose events (heartbeat, per-interrupt detail) → file only */
#define JIT_FLOG(fmt, ...) do { if (jit_log_file) { fprintf(jit_log_file, "[JIT %.2fs] " fmt "\n", jit_elapsed_s(), ##__VA_ARGS__); } } while(0)
#endif

#if PPC_PROFILE_GENERIC_CALLS
uint32 powerpc_cpu::generic_calls_count[PPC_I(MAX)];
static int generic_calls_ids[PPC_I(MAX)];
const int generic_calls_top_ten = 20;

int generic_calls_compare(const void *e1, const void *e2)
{
	const int id1 = *(const int *)e1;
	const int id2 = *(const int *)e2;
	return powerpc_cpu::generic_calls_count[id2] - powerpc_cpu::generic_calls_count[id1];
}
#endif

#if PPC_PROFILE_REGS_USE
int register_info_compare(const void *e1, const void *e2)
{
	const powerpc_cpu::register_info *ri1 = (powerpc_cpu::register_info *)e1;
	const powerpc_cpu::register_info *ri2 = (powerpc_cpu::register_info *)e2;
	return ri2->count - ri1->count;
}
#endif

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
/* Inline interpreter-call bridge for the AArch64 JIT (dyngen do_generic
 * equivalent).
 *
 * When the JIT hits an opcode it cannot compile natively (compile_one returns
 * false), instead of marking the block incomplete it emits an inline call to
 * this function for that one instruction.  The bridge decodes and executes the
 * single PPC instruction through the *same* interpreter handler the interpreter
 * loop uses (decode()->execute()), keeping the block "complete" and avoiding the
 * mixed JIT/interpreter execution of the same PC that corrupts the ROM's 68k
 * emulator.  See docs/superpowers/research/2026-06-02-dyngen-mechanisms.md GAP 3.
 *
 * s_active_cpu is set at the top of powerpc_cpu::execute(): the JIT only ever
 * runs from inside execute(), single-threaded, on one cpu object across nested
 * execute_depth calls, so it is always valid when the bridge fires.  The JIT
 * passes the regs pointer (RSTATE/x20), not the cpu object, and regs_ptr() has
 * no back-pointer to the cpu, so we recover the cpu this way rather than from
 * the regs pointer. */
static powerpc_cpu *s_active_cpu = NULL;

/* ---- In-memory execution trace ring (SS_JIT_TRACE_RING=1) -----------------
 *
 * Zero-I/O execution history for crash diagnosis.  Every JIT block execution,
 * interpreter block entry, and inline interpreter call is recorded into a
 * fixed-size ring (~20 stores per record — boot speed is essentially
 * unaffected, unlike SS_JIT_TRACE's fprintf+fflush per block).  The SIGSEGV
 * crash handler (sheepshaver_glue.cpp) calls ppc_jit_dump_trace_ring() to
 * write the ring to /tmp/ss_jit_ring.txt, giving the exact block-execution
 * history leading up to a deterministic crash.
 *
 * Record types:
 *   'J' — JIT block executed; from_pc/to_pc = block entry/exit, registers AFTER
 *   'I' — interpreter block entered; from_pc = entry PC, registers BEFORE
 *   'C' — inline interpreter call; from_pc = instruction PC, registers BEFORE
 *
 * Register fields use the ROM 68k (DR) emulator's conventions:
 *   r24 = 68k PC, r27 = opcode, r29 = handler address,
 *   a[0..7] = PPC r16..r23 = 68k A0..A7 (a7 = 68k stack pointer). */
struct jit_ring_rec {
	uint32 from_pc, to_pc;
	uint32 r1;  /* 68k A7 (stack pointer) in the DR emulator convention */
	uint32 r24, r27, r29, lr, ctr, cr;
	uint32 a[8];
	uint32 d[8];  /* 68k D0-D7 = PPC r8-r15 in the DR emulator */
	uint32 opcode;
	char   type;
};
#define JIT_RING_SIZE 0x40000  /* 256K records; power of 2 */
static jit_ring_rec *jit_ring = NULL;
static uint32 jit_ring_idx = 0;

extern "C" void ppc_jit_dump_trace_ring(void); /* defined below; used by the trigger */

/* ---- Boot-time region profiling ----
 *
 * WHY: JIT boot takes 10-22+ min vs ~10s for the interpreter, yet the JIT executes
 * blocks faster per-block.  That means either (a) the guest executes far more
 * instructions under JIT, or (b) per-block/transition overhead dominates.  The DR
 * (68k) emulator at ROM+0x460000..+0x500000 is NOT JIT-compilable, so JIT-mode boot
 * ping-pongs between JIT code (nanokernel/RAM) and interpreted code (DR emulator).
 * These counters measure the work split and the transition rate in BOTH modes so
 * the two can be compared directly.
 *
 * Logged by the heartbeat every ~5s as:
 *   [JIT ...]    jNK=n jDR=n jRAM=n | iNK=n iDR=n iRAM=n | j2i=n i2j=n
 *   [INTERP ...] iNK=n iDR=n iRAM=n        (pure interpreter mode, SS_USE_JIT=0)
 *
 * Region key: NK = nanokernel+toolbox ROM (JIT-compilable), DR = 68k DR emulator
 * (interpreter-only), RAM = guest RAM (JIT-compilable), OTH = everything else. */
enum { RGN_NK = 0, RGN_DR = 1, RGN_RAM = 2, RGN_OTH = 3 };
static uint64 rgn_jit_blocks[4];     /* JIT-executed blocks, by block entry PC */
static uint64 rgn_interp_blocks[4];  /* interpreter-executed blocks, by block entry PC */
static uint64 rgn_jit_to_interp;     /* JIT dispatch fell through to interpreter */
static uint64 rgn_interp_to_jit;     /* interpreter loop handed off to JIT */

static inline int rgn_classify(uint32 pc) {
	/* ROMBase is 0x50000000 on this port; DR emulator at ROM+0x460000..+0x500000 */
	if (pc < 0x50000000) return RGN_RAM;
	if (pc < 0x50460000) return RGN_NK;
	if (pc < 0x50500000) return RGN_DR;
	return RGN_OTH;
}

static void jit_ring_init_once(void) {
	static bool done = false;
	if (done) return;
	done = true;
	const char *e = getenv("SS_JIT_TRACE_RING");
	if (e && *e == '1')
		jit_ring = (jit_ring_rec *)calloc(JIT_RING_SIZE, sizeof(jit_ring_rec));
}

static inline void jit_ring_record(powerpc_registers *r, char type,
                                   uint32 from_pc, uint32 to_pc, uint32 opcode) {
	if (!jit_ring) return;
	jit_ring_rec *rec = &jit_ring[jit_ring_idx & (JIT_RING_SIZE - 1)];
	jit_ring_idx++;
	rec->type = type; rec->from_pc = from_pc; rec->to_pc = to_pc; rec->opcode = opcode;
	rec->r1 = r->gpr[1];
	rec->r24 = r->gpr[24]; rec->r27 = r->gpr[27]; rec->r29 = r->gpr[29];
	rec->lr = r->lr; rec->ctr = r->ctr; rec->cr = r->cr.get();
	for (int i = 0; i < 8; i++) rec->a[i] = r->gpr[16 + i];
	for (int i = 0; i < 8; i++) rec->d[i] = r->gpr[8 + i];  /* 68k D0-D7 */

	/* SS_JIT_WATCH_ADDR=<hex>[,<hex>...]: generic software watchpoints on up to
	 * 4 guest words.  After every recorded event, read each (4-aligned) word
	 * and report every change, identifying the block/event that made it.
	 * SS_JIT_WATCH_DUMPS=<n> (default 3): how many of the first changes also
	 * dump the trace ring (set 0 when watching busy locations like stack slots).
	 * Used for the bug-#2 hunt: watch the CD-ROM DrvSts flags word and the
	 * Device Manager argument slot simultaneously. */
	{
		static int      awatch_state = -1;   /* -1 unread, 0 off, N = count */
		static uint32   awatch_addr[4];
		static uint32   awatch_last[4];
		static bool     awatch_have_last[4];
		static int      awatch_dumps = 0;
		static int      awatch_dump_budget = 3;
		if (awatch_state < 0) {
			const char *e = getenv("SS_JIT_WATCH_ADDR");
			awatch_state = 0;
			if (e && *e) {
				char buf[128]; strncpy(buf, e, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
				char *save = NULL;
				for (char *tok = strtok_r(buf, ",", &save); tok && awatch_state < 4;
				     tok = strtok_r(NULL, ",", &save))
					awatch_addr[awatch_state++] = (uint32)strtoul(tok, NULL, 16) & ~3u;
			}
			const char *d = getenv("SS_JIT_WATCH_DUMPS");
			if (d) awatch_dump_budget = atoi(d);
		}
		for (int w = 0; w < awatch_state; w++) {
			// M1: device reads are side-effecting; tools must never touch them (§2b).
			if (vm_is_mmio(awatch_addr[w])) continue;
			uint32 now = vm_read_memory_4(awatch_addr[w]);
			if (awatch_have_last[w] && now != awatch_last[w]) {
				fprintf(stderr, "[WATCH] pc=%08x addr=%08x value=%08x  (was %08x record #%u type=%c block %08x->%08x sp=%08x r24=%08x)\n",
				        from_pc, awatch_addr[w], now, awatch_last[w], jit_ring_idx, type, from_pc, to_pc,
				        rec->r1, rec->r24);
				fflush(stderr);
				/* Dump the ring on the first few changes so the lead-up is captured. */
				if (awatch_dumps < awatch_dump_budget) { awatch_dumps++; ppc_jit_dump_trace_ring(); }
			}
			awatch_last[w] = now;
			awatch_have_last[w] = true;
		}
	}

	/* SS_JIT_WATCH_STUB=1: software watchpoint on the Mixed Mode switch-back
	 * stub.  ROM block 0x5010bb90 writes 0xFE020000 (the Mixed Mode F-line
	 * trap) at [r1 - 0x70]; the 68k routine called via Mixed Mode later
	 * RTSes to that stub.  The deterministic boot crash is that the stub
	 * reads back as 00000000.  This watchpoint:
	 *   - arms after every execution of block 0x5010bb90 (stub freshly written),
	 *   - verifies the write actually landed,
	 *   - after every subsequent block, checks the stub still holds 0xFE020000,
	 *   - on the first change, prints the culprit block + disarms.
	 * In a healthy flow the first change happens only after the 68k has
	 * consumed the stub (r24 reaches the stub address). */
	{
		static int   watch_enabled = -1;
		static uint32 watch_addr = 0;
		static uint32 watch_arm_idx = 0;
		if (watch_enabled < 0) {
			const char *e = getenv("SS_JIT_WATCH_STUB");
			watch_enabled = (e && *e == '1') ? 1 : 0;
		}
		if (watch_enabled == 1) {
			if (watch_addr != 0) {
				uint32 now = vm_read_memory_4(watch_addr);
				if (now != 0xFE020000) {
					fprintf(stderr, "STUB-WATCH: [%08x] changed FE020000 -> %08x\n"
					        "  culprit record #%u: type=%c block %08x -> %08x sp=%08x r24=%08x\n"
					        "  (armed at record #%u)\n",
					        watch_addr, now, jit_ring_idx, type, from_pc, to_pc,
					        rec->r1, rec->r24, watch_arm_idx);
					fflush(stderr);
					watch_addr = 0; /* disarm; next 5010bb90 re-arms */
				}
			}
			if (from_pc == 0x5010bb90) {
				watch_addr = r->gpr[1] - 0x70;
				watch_arm_idx = jit_ring_idx;
				uint32 v = vm_read_memory_4(watch_addr);
				if (v != 0xFE020000) {
					fprintf(stderr, "STUB-WATCH: ARM FAILED — [%08x] = %08x right after stub-writer block (write itself broken)\n",
					        watch_addr, v);
					fflush(stderr);
					watch_addr = 0;
				}
			}
		}
	}

	/* SS_JIT_RING_DUMP_TRIGGER=1: dump the ring shortly after the ROM's 68k
	 * (DR) emulator starts executing 68k code located in the guest STACK
	 * region (Mixed Mode switch-back stubs live there).  ROM-anchored
	 * condition: the executing block is in the DR emulator range AND the 68k
	 * PC (r24) is in the stack region.  A countdown delays the dump so the
	 * records show what the stub execution actually does.  Used to capture
	 * what a WORKING (interpreter) run executes at a stack stub, vs the
	 * zeros a broken JIT run finds there. Fires once. */
	{
		static int trigger_state = -1;     /* -1 unread, 0 off, 1 armed, 2 counting, 3 done */
		static int trigger_countdown = 0;
		if (trigger_state < 0) {
			const char *e = getenv("SS_JIT_RING_DUMP_TRIGGER");
			trigger_state = (e && *e == '1') ? 1 : 0;
		}
		if (trigger_state == 1 &&
		    from_pc >= 0x50460000 && from_pc < 0x50500000 &&
		    rec->r24 >= 0x103f0000 && rec->r24 < 0x10400000) {
			trigger_state = 2;
			trigger_countdown = 100;
			fprintf(stderr, "RING TRIGGER: DR emulator executing stack code, r24=%08x sp=%08x (dump in 100 records)\n",
			        rec->r24, rec->r1);
		}
		if (trigger_state == 2 && --trigger_countdown <= 0) {
			trigger_state = 3;
			ppc_jit_dump_trace_ring();
			fprintf(stderr, "RING TRIGGER: dump complete\n");
		}
	}

	/* SS_JIT_RING_68K_MONITOR: detect changes to guest $28 (A-line vector)
	 * and trigger ring dump when corruption is detected.
	 * Fires once, dumps ring + exits — use with SS_JIT_TRACE_RING=1.
	 *
	 * History: built to diagnose the 9.2.1-on-1.1-ROM post-splash stall.
	 * Root cause (2026-06-10): 9.2.1's Memory Manager heap block-split
	 * routine writes through a corrupt free-list backward-link (0x1F),
	 * clobbering $28's LSB (0x50015570 -> 0x50015500). All A-line traps
	 * then dispatch to Name Registry ASCII data -> illegal insn -> ROM
	 * serial debug monitor -> SCC poll forever. Confirmed NOT a JIT bug
	 * (interpreter reproduces). Guest/ROM-vintage mismatch only — does not
	 * occur on 8.6, and won't occur on 9.0.1 ROM (Machine Layer path).
	 * See SYSTEM-BOOT-GATES.md §5 for full write-up. */
	{
		static int mon_state = -1;
		static uint32 last_28 = 0;
		if (mon_state < 0) {
			const char *e = getenv("SS_JIT_RING_68K_MONITOR");
			mon_state = (e && *e == '1') ? 1 : 0;
		}
		if (mon_state >= 1) {
			uint32 cur_28 = vm_read_memory_4(0x28);
			if (cur_28 != last_28) {
				fprintf(stderr, "[$28-CHG] %08x -> %08x  from_pc=%08x r24=%08x r1=%08x\n",
				        last_28, cur_28, from_pc, rec->r24, rec->r1);
				if (last_28 == 0x50015570 && cur_28 != 0x50015570
			    && vm_read_memory_4(0x1DAC) != 0xFFFFFFFF) {
					fprintf(stderr, "[$28-CHG] CORRUPTION DETECTED — dumping ring\n");
					fprintf(stderr, "  ALL GPRs:");
					for (int gi = 0; gi < 32; gi++)
						fprintf(stderr, " r%d=%08x", gi, r->gpr[gi]);
					fprintf(stderr, "\n");
					fprintf(stderr, "  MEM[$28]=%08x  [$1DAC]=%08x [$0E00]=%08x\n",
					        cur_28, vm_read_memory_4(0x1DAC), vm_read_memory_4(0x0E00));
					ppc_jit_dump_trace_ring();
					fprintf(stderr, "[$28-CHG] dump complete — exiting\n");
					fflush(stderr);
					kill(getpid(), SIGTERM);
				}
				last_28 = cur_28;
			}
		}
	}

	/* SS_JIT_RING_DUMP_AT_PC=<hex>: dump the ring shortly after the first
	 * block with from_pc == <hex> is recorded.  A countdown of 200 records
	 * after the trigger ensures the ring contains both the anchor block and
	 * ~200 subsequent blocks (enough for branch-divergence analysis).
	 * Works identically in interpreter and JIT modes.  Fires once, then
	 * sends SIGTERM to self so the process exits cleanly. */
	{
		static int atpc_state = -1;  /* -1 unread, 0 off, 1 armed, 2 counting, 3 done */
		static uint32 atpc_target = 0;
		static int atpc_countdown = 0;
		if (atpc_state < 0) {
			const char *e = getenv("SS_JIT_RING_DUMP_AT_PC");
			atpc_state = 0;
			if (e && *e) {
				atpc_target = (uint32)strtoul(e, NULL, 16);
				if (atpc_target) atpc_state = 1;
			}
		}
		if (atpc_state == 1 && from_pc == atpc_target) {
			atpc_state = 2;
			const char *cd = getenv("SS_JIT_RING_DUMP_AT_PC_DELAY");
			atpc_countdown = (cd && *cd) ? atoi(cd) : 200;
			fprintf(stderr, "RING AT-PC TRIGGER: from_pc=%08x matched (dump in 200 records)\n", from_pc);
		}
		if (atpc_state == 2 && --atpc_countdown <= 0) {
			atpc_state = 3;
			ppc_jit_dump_trace_ring();
			/* Dump expanded ROM (0x50000000..0x50500000) to file for disassembly */
			{
				FILE *rf = fopen("/tmp/ss_rom_dump.bin", "wb");
				if (rf && ROMBaseHost) {
					fwrite(ROMBaseHost, 1, 0x500000, rf);
					fclose(rf);
					fprintf(stderr, "RING AT-PC TRIGGER: ROM dump to /tmp/ss_rom_dump.bin (5MB from ROMBase)\n");
				}
			}
			fprintf(stderr, "RING AT-PC TRIGGER: dump complete — exiting\n");
			fflush(stderr);
			kill(getpid(), SIGTERM);
		}
	}
}

/* EMUL_OP entry/return recorder — called from sheepshaver_glue.cpp's
 * execute_emul_op() in BOTH interpreter and JIT mode.  Record layout reuse:
 *   from_pc = 68k PC, to_pc = opcode = EMUL_OP number,
 *   r27 field = 68k D0, r29 field = 68k D1, r1 = sp, a[] = A0-A7.
 * Type 'E' = before EmulOp (inputs), 'R' = after (results; D0 = result code). */
extern "C" void ppc_jit_ring_record_emulop(char type, uint32 pc68k, uint32 op,
                                           uint32 d0, uint32 d1, uint32 sp, const uint32 *a_regs) {
	if (!jit_ring) return;
	jit_ring_rec *rec = &jit_ring[jit_ring_idx & (JIT_RING_SIZE - 1)];
	jit_ring_idx++;
	rec->type = type; rec->from_pc = pc68k; rec->to_pc = op; rec->opcode = op;
	rec->r1 = sp;
	rec->r24 = pc68k; rec->r27 = d0; rec->r29 = d1;
	rec->lr = 0; rec->ctr = 0; rec->cr = 0;
	for (int i = 0; i < 8; i++) rec->a[i] = a_regs[i];
	/* Driver EMUL_OPs (SONY/DISK/CDROM OPEN/PRIME/CONTROL/STATUS, ops 10-21):
	 * A0 = IOParam pointer.  Capture the request so working-vs-broken boots can
	 * be compared at the I/O-request level (A3-corruption / spurious CD-eject):
	 *   lr field  = ioBuffer   [a0+0x20]
	 *   ctr field = ioReqCount [a0+0x24]   ('R' records: ioActCount [a0+0x28])
	 *   cr field  = ioPosOffset[a0+0x2e]   ('R' records: ioResult   [a0+0x10])
	 * Only for driver ops — a0 is not a pointer for other EMUL_OPs. */
	if (op >= 10 && op <= 21) {
		uint32 a0 = a_regs[0];
		if (type == 'E') {
			rec->lr  = vm_read_memory_4(a0 + 0x20);
			rec->ctr = vm_read_memory_4(a0 + 0x24);
			rec->cr  = vm_read_memory_4(a0 + 0x2e);
		} else {
			rec->lr  = vm_read_memory_4(a0 + 0x28);
			rec->ctr = vm_read_memory_4(a0 + 0x10);
			rec->cr  = vm_read_memory_4(a0 + 0x2e);
		}
	}
}

extern "C" void ppc_jit_dump_trace_ring(void) {
	if (!jit_ring || jit_ring_idx == 0) return;
	FILE *f = fopen("/tmp/ss_jit_ring.txt", "w");
	if (!f) return;
	uint32 n = jit_ring_idx < JIT_RING_SIZE ? jit_ring_idx : JIT_RING_SIZE;
	uint32 start = jit_ring_idx - n;
	for (uint32 i = 0; i < n; i++) {
		const jit_ring_rec *rec = &jit_ring[(start + i) & (JIT_RING_SIZE - 1)];
		fprintf(f, "%c %08x %08x op=%08x sp=%08x r24=%08x r27=%08x r29=%08x lr=%08x ctr=%08x cr=%08x "
		           "a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x "
		           "d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d6=%08x d7=%08x\n",
		        rec->type, rec->from_pc, rec->to_pc, rec->opcode, rec->r1,
		        rec->r24, rec->r27, rec->r29, rec->lr, rec->ctr, rec->cr,
		        rec->a[0], rec->a[1], rec->a[2], rec->a[3],
		        rec->a[4], rec->a[5], rec->a[6], rec->a[7],
		        rec->d[0], rec->d[1], rec->d[2], rec->d[3],
		        rec->d[4], rec->d[5], rec->d[6], rec->d[7]);
	}
	fclose(f);
	fprintf(stderr, "JIT trace ring: %u records (of %u total) dumped to /tmp/ss_jit_ring.txt\n",
	        n, jit_ring_idx);
}

void powerpc_cpu::jit_interp_one(uint32 opcode, uint32 pc_val)
{
	/* The bridge owns the guest PC: set it so the handler observes the correct
	 * PC, then let the handler advance it (increment_pc or branch semantics). */
	pc() = pc_val;
	jit_ring_record(regs_ptr(), 'C', pc_val, 0, opcode);
	const instr_info_t *ii = decode(opcode);
	ii->execute(this, opcode);
}

void powerpc_cpu::jit_set_active()
{
	s_active_cpu = this;
}

extern "C" void ppc_jit_interp_one(uint32_t opcode, uint32_t pc_val)
{
	/* s_active_cpu is set by execute() and by jit_set_active() (test harness).
	 * A NULL here means the JIT was driven without registering a cpu — a bug. */
	assert(s_active_cpu != NULL);
	s_active_cpu->jit_interp_one(opcode, pc_val);
}
#endif

static int ppc_refcount = 0;

#ifdef DO_CONVENTION_CALL_STATICS
template<> bool nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_init_done = false;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_code_len = 0;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_pf_offset = 0;
#endif

void powerpc_cpu::set_register(int id, any_register const & value)
{
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		gpr(id - powerpc_registers::GPR_BASE) = value.i;
		return;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		fpr(id - powerpc_registers::FPR_BASE) = value.d;
		return;
	}
	switch (id) {
	case powerpc_registers::CR:			cr().set(value.i);		break;
	case powerpc_registers::FPSCR:		fpscr() = value.i;		break;
	case powerpc_registers::XER:		xer().set(value.i);		break;
	case powerpc_registers::LR:			lr() = value.i;			break;
	case powerpc_registers::CTR:		ctr() = value.i;		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			pc() = value.i;			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			gpr(1)= value.i;		break;
	default:							abort();				break;
	}
}

any_register powerpc_cpu::get_register(int id)
{
	any_register value;
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		value.i = gpr(id - powerpc_registers::GPR_BASE);
		return value;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		value.d = fpr(id - powerpc_registers::FPR_BASE);
		return value;
	}
	switch (id) {
	case powerpc_registers::CR:			value.i = cr().get();	break;
	case powerpc_registers::FPSCR:		value.i = fpscr();		break;
	case powerpc_registers::XER:		value.i = xer().get();	break;
	case powerpc_registers::LR:			value.i = lr();			break;
	case powerpc_registers::CTR:		value.i = ctr();		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			value.i = pc();			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			value.i = gpr(1);		break;
	default:							abort();				break;
	}
	return value;
}

#if KPX_MAX_CPUS != 1
uint32 powerpc_registers::reserve_valid = 0;
uint32 powerpc_registers::reserve_addr = 0;
uint32 powerpc_registers::reserve_data = 0;
#endif

void powerpc_cpu::init_registers()
{
	assert((((uintptr)&vr(0)) % 16) == 0);
	for (int i = 0; i < 32; i++) {
		gpr(i) = 0;
		fpr(i) = 0;
	}
	cr().set(0);
	fpscr() = 0;
	xer().set(0);
	lr() = 0;
	ctr() = 0;
	pc() = 0;
	for (int i = 0; i < 4; i++) regs().sprg[i] = 0;	// SPRG0-3 must start clean (uninit -> divergence)
	// Wave 0: zero the pre-existing uninitialized trailing supervisor fields (malloc-
	// garbage cold-state bug — the regs struct comes from raw operator new). Also
	// initialize the new sr[16]/msr fields appended last.
	regs().sdr1 = 0;
	for (int i = 0; i < 16; i++) regs().bat[i] = 0;
	regs().srr0 = 0;
	regs().srr1 = 0;
	for (int i = 0; i < 16; i++) regs().sr[i] = 0;
	regs().msr = 0xf072;	// Cold value: byte-identical to old mfmsr hardcode (0xf072).
}

void powerpc_cpu::init_flight_recorder()
{
#if PPC_FLIGHT_RECORDER
	log_ptr = 0;
	log_ptr_wrapped = false;
#endif
}

void powerpc_cpu::do_record_step(uint32 pc, uint32 opcode)
{
#if PPC_FLIGHT_RECORDER
	log[log_ptr].pc = pc;
	log[log_ptr].opcode = opcode;
#ifdef SHEEPSHAVER
	log[log_ptr].sp = gpr(1);
	log[log_ptr].r24 = gpr(24);
#endif
#if PPC_FLIGHT_RECORDER >= 2
	for (int i = 0; i < 32; i++) {
		log[log_ptr].r[i] = gpr(i);
		log[log_ptr].fr[i] = fpr(i);
	}
	log[log_ptr].lr = lr();
	log[log_ptr].ctr = ctr();
	log[log_ptr].cr = cr().get();
	log[log_ptr].xer = xer().get();
	log[log_ptr].fpscr = fpscr();
#endif
	log_ptr++;
	if (log_ptr == LOG_SIZE) {
		log_ptr = 0;
		log_ptr_wrapped = true;
	}
#endif
}

#if PPC_FLIGHT_RECORDER
void powerpc_cpu::start_log()
{
	logging = true;
	invalidate_cache();
}

void powerpc_cpu::stop_log()
{
	logging = false;
	invalidate_cache();
}

void powerpc_cpu::dump_log(const char *filename)
{
	if (filename == NULL)
		filename = "ppc.log";

	FILE *f = fopen(filename, "w");
	if (f == NULL)
		return;

	int start_ptr = 0;
	int log_size = log_ptr;
	if (log_ptr_wrapped) {
		start_ptr = log_ptr;
		log_size = LOG_SIZE;
	}

	for (int i = 0; i < log_size; i++) {
		int j = (i + start_ptr) % LOG_SIZE;
#if PPC_FLIGHT_RECORDER >= 2
		fprintf(f, " pc %08x  lr %08x ctr %08x  cr %08x xer %08x ", log[j].pc, log[j].lr, log[j].ctr, log[j].cr, log[j].xer);
		fprintf(f, " r0 %08x  r1 %08x  r2 %08x  r3 %08x ", log[j].r[0], log[j].r[1], log[j].r[2], log[j].r[3]);
		fprintf(f, " r4 %08x  r5 %08x  r6 %08x  r7 %08x ", log[j].r[4], log[j].r[5], log[j].r[6], log[j].r[7]);
		fprintf(f, " r8 %08x  r9 %08x r10 %08x r11 %08x ", log[j].r[8], log[j].r[9], log[j].r[10], log[j].r[11]);
		fprintf(f, "r12 %08x r13 %08x r14 %08x r15 %08x ", log[j].r[12], log[j].r[13], log[j].r[14], log[j].r[15]);
		fprintf(f, "r16 %08x r17 %08x r18 %08x r19 %08x ", log[j].r[16], log[j].r[17], log[j].r[18], log[j].r[19]);
		fprintf(f, "r20 %08x r21 %08x r22 %08x r23 %08x ", log[j].r[20], log[j].r[21], log[j].r[22], log[j].r[23]);
		fprintf(f, "r24 %08x r25 %08x r26 %08x r27 %08x ", log[j].r[24], log[j].r[25], log[j].r[26], log[j].r[27]);
		fprintf(f, "r28 %08x r29 %08x r30 %08x r31 %08x\n", log[j].r[28], log[j].r[29], log[j].r[30], log[j].r[31]);
		fprintf(f, "opcode %08x\n", log[j].opcode);
#else
		fprintf(f, " pc %08x opc %08x", log[j].pc, log[j].opcode);
#ifdef SHEEPSHAVER
		fprintf(f, " sp %08x r24 %08x", log[j].sp, log[j].r24);
#endif
		fprintf(f, "| ");
#if !ENABLE_MON
		fprintf(f, "\n");
#endif
#endif
#if ENABLE_MON
		disass_ppc(f, log[j].pc, log[j].opcode);
#endif
	}
	fclose(f);
}
#endif

#if ENABLE_MON
static uint32 mon_read_byte_ppc(uintptr addr)
{
	return *((uint8 *)addr);
}

static void mon_write_byte_ppc(uintptr addr, uint32 b)
{
	uint8 *m = (uint8 *)addr;
	*m = b;
}
#endif

void powerpc_cpu::initialize()
{
#ifdef SHEEPSHAVER
	printf("PowerPC CPU emulator by Gwenole Beauchesne\n");
#endif

#if PPC_PROFILE_REGS_USE
	reginfo = new register_info[32];
	for (int i = 0; i < 32; i++) {
		reginfo[i].id = i;
		reginfo[i].count = 0;
	}
#endif

	init_flight_recorder();
	init_decoder();
	init_registers();
	init_decode_cache();
	execute_depth = 0;

	// Initialize block lookup table
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.initialize();
#endif

	// Init cache range invalidate recorder
	cache_range.start = cache_range.end = 0;

	// Init syscalls handler
	execute_do_syscall = NULL;

	// Init field2mask
	for (int i = 0; i < 256; i++) {
		uint32 mask = 0;
		if (i & 0x01) mask |= 0x0000000f;
		if (i & 0x02) mask |= 0x000000f0;
		if (i & 0x04) mask |= 0x00000f00;
		if (i & 0x08) mask |= 0x0000f000;
		if (i & 0x10) mask |= 0x000f0000;
		if (i & 0x20) mask |= 0x00f00000;
		if (i & 0x40) mask |= 0x0f000000;
		if (i & 0x80) mask |= 0xf0000000;
		field2mask[i] = mask;
	}

#if ENABLE_MON
	mon_init();
	mon_read_byte = mon_read_byte_ppc;
	mon_write_byte = mon_write_byte_ppc;
#endif

#if PPC_PROFILE_COMPILE_TIME
	compile_count = 0;
	compile_time = 0;
	emul_start_time = clock();
#endif
}

#if PPC_ENABLE_JIT
void powerpc_cpu::enable_jit(uint32 cache_size)
{
	use_jit = true;
	if (cache_size)
		codegen.set_cache_size(cache_size);
	codegen.initialize();
}
#endif

// Memory allocator returning powerpc_cpu objects aligned on 16-byte boundaries
// FORMAT: [ alignment ] magic identifier, offset to malloc'ed data, powerpc_cpu data
void *powerpc_cpu::operator new(size_t size)
{
	const int ALIGN = 16;

	// Allocate enough space for powerpc_cpu data + signature + align pad
	uint8 *ptr = (uint8 *)malloc(size + ALIGN * 2);
	if (ptr == NULL)
		throw std::bad_alloc();

	// Align memory
	int ofs = 0;
	while ((((uintptr)ptr) % ALIGN) != 0)
		ofs++, ptr++;

	// Insert signature and offset
	struct aligned_block_t {
		uint32 pad[(ALIGN - 8) / 4];
		uint32 signature;
		uint32 offset;
		uint8  data[sizeof(powerpc_cpu)];
	};
	aligned_block_t *blk = (aligned_block_t *)ptr;
	blk->signature = 0x53435055;		/* 'SCPU' */
	blk->offset = ofs + (&blk->data[0] - (uint8 *)blk);
	assert((((uintptr)&blk->data) % ALIGN) == 0);
	return &blk->data[0];
}

void powerpc_cpu::operator delete(void *p)
{
	uint32 *blk = (uint32 *)p;
	assert(blk[-2] == 0x53435055);		/* 'SCPU' */
	void *ptr = (void *)(((uintptr)p) - blk[-1]);
	free(ptr);
}

#ifdef SHEEPSHAVER
powerpc_cpu::powerpc_cpu()
#if PPC_ENABLE_JIT
	: codegen(this)
#endif
#else
powerpc_cpu::powerpc_cpu(task_struct *parent_task)
	: basic_cpu(parent_task)
#if PPC_ENABLE_JIT
	, codegen(this)
#endif
#endif
{
#if PPC_ENABLE_JIT
	use_jit = false;
#endif
	spcflags().init();
	++ppc_refcount;
	initialize();
}

powerpc_cpu::~powerpc_cpu()
{
	--ppc_refcount;
#if PPC_PROFILE_COMPILE_TIME
	clock_t emul_end_time = clock();

	const char *type = NULL;
#if PPC_ENABLE_JIT
	if (use_jit)
		type = "compile";
#endif
#if PPC_DECODE_CACHE
	if (!type)
		type = "predecode";
#endif
	if (type) {
		printf("### Statistics for block %s\n", type);
		printf("Total block %s count : %d\n", type, compile_count);
		uint32 emul_time = emul_end_time - emul_start_time;
		printf("Total emulation time : %.1f sec\n",
			   double(emul_time) / double(CLOCKS_PER_SEC));
		printf("Total %s time : %.1f sec (%.1f%%)\n", type,
			   double(compile_time) / double(CLOCKS_PER_SEC),
			   100.0 * double(compile_time) / double(emul_time));
		printf("\n");
	}
#endif

#if PPC_PROFILE_GENERIC_CALLS
	if (use_jit && ppc_refcount == 0) {
		uint64 total_generic_calls_count = 0;
		for (int i = 0; i < PPC_I(MAX); i++) {
			generic_calls_ids[i] = i;
			total_generic_calls_count += generic_calls_count[i];
		}
		qsort(generic_calls_ids, PPC_I(MAX), sizeof(int), generic_calls_compare);
		printf("Rank      Count Ratio Name\n");
		for (int i = 0; i < generic_calls_top_ten; i++) {
			uint32 mnemo = generic_calls_ids[i];
			uint32 count = generic_calls_count[mnemo];
			const instr_info_t *ii = powerpc_ii_table;
			while (ii->mnemo != mnemo)
				ii++;
			printf("%03d: %10lu %2.1f%% %s\n", i, count, 100.0*double(count)/double(total_generic_calls_count), ii->name);
		}
	}
#endif

#if PPC_PROFILE_REGS_USE
	printf("\n### Statistics for register usage\n");
	uint64 tot_reg_count = 0;
	for (int i = 0; i < 32; i++)
		tot_reg_count += reginfo[i].count;
	qsort(reginfo, 32, sizeof(register_info), register_info_compare);
	uint64 cum_reg_count = 0;
	for (int i = 0; i < 32; i++) {
		cum_reg_count += reginfo[i].count;
	    printf("r%-2d : %16llu %2.1f%% [%3.1f%%]\n",
			   reginfo[i].id, reginfo[i].count,
			   100.0*double(reginfo[i].count)/double(tot_reg_count),
			   100.0*double(cum_reg_count)/double(tot_reg_count));
	}
	delete[] reginfo;
#endif

	kill_decode_cache();

#if ENABLE_MON
	mon_exit();
#endif
}

void powerpc_cpu::dump_registers()
{
	fprintf(stderr, " r0 %08x   r1 %08x   r2 %08x   r3 %08x\n", gpr(0), gpr(1), gpr(2), gpr(3));
	fprintf(stderr, " r4 %08x   r5 %08x   r6 %08x   r7 %08x\n", gpr(4), gpr(5), gpr(6), gpr(7));
	fprintf(stderr, " r8 %08x   r9 %08x  r10 %08x  r11 %08x\n", gpr(8), gpr(9), gpr(10), gpr(11));
	fprintf(stderr, "r12 %08x  r13 %08x  r14 %08x  r15 %08x\n", gpr(12), gpr(13), gpr(14), gpr(15));
	fprintf(stderr, "r16 %08x  r17 %08x  r18 %08x  r19 %08x\n", gpr(16), gpr(17), gpr(18), gpr(19));
	fprintf(stderr, "r20 %08x  r21 %08x  r22 %08x  r23 %08x\n", gpr(20), gpr(21), gpr(22), gpr(23));
	fprintf(stderr, "r24 %08x  r25 %08x  r26 %08x  r27 %08x\n", gpr(24), gpr(25), gpr(26), gpr(27));
	fprintf(stderr, "r28 %08x  r29 %08x  r30 %08x  r31 %08x\n", gpr(28), gpr(29), gpr(30), gpr(31));
	fprintf(stderr, " f0 %02.5f   f1 %02.5f   f2 %02.5f   f3 %02.5f\n", fpr(0), fpr(1), fpr(2), fpr(3));
	fprintf(stderr, " f4 %02.5f   f5 %02.5f   f6 %02.5f   f7 %02.5f\n", fpr(4), fpr(5), fpr(6), fpr(7));
	fprintf(stderr, " f8 %02.5f   f9 %02.5f  f10 %02.5f  f11 %02.5f\n", fpr(8), fpr(9), fpr(10), fpr(11));
	fprintf(stderr, "f12 %02.5f  f13 %02.5f  f14 %02.5f  f15 %02.5f\n", fpr(12), fpr(13), fpr(14), fpr(15));
	fprintf(stderr, "f16 %02.5f  f17 %02.5f  f18 %02.5f  f19 %02.5f\n", fpr(16), fpr(17), fpr(18), fpr(19));
	fprintf(stderr, "f20 %02.5f  f21 %02.5f  f22 %02.5f  f23 %02.5f\n", fpr(20), fpr(21), fpr(22), fpr(23));
	fprintf(stderr, "f24 %02.5f  f25 %02.5f  f26 %02.5f  f27 %02.5f\n", fpr(24), fpr(25), fpr(26), fpr(27));
	fprintf(stderr, "f28 %02.5f  f29 %02.5f  f30 %02.5f  f31 %02.5f\n", fpr(28), fpr(29), fpr(30), fpr(31));
	fprintf(stderr, " lr %08x  ctr %08x   cr %08x  xer %08x\n", lr(), ctr(), cr().get(), xer().get());
	fprintf(stderr, " pc %08x fpscr %08x\n", pc(), fpscr());
	fflush(stderr);
}

void powerpc_cpu::dump_instruction(uint32 opcode)
{
	fprintf(stderr, "[%08x]-> %08x\n", pc(), opcode);
}

void powerpc_cpu::fake_dump_registers(uint32)
{
	dump_registers();
}

void powerpc_registers::interrupt_copy(powerpc_registers &oregs, powerpc_registers const &iregs)
{
	for (int i = 0; i < 32; i++) {
		oregs.gpr[i] = iregs.gpr[i];
		oregs.fpr[i] = iregs.fpr[i];
	}
	oregs.cr	= iregs.cr;
	oregs.fpscr	= iregs.fpscr;
	oregs.xer	= iregs.xer;
	oregs.lr	= iregs.lr;
	oregs.ctr	= iregs.ctr;
	oregs.pc	= iregs.pc;

	uint32 vrsave = iregs.vrsave;
	oregs.vrsave  = vrsave;
	if (vrsave) {
		for (int i = 31; i >= 0; i--) {
			if (vrsave & 1)
				oregs.vr[i] = iregs.vr[i];
			vrsave >>= 1;
		}
	}
}

bool powerpc_cpu::check_spcflags()
{
	if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) {
		spcflags().clear(SPCFLAG_CPU_EXEC_RETURN);
		return false;
	}
#ifdef SHEEPSHAVER
	if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_HANDLE_INTERRUPT);
		/* M3a Task 4: real DEC exception delivery (newworld profile ONLY —
		 * the gate sits BEFORE any new side effect; paravirtual takes the legacy
		 * path below byte-identically). The hook consumes only the VirtClock DEC
		 * latch (rev 2 M3: InterruptFlags/VIA stays with HandleInterrupt until
		 * M3b's PIC). Delivered: live regs mutated in place; return true — the
		 * dispatcher re-derives the next block from pc(); never fall through to
		 * HandleInterrupt in the same call (rev 2 F3). Not pending, or deferred
		 * (EE off / execute_depth > 1, latch left SET): the hook returns false
		 * and we fall through to the legacy path exactly as before. The HANDLE
		 * flag is cleared exactly once above, common to both paths. */
		if (MachineProfileIsNewWorld() && SheepExcDeliverPending())
			return true;
		static bool processing_interrupt = false;
		if (!processing_interrupt) {
			processing_interrupt = true;
			powerpc_registers r;
			powerpc_registers::interrupt_copy(r, regs());
			HandleInterrupt(&r);
			powerpc_registers::interrupt_copy(regs(), r);
			processing_interrupt = false;
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* File only — interrupts arrive at 60Hz during VBL spinwait, too noisy for stderr */
			JIT_FLOG("interrupt delivered, pc=%08x", (uint32_t)pc());
#endif
		}
	}
	if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
	}
#endif
	if (spcflags().test(SPCFLAG_CPU_ENTER_MON)) {
		spcflags().clear(SPCFLAG_CPU_ENTER_MON);
#if ENABLE_MON
		// Start up mon in real-mode
		const char *arg[] = {
			"mon",
#ifdef SHEEPSHAVER
			"-m",
#endif
			"-r",
			NULL
		};
		mon(sizeof(arg)/sizeof(arg[0]) - 1, arg);
#endif
	}
	return true;
}

#if DYNGEN_DIRECT_BLOCK_CHAINING
void * powerpc_cpu::call_compile_chain_block(powerpc_cpu * the_cpu, block_info *sbi)
{
	return the_cpu->compile_chain_block(sbi);
}

void * PF_CONVENTION powerpc_cpu::compile_chain_block(block_info *sbi)
{
	// Block index is stuffed into the source basic block pointer,
	// which is aligned at least on 4-byte boundaries
	const int n = ((uintptr)sbi) & 3;
	sbi = (block_info *)(((uintptr)sbi) & ~3L);

	const uint32 tpc = sbi->li[n].jmp_pc;
	block_info *tbi = my_block_cache.find(tpc);
	if (tbi == NULL)
		tbi = compile_block(tpc);
	assert(tbi && tbi->pc == tpc);

	dg_set_jmp_target(sbi->li[n].jmp_addr, tbi->entry_point);
	return tbi->entry_point;
}
#endif

void powerpc_cpu::execute(uint32 entry)
{
	bool invalidated_cache = false;
	pc() = entry;
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Make this cpu reachable from the JIT inline-interpreter-call bridge
	 * (ppc_jit_interp_one).  Single-threaded; nested execute() calls share the
	 * same cpu object, so re-assigning here is harmless. */
	s_active_cpu = this;
	FILE *jit_trace_fp_for_cpu = NULL; /* set by trace block at pdi_execute, read by JIT gate */
#endif
#if PPC_EXECUTE_DUMP_STATE
	const bool dump_state = true;
#endif
	execute_depth++;
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	if (execute_depth == 1 || (PPC_ENABLE_JIT && PPC_REENTRANT_JIT)) {
#if PPC_ENABLE_JIT
		if (use_jit) {
			block_info *bi = my_block_cache.find(pc());
			if (bi == NULL)
				bi = compile_block(pc());
			for (;;) {
				// Execute all cached blocks
				for (;;) {
					codegen.execute(bi->entry_point);

					if (!spcflags().empty()) {
						if (!check_spcflags())
							goto return_site;

						// Force redecoding if cache was invalidated
						if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
							spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
							invalidated_cache = true;
							break;
						}
					}

					// Don't check for backward branches here as this
					// is now done by generated code. Besides, we will
					// get here if the fast cache lookup failed too.
					if ((bi = my_block_cache.find(pc())) == NULL)
						break;
				}

				// Compile new block
				bi = compile_block(pc());
			}
		}
#endif
#if PPC_DECODE_CACHE
		block_info *bi = my_block_cache.find(pc());
		if (bi != NULL)
			goto pdi_execute;
		for (;;) {
#if PPC_PROFILE_COMPILE_TIME
			compile_count++;
			clock_t start_time;
			start_time = clock();
#endif
			bi = my_block_cache.new_blockinfo();
			bi->init(pc());

			// Predecode a new block
			block_info::decode_info *di;
			const instr_info_t *ii;
			uint32 dpc;
			di = bi->di = decode_cache_p;
			dpc = pc() - 4;
			do {
				uint32 opcode = vm_read_memory_4(dpc += 4);
				ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::dump_instruction);
					di++;
				}
#endif
#if PPC_FLIGHT_RECORDER
				if (is_logging()) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::record_step);
					di++;
				}
#endif
				di->opcode = opcode;
				di->execute = ii->execute;
				di++;
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = 0;
					di->execute = nv_mem_fun(&powerpc_cpu::fake_dump_registers);
					di++;
				}
#endif
				if (di >= decode_cache_end_p) {
					// Invalidate cache and move current code to start
					invalidate_cache();
					const int blocklen = di - bi->di;
					memmove(decode_cache_p, bi->di, blocklen * sizeof(*di));
					bi->di = decode_cache_p;
					di = bi->di + blocklen;
				}
			} while ((ii->cflow & CFLOW_END_BLOCK) == 0);
			bi->end_pc = dpc;
			bi->min_pc = dpc;
			bi->max_pc = entry;
			bi->size = di - bi->di;
			my_block_cache.add_to_cl_list(bi);
			my_block_cache.add_to_active_list(bi);
			decode_cache_p += bi->size;
#if PPC_PROFILE_COMPILE_TIME
			compile_time += (clock() - start_time);
#endif

			// Execute all cached blocks
		  pdi_execute:
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* PC trace: log block-start PCs for differential JIT vs interpreter debugging.
			 * SS_JIT_TRACE=/path → each line is: "I <pc>" (interpreter block entry)
			 * or "J <from_pc> <to_pc> <r1> <r3>" (JIT block execution, logged post-run). */
			{
				static FILE *jit_trace_fp = (FILE *)(uintptr_t)1;
				if (jit_trace_fp == (FILE *)(uintptr_t)1) {
					const char *path = getenv("SS_JIT_TRACE");
					jit_trace_fp = path ? fopen(path, "w") : NULL;
					jit_ring_init_once(); /* SS_JIT_TRACE_RING=1: in-memory ring */
				}
				if (jit_trace_fp) { fprintf(jit_trace_fp, "I %08x\n", pc()); fflush(jit_trace_fp); }
				jit_trace_fp_for_cpu = jit_trace_fp; /* share with JIT gate below */
				jit_ring_record(regs_ptr(), 'I', pc(), 0, 0);
			}
#endif
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* AArch64 direct-codegen JIT: try to execute block natively.
			 *
			 * Gates in this block — see SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md:
			 *
			 * GATE 1 (SS_USE_JIT=0): OVERRIDE — JIT enabled by default; set SS_USE_JIT=0
			 *   to force interpreter execution for diagnostics or regressions.
			 *
			 * GATE 2 (jblk.complete): CONTAINMENT — only execute fully compiled blocks.
			 *   Status: overcautious; partial blocks are safe (truncation epilogue writes
			 *   valid PPCR_PC and interpreter can resume from there). Candidate for removal.
			 *   Expiry: remove when parity harness confirms partial-block execution is correct.
			 *
			 * GATE 3 (PC range check): DIAGNOSTIC — detects JIT compiler bugs that produce
			 *   out-of-range PCs.  Should log before skipping, not silently continue.
			 */
			{
				static bool jit_init_done = false;
				static const char *jit_env = getenv("SS_USE_JIT");
				static bool jit_enabled = !(jit_env && jit_env[0] == '0' && jit_env[1] == '\0');
				if (!jit_enabled) goto skip_jit; /* GATE 1: SS_USE_JIT=0 diagnostic override */
				if (__builtin_expect(!jit_profile_checked, false)) {
					jit_profile_checked = true;
					const char *e = getenv("SS_JIT_PROFILE");
					jit_profile_enabled = (e && e[0] && e[0] != '0');
					if (jit_profile_enabled)
						fprintf(stderr, "[PROFILE] B1 execution profiler ENABLED — per-block hit counts active\n");
				}
				if (!jit_init_done) {
					/* Code cache size: default 256 MB.  MAP_JIT memory is virtual
					 * — no physical cost until touched.  Override via
					 * jitcachesize pref (in KB) or SS_JIT_CACHE_KB env var.
					 * 64 MB caused 2+ full flushes per boot; 256 MB eliminates
					 * most flush churn during normal use. */
					{
						uint32 cache_kb = 262144; /* 256 MB default */
						const char *env = getenv("SS_JIT_CACHE_KB");
						if (env && *env) cache_kb = strtoul(env, NULL, 0);
						ppc_jit_aarch64_init(cache_kb);
					}
					/* Register the Mac ROM as a JIT-compilable range.  ROM is
					 * write-protected after patching (main_unix.cpp), so compiled
					 * ROM blocks are permanently valid.  The range stops at
					 * +0x460000 to EXCLUDE the ROM's built-in 68k (DR) emulator.
					 * Session 7 proved entry-poll suppression alone is insufficient:
					 * the C dispatcher's between-block spcflags check also fires
					 * mid-dispatch-cycle (between DR dispatch and toolbox handler),
					 * setting CR2.LT before bclr 5,8 can evaluate it. Fully deferring
					 * spcflags starves interrupts (jNK goes flat). The fix needs
					 * per-instruction spcflags delivery that matches interpreter
					 * timing — see session 7 LEARNINGS entry and HANDOFF.
					 * SS_JIT_NO_ROM=1: bisect switch — keep ROM interpreter-only. */
					{
						const char *no_rom = getenv("SS_JIT_NO_ROM");
						if (!(no_rom && *no_rom == '1')) {
							/* SS_JIT_ROM_SIZE: override ROM JIT range (hex).
							 * Binary-search between 0x460000 (safe) and 0x500000
							 * (broken) to locate DR emulator hang region. */
							uint32_t rom_jit_size = 0x500000;
							const char *rom_size_env = getenv("SS_JIT_ROM_SIZE");
							if (rom_size_env) {
								rom_jit_size = (uint32_t)strtoul(rom_size_env, NULL, 16);
								if (rom_jit_size < 0x100000) rom_jit_size = 0x100000;
								if (rom_jit_size > 0x500000) rom_jit_size = 0x500000;
							}
							fprintf(stderr, "[JIT] ROM JIT range: [%08x..%08x] (size=0x%x%s)\n",
								ROMBase, ROMBase + rom_jit_size, rom_jit_size,
								rom_size_env ? ", SS_JIT_ROM_SIZE override" : "");
							ppc_jit_aarch64_set_rom_range(ROMBase, rom_jit_size, ROMBaseHost);
						}
					}
					jit_init_done = true;
					JIT_LOG("JIT initialized");
				}
				ppc_jit_block jblk;
				/* GATE 2: execute only complete native blocks. Incomplete blocks are
				 * compile-time probes only; skip_jit lets the interpreter execute the
				 * first uncompiled/fallback-only instruction at the original PC.
				 *
				 * Two-tier dispatch: ppc_jit_aarch64_lookup_fast() is a hash-lookup-only
				 * fast path (tiny stack frame, no compile machinery) covering the
				 * overwhelmingly common already-compiled case; the full compile() runs
				 * only on lookup miss. */
				uint32 jit_block_start_pc = pc(); /* block entry PC, for trace + GATE3 */
				ppc_jit_entry_fn fn = ppc_jit_aarch64_lookup_fast(pc());
				if (!fn && ppc_jit_aarch64_compile(pc(), RAMBaseHost, RAMSize, &jblk) && jblk.complete)
					fn = (ppc_jit_entry_fn)(void*)jblk.code;

				/* SS_JIT_VERIFY: differential state comparison — run every JIT block
				 * through the interpreter and compare results.  EXTREMELY SLOW.
				 * Gate: SS_JIT_VERIFY=1 env var; zero overhead when disabled. */
				static int jit_verify_enabled = -1;
				if (jit_verify_enabled < 0) {
					const char *env = getenv("SS_JIT_VERIFY");
					jit_verify_enabled = (env && *env == '1') ? 1 : 0;
					if (jit_verify_enabled)
						fprintf(stderr, "[VERIFY] JIT verification mode ENABLED — every block runs twice\n");
						if (!getenv("SS_JIT_NO_CHAIN") || getenv("SS_JIT_NO_CHAIN")[0] != '1')
							fprintf(stderr, "[VERIFY] WARNING: SS_JIT_NO_CHAIN=1 is not set; chained blocks "
							        "will report as ARTIFACT-PC (jit_state is end-of-chain). Set it for clean results.\n");
				}
				powerpc_registers jit_verify_pre_state;
				int jit_verify_n_insns = 0;

				if (fn) {
					/* ---- Dispatch call-chain ring (SS_JIT_CHAIN_LOG=1) ----
					 * Records last 8 block-entry PCs into a ring buffer.
					 * When the target address is hit, dumps the ring to stderr.
					 * TEMPORARY diagnostic — remove after boot-hang investigation. */
					{
						static bool chain_log_enabled = false;
						static bool chain_log_checked = false;
						static uint32 dispatch_ring[8];
						static int dispatch_ring_idx = 0;
						if (__builtin_expect(!chain_log_checked, false)) {
							chain_log_checked = true;
							chain_log_enabled = (getenv("SS_JIT_CHAIN_LOG") && atoi(getenv("SS_JIT_CHAIN_LOG")));
						}
						if (__builtin_expect(chain_log_enabled, false)) {
							dispatch_ring[dispatch_ring_idx & 7] = jit_block_start_pc;
							dispatch_ring_idx++;
						}
						if (__builtin_expect(jit_verify_enabled, false)) {
								memcpy(&jit_verify_pre_state, regs_ptr(), sizeof(powerpc_registers));
								jit_verify_n_insns = ppc_jit_aarch64_lookup_n_insns(jit_block_start_pc);
								if (jit_verify_n_insns == 0)
									jit_verify_n_insns = jblk.n_insns; /* freshly compiled */
							}
						// SS_SEED_MEM (PC-triggered): apply seeds at this block entry.
						// Inline-guarded like SS_PROBE_PC so it is ~0 cost on the hot
						// dispatch path when no seeds are set (-1 -> parse once -> 0 forever).
						if (__builtin_expect(s_seed_count != 0, false))
							ss_seed_mem_check_pc((uint32_t)jit_block_start_pc);
						// SS_PROBE_PC: dump registers/memory at specified block-entry PCs.
						// Parsed once; up to PROBE_MAX compares per block when active.
						{
							if (__builtin_expect(s_probe_count < 0, false)) {
								s_probe_count = parse_probes(getenv("SS_PROBE_PC"));
								if (s_probe_count > 0)
									fprintf(stderr, "[PROBE] Parsed %d probe(s) from SS_PROBE_PC\n", s_probe_count);
							}
							if (__builtin_expect(s_probe_count > 0, false)) {
								uint32_t bpc = (uint32_t)jit_block_start_pc;
								for (int pi = 0; pi < s_probe_count; pi++) {
									if (s_probes[pi].pc == bpc) {
										s_probes[pi].visits++;
										if (probe_should_log(s_probes[pi].visits)) {
											probe_entry *pe = &s_probes[pi];
											fprintf(stderr, "[PROBE 0x%08x visit=%llu]",
											        bpc, (unsigned long long)pe->visits);
											if (pe->dump_all) {
												fprintf(stderr, "\n");
												for (int ri = 0; ri < 32; ri++) {
													fprintf(stderr, "  r%-2d = 0x%08x", ri, (uint32_t)gpr(ri));
													if ((ri & 3) == 3) fprintf(stderr, "\n");
												}
												fprintf(stderr, "  CR  = 0x%08x  LR  = 0x%08x  CTR = 0x%08x\n",
												        cr().get(), (uint32_t)lr(), (uint32_t)ctr());
											} else {
												for (int fi = 0; fi < pe->n_fields; fi++) {
													if (pe->fields[fi].type == PROBE_GPR) {
														fprintf(stderr, " r%d=0x%08x",
														        pe->fields[fi].value,
														        (uint32_t)gpr(pe->fields[fi].value));
													} else if (pe->fields[fi].type == PROBE_MEM) {
														// M1: refuse side-effecting device reads (§2b).
														if (vm_is_mmio(pe->fields[fi].value)) {
															fprintf(stderr, " [0x%08x]=<MMIO-refused>", pe->fields[fi].value);
														} else {
															// No bounds check: unmapped address will SIGSEGV (developer tool)
															uint32_t val = vm_read_memory_4(pe->fields[fi].value);
															fprintf(stderr, " [0x%08x]=0x%08x",
															        pe->fields[fi].value, val);
														}
													} else {
														// PROBE_MEM_REG: [rN:SIZE] — dump SIZE bytes from address in gpr(N)
														uint32_t base_addr = (uint32_t)gpr(pe->fields[fi].value);
														uint32_t sz = pe->fields[fi].size;
														fprintf(stderr, "\n  mem[r%u=0x%08x +0x0000..+0x%04x]:",
														        pe->fields[fi].value, base_addr, sz);
														uint32_t nwords = (sz + 3) / 4;
														for (uint32_t wi = 0; wi < nwords; wi++) {
															if ((wi & 7) == 0)
																fprintf(stderr, "\n    +0x%04x:", wi * 4);
															if (vm_is_mmio(base_addr + wi * 4)) { fprintf(stderr, " <refused>"); continue; }
															uint32_t val = vm_read_memory_4(base_addr + wi * 4);
															fprintf(stderr, " %08x", val);
														}
														fprintf(stderr, "\n");
													}
												}
												fprintf(stderr, "\n");
											}
											fflush(stderr);
										}
									}
								}
							}
						}
						// B1 profiler: count block executions + timing (guarded, ~0 cost when off)
						if (__builtin_expect(jit_profile_enabled, false)) {
							jit_profile_counts[jit_block_start_pc]++;
							jit_profile_total++;
							uint64_t t0 = mach_absolute_time();
							fn((void*)regs_ptr());
							jit_profile_time[jit_block_start_pc] += mach_absolute_time() - t0;
						} else {
						fn((void*)regs_ptr());
						}
						if (__builtin_expect(chain_log_enabled && jit_block_start_pc == 0x50132ec8, false)) {
							static int chain_log_budget = 20;
							if (chain_log_budget > 0) {
								chain_log_budget--;
								uint32 exit_pc = pc();
								fprintf(stderr, "[CHAIN] exit=%08x ring:", exit_pc);
								for (int i = 0; i < 8; i++)
									fprintf(stderr, " %08x", dispatch_ring[(dispatch_ring_idx - 8 + i) & 7]);
								fprintf(stderr, "\n");
							}
						}
					}
				  pdi_jit_post:
					/* ---- SS_JIT_VERIFY: compare JIT output against interpreter ---- */
					if (__builtin_expect(jit_verify_enabled && jit_verify_n_insns > 0, false)) {
						/* Report budget: once it hits 0 the oracle stops checking EVERY
						 * subsequent block for the rest of the boot (entry-gated below), so a
						 * low budget makes verify go dark early — covering only the front of
						 * boot, not the eviction-heavy later workload. Default 20 (quiet);
						 * override with SS_JIT_VERIFY_BUDGET=N for whole-boot coverage. */
						static int verify_divergence_budget = -1;
						if (verify_divergence_budget < 0) {
							const char *b = getenv("SS_JIT_VERIFY_BUDGET");
							verify_divergence_budget = (b && atoi(b) > 0) ? atoi(b) : 20;
							fprintf(stderr, "[VERIFY] divergence report budget = %d%s\n",
							        verify_divergence_budget, (b && atoi(b) > 0) ? " (SS_JIT_VERIFY_BUDGET)" : " (default)");
						}
						/* SS_JIT_VERIFY_PC=LO:HI (hex): targeted-PC verify — restrict the
						 * oracle to blocks whose start PC is in [LO,HI). Lets you re-check one
						 * suspect range without the whole-boot interp-replay slowdown (the
						 * budget/timing trap, OPTIMIZATION-PLAN 0b-extra4). Unset = all in-range. */
						static uint32 verify_pc_lo = 0, verify_pc_hi = 0;
						static int verify_pc_checked = 0;
						if (!verify_pc_checked) {
							verify_pc_checked = 1;
							const char *p = getenv("SS_JIT_VERIFY_PC");
							unsigned lo = 0, hi = 0;
							if (p && *p && sscanf(p, "%x:%x", &lo, &hi) == 2 && hi > lo) {
								verify_pc_lo = lo; verify_pc_hi = hi;
								fprintf(stderr, "[VERIFY] PC scope = [%08x,%08x) (SS_JIT_VERIFY_PC)\n", lo, hi);
							}
						}
						/* After a divergence, skip the next few in-range blocks before
						 * re-checking so a single bug doesn't cascade into a wall of false
						 * divergences. Counts DOWN to 0 (was a latching bool that, once set,
						 * gated its own clearing branch and silenced verify for the whole run). */
						static int verify_suppress_blocks = 0;
						/* Skip blocks that end with bl/bctrl to the Mixed Mode dispatch
						 * area (0x10100000-0x10110000) or any callee — these produce
						 * cascading false divergences because the interpreter follows
						 * the call through a different dispatch path.  Also skip if a
						 * prior divergence poisoned the register state (suppress until
						 * we see a clean block). */
						bool has_link_call = false;
						{
							uint32 last_op = vm_read_memory_4(jit_block_start_pc + (jit_verify_n_insns - 1) * 4);
							uint32 pri = last_op >> 26;
							bool lk = last_op & 1;
							if (lk && (pri == 18 || pri == 16 ||
							           (pri == 19 && (((last_op >> 1) & 0x3FF) == 16 ||
							                         ((last_op >> 1) & 0x3FF) == 528))))
								has_link_call = true;
						}
						if (verify_divergence_budget > 0 && !has_link_call && verify_suppress_blocks == 0 &&
						    jit_block_start_pc >= 0x10000000 && jit_block_start_pc < 0x20000000 &&
						    (verify_pc_hi == 0 || (jit_block_start_pc >= verify_pc_lo && jit_block_start_pc < verify_pc_hi))) {
							/* Save post-JIT state */
							powerpc_registers jit_state;
							memcpy(&jit_state, regs_ptr(), sizeof(powerpc_registers));

							/* No-op skip (X1): if fn() changed nothing (e.g. it bailed at the entry
							 * spcflags poll, storing block_start to PC and returning), there is nothing
							 * to verify and re-running the interpreter would falsely 'execute' the block.
							 * memcmp covers all of powerpc_registers (incl. CR/XER/FPSCR/PC); both are
							 * memcpy copies of the same struct so padding matches. A legit loop-to-start
							 * is NOT skipped - its iteration changed registers. */
							bool jit_changed = (memcmp(&jit_state, &jit_verify_pre_state, sizeof(powerpc_registers)) != 0);
							if (jit_changed) {

							/* Restore pre-block state and re-run the interpreter, mirroring the JIT
							 * block's single-block control flow (X1 fix i). A JIT block runs LINEARLY
							 * until the first conditional branch (bc, opcode 16 - BOTH arms epilogue to
							 * the dispatcher, taken or not) or an unconditional terminator/taken branch.
							 * Execute each insn, then stop when PC left the sequential path (pc != cur+4:
							 * a taken branch/terminator) OR the insn was a bc (its not-taken arm leaves
							 * pc=cur+4 yet still ends the block; dead code the compiler emits past it
							 * inflates jit_verify_n_insns, so the cap alone won't stop here). The OLD
							 * 'stop when pc leaves [start,end)' rule re-iterated loops, followed blr
							 * returns, and ran into post-bc dead code - false-positive classes 2/4/5
							 * (OPTIMIZATION-PLAN 0b-extra4). Requires SS_JIT_NO_CHAIN=1 (else jit_state is
							 * end-of-chain). Cleans the control-structural classes; memory-dependent
							 * blocks correctly stay SUSPECT until fix (ii). */
							memcpy(regs_ptr(), &jit_verify_pre_state, sizeof(powerpc_registers));
							for (int vi = 0; vi < jit_verify_n_insns; vi++) {
								uint32 cur_pc_v = pc();
								uint32 opcode = vm_read_memory_4(cur_pc_v);
								const instr_info_t *ii = decode(opcode);
								ii->execute(this, opcode);
								if (pc() != cur_pc_v + 4 || (opcode >> 26) == 16)
									break; /* JIT block exited here (taken branch/terminator, or bc either arm) */
							}

							/* Compare post-interpreter state against post-JIT state */
							bool match = true;
							for (int i = 0; i < 32; i++) {
								if (gpr(i) != jit_state.gpr[i]) {
									fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: GPR%d interp=0x%08x jit=0x%08x\n",
									        jit_block_start_pc, i, gpr(i), jit_state.gpr[i]);
									match = false;
								}
							}
							if (cr().get() != jit_state.cr.get()) {
								fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: CR interp=0x%08x jit=0x%08x\n",
								        jit_block_start_pc, cr().get(), jit_state.cr.get());
								match = false;
							}
							if (lr() != jit_state.lr) {
								fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: LR interp=0x%08x jit=0x%08x\n",
								        jit_block_start_pc, (uint32)lr(), (uint32)jit_state.lr);
								match = false;
							}
							if (ctr() != jit_state.ctr) {
								fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: CTR interp=0x%08x jit=0x%08x\n",
								        jit_block_start_pc, (uint32)ctr(), (uint32)jit_state.ctr);
								match = false;
							}
							if (xer().get_ca() != jit_state.xer.get_ca() ||
							    xer().get_ov() != jit_state.xer.get_ov() ||
							    xer().get_so() != jit_state.xer.get_so()) {
								fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: XER interp=so%d,ov%d,ca%d jit=so%d,ov%d,ca%d\n",
								        jit_block_start_pc,
								        xer().get_so(), xer().get_ov(), xer().get_ca(),
								        jit_state.xer.get_so(), jit_state.xer.get_ov(), jit_state.xer.get_ca());
								match = false;
							}
							if (pc() != jit_state.pc) {
								fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: PC interp=0x%08x jit=0x%08x\n",
								        jit_block_start_pc, (uint32)pc(), (uint32)jit_state.pc);
								match = false;
							}
							/* FPR + VR: the boot-time oracle now also catches floating-point and
							 * AltiVec divergence (e.g. the ev_mixed bugs in real software), not just
							 * GPR/CR/flags. Bit-exact compare; jit_state already snapshots the full
							 * register file. (Mirrors the harness REGDUMP FPR/VR addition.) */
							for (int i = 0; i < 32; i++) {
								uint64 ib; double id = fpr(i); memcpy(&ib, &id, sizeof(ib));
								if (ib != jit_state.fpr[i].j) {
									fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: FPR%d interp=%016llx jit=%016llx\n",
									        jit_block_start_pc, i, (unsigned long long)ib, (unsigned long long)jit_state.fpr[i].j);
									match = false;
								}
							}
							for (int i = 0; i < 32; i++) {
								if (vr(i).j[0] != jit_state.vr[i].j[0] || vr(i).j[1] != jit_state.vr[i].j[1]) {
									fprintf(stderr, "[VERIFY] DIVERGENCE block %08x: VR%d interp=%016llx%016llx jit=%016llx%016llx\n",
									        jit_block_start_pc, i,
									        (unsigned long long)vr(i).j[1], (unsigned long long)vr(i).j[0],
									        (unsigned long long)jit_state.vr[i].j[1], (unsigned long long)jit_state.vr[i].j[0]);
									match = false;
								}
							}

							if (!match) {
								/* Per-block report dedup (X1): a block that diverges on EVERY visit (e.g. a
								 * memory-RMW counter like 100fd0e0) would otherwise consume the whole report
								 * budget and blind the oracle to the rest of boot. Report each distinct block
								 * PC once, then suppress further reports so the budget covers many distinct
								 * blocks. (Per-process; reset only by restart.) */
								static uint32 reported_pcs[1024];
								static int reported_n = 0;
								bool already_reported = false;
								for (int k = 0; k < reported_n; k++)
									if (reported_pcs[k] == jit_block_start_pc) { already_reported = true; break; }
								if (!already_reported) {
									if (reported_n < 1024) reported_pcs[reported_n++] = jit_block_start_pc;

								/* Classify the record so the log is triagable instead of a wall
								 * of noise (OPTIMIZATION-PLAN 0b-extra4). If interp and JIT agree
								 * on the exit PC, they took the SAME control flow, so a register
								 * divergence is a real-bug CANDIDATE -> SUSPECT. If the exit PC
								 * differs, they took different paths (chaining / blr-return /
								 * intra-block loop / conditional arm) and the whole record is a
								 * structural ARTIFACT of the differential oracle, not a codegen
								 * bug. CAVEAT: a memory read-modify-write block (load X; op; store
								 * X) also lands in SUSPECT, because the replay restores registers
								 * but not guest memory — disambiguate by the value stepping by 2
								 * across visits (JIT store + replay store), not 1. Triage:
								 * `grep '\[VERIFY\] SUSPECT'`. */
								bool pc_match = (pc() == jit_state.pc);
								fprintf(stderr, "[VERIFY] %s block %08x (exit PC %s)\n",
								        pc_match ? "SUSPECT" : "ARTIFACT-PC", jit_block_start_pc,
								        pc_match ? "matches: real-bug candidate (memory-RMW also lands here — check value steps by 2, not 1)"
								                 : "differs: structural artifact, not a codegen bug");
								verify_divergence_budget--;
								/* TUNABLE: cascade-suppression window (in-range blocks skipped
								 * after a divergence). 2 is a guess — line 1387 restores full
								 * jit_state and the next block recaptures pre_state from it, so
								 * interp and JIT start each block identically and cross-block
								 * poisoning shouldn't actually occur; this is belt-and-suspenders.
								 * If it ever needs adjustment, the symptoms are:
								 *   too LOW  -> [VERIFY] DIVERGENCE lines arrive in tight bursts
								 *               (a real bug echoing across consecutive blocks),
								 *               and verify_divergence_budget(20) drains fast.
								 *   too HIGH -> genuine, independent divergences get swallowed;
								 *               you see one report then suspicious silence even
								 *               though the JIT is still wrong downstream. */
								verify_suppress_blocks = 2;
								/* Dump the block's opcodes */
								fprintf(stderr, "[VERIFY] Block %08x (%d insns):", jit_block_start_pc, jit_verify_n_insns);
								for (int vi = 0; vi < jit_verify_n_insns; vi++) {
									uint32 op = vm_read_memory_4(jit_block_start_pc + vi * 4);
									fprintf(stderr, " %08x", op);
								}
								fprintf(stderr, "\n");
								if (verify_divergence_budget == 0)
									fprintf(stderr, "[VERIFY] Budget exhausted — further divergences suppressed\n");
							}

								}
							}
							/* Restore JIT state so execution continues correctly */
							memcpy(regs_ptr(), &jit_state, sizeof(powerpc_registers));
						} else if (verify_suppress_blocks > 0 &&
						           jit_block_start_pc >= 0x10000000 && jit_block_start_pc < 0x20000000) {
							/* Counting down a post-divergence cascade window. Only in-range
							 * blocks count — execution is overwhelmingly ROM (0x50xxxxxx), so
							 * decrementing on every block would drain this before any RAM block
							 * is skipped, making suppression a no-op. */
							verify_suppress_blocks--;
						}
					}
					/* Time-based heartbeat — file only, no stderr spam */
					{
						static uint64_t jit_block_count = 0;
						static double last_t = 0;
						static uint32_t last_pc = 0;
						static int stuck_count = 0;
						jit_block_count++;
						/* SS_LOG_FIRST_BLOCKS=N: dump the first N block-entry PCs to stderr — the
						 * boot path. Only ~27 blocks run before the parcels nanokernel deadlock, so a
						 * few hundred capture the whole init->fault->spinlock sequence. Off by default. */
						{
							static int fb = -1;
							if (fb < 0) { const char *e = getenv("SS_LOG_FIRST_BLOCKS"); fb = e ? atoi(e) : 0; }
							if (fb > 0 && jit_block_count <= (uint64_t)fb) {
								fprintf(stderr, "[FB %llu] pc=%08x\n",
								        (unsigned long long)jit_block_count, (uint32_t)jit_block_start_pc);
								if (jit_block_count == (uint64_t)fb) fflush(stderr);
							}
							/* Ad-hoc diagnostic probes replaced by SS_PROBE_PC env var
							 * (parsed at JIT init, checked pre-dispatch). See CLAUDE.md. */
						}
						/* Region profiling: classify by block ENTRY pc (jit_block_start_pc),
						 * since the exit pc may be in a different region. */
						rgn_jit_blocks[rgn_classify(jit_block_start_pc)]++;
						/* Heartbeat: check the clock only every 4096 blocks — clock_gettime
						 * per block (~25M/s) would itself cost ~0.5s/s of wall time. */
						if ((jit_block_count & 0xFFF) == 0) {
						double now = jit_elapsed_s();
						if (now - last_t >= 5.0) {
							jit_diag_log_open();
							uint32_t cur_pc = (uint32_t)pc();
							static uint64 prev_block_count = 0;
							uint64 delta = jit_block_count - prev_block_count;
							double dt = now - last_t;
							double rate = (dt > 0) ? delta / dt / 1e6 : 0;
							prev_block_count = jit_block_count;
							uint32_t compiled = ppc_jit_aarch64_blocks_compiled();
							/* SS_JIT_RING_DUMP_ON_STALL=<n>: dump the trace ring once when the
							 * compiled-block count stops increasing for n consecutive heartbeats
							 * while execution is still fast (>1M blocks/s). This captures tight
							 * hang loops where the same already-compiled blocks repeat forever
							 * (e.g. extension-loading hang), without relying on stable PCs.
							 * Requires SS_JIT_TRACE_RING=1 to have a ring to dump. */
							{
								static int stall_n = -1;
								static uint32_t stall_last_compiled = 0;
								static int stall_count = 0;
								if (stall_n < 0) {
									const char *e = getenv("SS_JIT_RING_DUMP_ON_STALL");
									stall_n = (e && *e) ? atoi(e) : 0;
								}
								if (stall_n > 0) {
									if (compiled == stall_last_compiled && rate > 1.0) {
										stall_count++;
										if (stall_count >= stall_n) {
											fprintf(stderr, "[JIT %.1fs] STALL: comp=%u unchanged for %d heartbeats at %.0fM/s — dumping trace ring\n",
											        now, compiled, stall_count, rate);
											ppc_jit_dump_trace_ring();
											/* SS_JIT_MEMDUMP=1: dump guest RAM for diff against interpreter */
											{
												const char *md_env = getenv("SS_JIT_MEMDUMP");
												if (md_env && *md_env == '1') {
													const char *path = "/tmp/ss_memdump.bin";
													FILE *mf = fopen(path, "wb");
													if (mf) {
														fwrite(RAMBaseHost, 1, RAMSize < 0x1000000 ? RAMSize : 0x1000000, mf);
														fclose(mf);
														fprintf(stderr, "[JIT] Memory dump: %s (%u bytes from guest 0x00000000)\n",
														        path, RAMSize < 0x1000000 ? RAMSize : 0x1000000);
													}
												}
											}
											stall_n = 0; /* one-shot */
										}
									} else {
										stall_count = 0;
										stall_last_compiled = compiled;
									}
								}
							}
							/* SS_JIT_MEMDUMP_AT=<seconds>: dump guest RAM at a fixed time.
							 * Use same time for both JIT and interpreter runs to compare. */
							{
								static int memdump_at_done = 0;
								static double memdump_at_t = 0;
								if (!memdump_at_done) {
									if (memdump_at_t == 0) {
										const char *e = getenv("SS_JIT_MEMDUMP_AT");
										memdump_at_t = e ? atof(e) : -1;
									}
									if (memdump_at_t > 0 && now >= memdump_at_t) {
										memdump_at_done = 1;
										const char *path = getenv("SS_JIT_MEMDUMP_PATH");
										if (!path) path = "/tmp/ss_memdump.bin";
										FILE *mf = fopen(path, "wb");
										if (mf) {
											uint32 dump_size = RAMSize < 0x1000000 ? RAMSize : 0x1000000;
											fwrite(RAMBaseHost, 1, dump_size, mf);
											fclose(mf);
											fprintf(stderr, "[JIT %.1fs] Memory dump: %s (%u bytes)\n",
											        now, path, dump_size);
										}
									}
								}
							}
							fprintf(jit_log_file, "[JIT %.1fs] blocks=%llu pc=%08x %.0fM/s comp=%u | jNK=%llu jDR=%llu jRAM=%llu | iDR=%llu | j2i=%llu\n",
							        now, (unsigned long long)jit_block_count, cur_pc, rate, compiled,
							        (unsigned long long)rgn_jit_blocks[RGN_NK], (unsigned long long)rgn_jit_blocks[RGN_DR],
							        (unsigned long long)rgn_jit_blocks[RGN_RAM],
							        (unsigned long long)rgn_interp_blocks[RGN_DR],
							        (unsigned long long)rgn_jit_to_interp);
							fflush(jit_log_file);
							/* Terminal heartbeat (stderr + log): 10s cadence for the
							 * first minute, then every 60s.  See jit-heartbeat.hpp. */
							{
								static hb_state hb;
								/* M3a Task 4: [EXC] DEC-delivery counters ride the heartbeat
								 * (rev 2 M5: SIGALRM boot-killers skip atexit dumps). Newworld
								 * only — paravirtual heartbeat lines stay byte-identical.
								 * M6a Wave 1: + MMIO region read counts (memo §5.5a). */
								char excbuf[160]; excbuf[0] = 0;
								if (MachineProfileIsNewWorld()) {
									uint64_t exc[3];
									SheepExcStats(exc);
									snprintf(excbuf, sizeof excbuf, " | exc=%llu/%llu/%llu",
									         (unsigned long long)exc[0], (unsigned long long)exc[1],
									         (unsigned long long)exc[2]);
									hb_append_mmio_suffix(excbuf, sizeof excbuf);
								}
								hb_tick(&hb, jit_log_file, true, now, jit_block_count,
								        compiled, rgn_jit_blocks, rgn_jit_to_interp,
								        excbuf[0] ? excbuf : NULL);
							}
							/* Boot-stall probe: until the guest reaches Process-Manager idle,
							 * read the front modal screen and log a [STALL] line each heartbeat.
							 * Surfaces EARLY-boot modal alerts (e.g. the model-rejection screen)
							 * that never reach the idle hook and so emit nothing. Auto-silences
							 * post-idle. Defined in emul_op.cpp. */
							ss_boot_stall_check(now, compiled, rate);
							/* NOTE: "same PC at consecutive heartbeats" is a SAMPLING HINT, not
							 * proof of a hang — hot dispatch PCs (e.g. the nanokernel exception
							 * dispatcher at 0x50313d34) recur by chance.  Do not treat
							 * repeated sampling as proof of a hang without corroboration. */
							if (cur_pc == last_pc) {
								stuck_count++;
								if (stuck_count >= 2) {
									fprintf(stderr, "[JIT %.1fs] HOT-PC pc=%08x sampled %d consecutive heartbeats (may be sampling artifact)\n",
									        now, cur_pc, stuck_count + 1);
									fprintf(stderr, "  r1=%08x r9=%08x r10=%08x r11=%08x r12=%08x cr=%08x\n",
									        gpr(1), gpr(9), gpr(10), gpr(11), gpr(12), cr().get());
								}
							} else {
								stuck_count = 0;
								last_pc = cur_pc;
							}
							last_t = now;
						}
						}
					}
					jit_ring_record(regs_ptr(), 'J', jit_block_start_pc, pc(), 0);
					/* Log JIT block: from, to, then the 68k-emulator-relevant state
					 * (r24=68k PC, r27=68k opcode, r29=handler addr, LR, CR, XER).
					 * fflush so the final entries survive a crash. */
					if (jit_trace_fp_for_cpu) {
						/* In the ROM 68k (DR) emulator's register convention:
						 * r24 = 68k PC, r27 = opcode, r29 = handler address,
						 * r8-r15 = 68k D0-D7, r16-r23 = 68k A0-A7.
						 * a7 (r23) is the 68k stack pointer — the register whose
						 * corruption (a7=0) is the deterministic boot-crash signature. */
						fprintf(jit_trace_fp_for_cpu, "J %08x %08x r24=%08x r27=%08x r29=%08x cr=%08x so=%d ov=%d ca=%d a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x\n",
						        jit_block_start_pc, pc(), gpr(24), gpr(27), gpr(29), cr().get(),
						        xer().get_so(), xer().get_ov(), xer().get_ca(),
						        gpr(16), gpr(17), gpr(18), gpr(19), gpr(20), gpr(21), gpr(22), gpr(23));
						fflush(jit_trace_fp_for_cpu);
					}
					/* GATE 3: PC range diagnostic (log-only, rate-limited).
					 * A result PC outside RAM/ROM/SheepMem is either a legitimate
					 * branch into other Mac OS space (kernel data, DR emulator —
					 * the interpreter handles those natively) or a JIT bug.  Either
					 * way the fast-dispatch fallback below routes it correctly:
					 * compile() refuses non-compilable PCs, so execution falls back
					 * to the interpreter block cache for that PC.
					 * Do NOT evict the source block: legitimate out-of-range
					 * branches are normal control flow, and evicting forces a
					 * pointless recompile on the block's next visit. */
					uint32 jit_pc = pc();
					if (jit_pc >= (uint32)(uintptr_t)RAMBaseHost + RAMSize &&
					    !(jit_pc >= (uint32)ROMBase && jit_pc < (uint32)ROMBase + 0x600000)) {
						static int gate3_log_budget = 10;
						if (gate3_log_budget > 0) {
							gate3_log_budget--;
							fprintf(stderr, "PPC-JIT-A64: GATE3: out-of-range PC 0x%08x after block at 0x%08x — interpreter dispatch%s\n",
							        jit_pc, jit_block_start_pc,
							        gate3_log_budget == 0 ? " (further messages suppressed)" : "");
						}
					}
					if (!spcflags().empty()) {
						if (!check_spcflags()) goto return_site;
					}
					/* Fast dispatch: if next PC is already in JIT cache, stay in the
					 * JIT loop without touching the interpreter block cache.
					 * This eliminates my_block_cache.find() + pdi_execute overhead for
					 * hot block-to-block transitions where both blocks are JIT-compiled. */
					jit_block_start_pc = pc();
					fn = ppc_jit_aarch64_lookup_fast(pc());
					if (!fn && ppc_jit_aarch64_compile(pc(), RAMBaseHost, RAMSize, &jblk) && jblk.complete)
						fn = (ppc_jit_entry_fn)(void*)jblk.code;
					if (fn) {
						if (__builtin_expect(jit_verify_enabled, false)) {
							memcpy(&jit_verify_pre_state, regs_ptr(), sizeof(powerpc_registers));
							jit_verify_n_insns = ppc_jit_aarch64_lookup_n_insns(jit_block_start_pc);
						}
						// SS_SEED_MEM (PC-triggered): apply seeds at this block entry.
						// Inline-guarded like SS_PROBE_PC so it is ~0 cost on the hot
						// dispatch path when no seeds are set (-1 -> parse once -> 0 forever).
						if (__builtin_expect(s_seed_count != 0, false))
							ss_seed_mem_check_pc((uint32_t)jit_block_start_pc);
						if (__builtin_expect(s_probe_count > 0, false)) {
							uint32_t bpc = (uint32_t)jit_block_start_pc;
							for (int pi = 0; pi < s_probe_count; pi++) {
								if (s_probes[pi].pc == bpc) {
									s_probes[pi].visits++;
									if (probe_should_log(s_probes[pi].visits)) {
										probe_entry *pe = &s_probes[pi];
										fprintf(stderr, "[PROBE 0x%08x visit=%llu]", bpc, (unsigned long long)pe->visits);
										if (pe->dump_all) {
											fprintf(stderr, "\n");
											for (int ri = 0; ri < 32; ri++) {
												fprintf(stderr, "  r%-2d = 0x%08x", ri, (uint32_t)gpr(ri));
												if ((ri & 3) == 3) fprintf(stderr, "\n");
											}
											fprintf(stderr, "  CR=0x%08x LR=0x%08x CTR=0x%08x\n",
											        (uint32_t)cr().get(), (uint32_t)lr(), (uint32_t)ctr());
										}
										for (int fi = 0; fi < pe->n_fields; fi++) {
											if (pe->fields[fi].type == PROBE_GPR)
												fprintf(stderr, " r%u=0x%08x", pe->fields[fi].value, (uint32_t)gpr(pe->fields[fi].value));
											else if (pe->fields[fi].type == PROBE_MEM) {
												// M1: refuse side-effecting device reads (§2b).
												if (vm_is_mmio(pe->fields[fi].value))
													fprintf(stderr, " [0x%08x]=<MMIO-refused>", pe->fields[fi].value);
												else {
													uint32_t val = vm_read_memory_4(pe->fields[fi].value);
													fprintf(stderr, " [0x%08x]=0x%08x", pe->fields[fi].value, val);
												}
											} else {
												// PROBE_MEM_REG: [rN:SIZE] — dump SIZE bytes from address in gpr(N)
												uint32_t base_addr = (uint32_t)gpr(pe->fields[fi].value);
												uint32_t sz = pe->fields[fi].size;
												fprintf(stderr, "\n  mem[r%u=0x%08x +0x0000..+0x%04x]:",
												        pe->fields[fi].value, base_addr, sz);
												uint32_t nwords = (sz + 3) / 4;
												for (uint32_t wi = 0; wi < nwords; wi++) {
													if ((wi & 7) == 0)
														fprintf(stderr, "\n    +0x%04x:", wi * 4);
													if (vm_is_mmio(base_addr + wi * 4)) { fprintf(stderr, " <refused>"); continue; }
													uint32_t val = vm_read_memory_4(base_addr + wi * 4);
													fprintf(stderr, " %08x", val);
												}
												fprintf(stderr, "\n");
											}
										}
										fprintf(stderr, "\n");
									}
									fflush(stderr);
								}
							}
						}
						fn((void*)regs_ptr());
						goto pdi_jit_post;
					}
					/* Region profiling: JIT dispatch could not handle this PC (typically
					 * the DR emulator range) — falling through to the interpreter. */
					rgn_jit_to_interp++;
					bi = my_block_cache.find(pc());
					if (bi) goto pdi_execute;
					continue;
				}
			}
#endif
		  skip_jit:
			// B1 fallback trace: count interpreter fallbacks by PC
			if (__builtin_expect(jit_profile_enabled, false)) {
				jit_fallback_counts[pc()]++;
				jit_fallback_total++;
			}
			for (;;) {
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
				/* Region profiling: count interpreted blocks by entry PC, and emit a
				 * heartbeat in pure-interpreter mode (SS_USE_JIT=0) where the JIT-side
				 * heartbeat never runs.  Clock checked every 4096 blocks to keep the
				 * per-block cost to one increment + one branch. */
				{
					static uint64 interp_block_count = 0;
					static double interp_last_t = 0;
					rgn_interp_blocks[rgn_classify(bi->pc)]++;
					interp_block_count++;
					if ((interp_block_count & 0xFFF) == 0) {
						double now = jit_elapsed_s();
						if (now - interp_last_t >= 5.0) {
							jit_diag_log_open();
							fprintf(jit_log_file, "[INTERP %.1fs] blocks=%llu pc=%08x | iNK=%llu iDR=%llu iRAM=%llu iOTH=%llu | j2i=%llu i2j=%llu\n",
							        now, (unsigned long long)interp_block_count, (uint32)bi->pc,
							        (unsigned long long)rgn_interp_blocks[RGN_NK], (unsigned long long)rgn_interp_blocks[RGN_DR],
							        (unsigned long long)rgn_interp_blocks[RGN_RAM], (unsigned long long)rgn_interp_blocks[RGN_OTH],
							        (unsigned long long)rgn_jit_to_interp, (unsigned long long)rgn_interp_to_jit);
							fflush(jit_log_file);
							/* Terminal heartbeat (stderr + log): 10s cadence for the
							 * first minute, then every 60s.  See jit-heartbeat.hpp. */
							{
								static hb_state hb;
								/* M3a Task 4: [EXC] counters on the heartbeat (see JIT-mode
								 * call site above). Newworld only.
								 * M6a Wave 1: + MMIO region read counts (memo §5.5a). */
								char excbuf[160]; excbuf[0] = 0;
								if (MachineProfileIsNewWorld()) {
									uint64_t exc[3];
									SheepExcStats(exc);
									snprintf(excbuf, sizeof excbuf, " | exc=%llu/%llu/%llu",
									         (unsigned long long)exc[0], (unsigned long long)exc[1],
									         (unsigned long long)exc[2]);
									hb_append_mmio_suffix(excbuf, sizeof excbuf);
								}
								hb_tick(&hb, jit_log_file, false, now, interp_block_count,
								        0, rgn_interp_blocks, rgn_interp_to_jit,
								        excbuf[0] ? excbuf : NULL);
							}
							/* SS_JIT_MEMDUMP_AT: timer-based memory dump (interpreter mode) */
							{
								static int md_done = 0;
								static double md_t = 0;
								if (!md_done) {
									if (md_t == 0) {
										const char *e = getenv("SS_JIT_MEMDUMP_AT");
										md_t = e ? atof(e) : -1;
									}
									if (md_t > 0 && now >= md_t) {
										md_done = 1;
										const char *path = getenv("SS_JIT_MEMDUMP_PATH");
										if (!path) path = "/tmp/ss_memdump.bin";
										FILE *mf = fopen(path, "wb");
										if (mf) {
											uint32 sz = RAMSize < 0x1000000 ? RAMSize : 0x1000000;
											fwrite(RAMBaseHost, 1, sz, mf);
											fclose(mf);
											fprintf(stderr, "[INTERP %.1fs] Memory dump: %s (%u bytes)\n", now, path, sz);
										}
									}
								}
							}
							interp_last_t = now;
						}
					}
				}
#endif
				const int r = bi->size % 4;
				di = bi->di + r;
				int n = (bi->size + 3) / 4;
				switch (r) {
				case 0: do {
						di += 4;
						di[-4].execute(this, di[-4].opcode);
				case 3: di[-3].execute(this, di[-3].opcode);
				case 2: di[-2].execute(this, di[-2].opcode);
				case 1: di[-1].execute(this, di[-1].opcode);
					} while (--n > 0);
				}

				if (!spcflags().empty()) {
					if (!check_spcflags())
						goto return_site;

					// Force redecoding if cache was invalidated
					if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
						spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
						invalidated_cache = true;
						break;
					}
				}

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
				/* JIT handoff: exit the interpreter loop when the next PC is in the
				 * JIT's compilable domain (RAM / registered ROM), so the outer
				 * dispatch compiles and runs it natively.  This must test
				 * compilABILITY, not "already compiled": code first reached from
				 * inside an interpreter session (e.g. toolbox routines called by the
				 * interpreter-only 68k emulator) has no block yet and would otherwise
				 * never meet the compiler — leaving most of the OS interpreted and
				 * JIT mode slower than pure interpreter mode.
				 * Cost: 2-4 compares per interpreted block. */
				if (bi->pc != pc() && ppc_jit_aarch64_is_compilable(pc()))
					break;
#endif

				if ((bi->pc != pc()) && ((bi = my_block_cache.find(pc())) == NULL))
					break;
			}
		}
#else
		goto do_interpret;
#endif
	}
#endif
  do_interpret:
	for (;;) {
		uint32 opcode = vm_read_memory_4(pc());
		const instr_info_t *ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_instruction(opcode);
#endif
#if PPC_FLIGHT_RECORDER
		if (is_logging())
			record_step(opcode);
#endif
#ifdef __MINGW32__
		assert(ii->execute.default_call_conv_ptr() != 0);
#else
		assert(ii->execute.ptr() != 0);
#endif
		ii->execute(this, opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_registers();
#endif
		if (!spcflags().empty() && !check_spcflags())
			goto return_site;
	}
  return_site:
	// Tell upper level we invalidated cache?
	if (invalidated_cache)
		spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	--execute_depth;
}

void powerpc_cpu::execute()
{
	execute(pc());
}

void powerpc_cpu::init_decode_cache()
{
#if PPC_DECODE_CACHE
	decode_cache = (block_info::decode_info *)vm_acquire(DECODE_CACHE_SIZE);
	if (decode_cache == VM_MAP_FAILED) {
		fprintf(stderr, "powerpc_cpu: Could not allocate decode cache\n");
		abort();
	}

	D(bug("powerpc_cpu: Allocated decode cache: %d KB at %p\n", DECODE_CACHE_SIZE / 1024, decode_cache));
	decode_cache_p = decode_cache;
	decode_cache_end_p = decode_cache + DECODE_CACHE_MAX_ENTRIES;
#if FLIGHT_RECORDER
	// Leave enough room to last call to record_step()
	decode_cache_end_p -= 2;
#endif
#if PPC_EXECUTE_DUMP_STATE
	// Leave enough room to last calls to dump state functions
	decode_cache_end_p -= 2;
#endif
#endif
}

void powerpc_cpu::kill_decode_cache()
{
#if PPC_DECODE_CACHE
	vm_release(decode_cache, DECODE_CACHE_SIZE);
#endif
}

void powerpc_cpu::invalidate_cache()
{
	D(bug("Invalidate all cache blocks\n"));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.clear();
	my_block_cache.initialize();
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
#endif
#if PPC_ENABLE_JIT
	codegen.invalidate_cache();
#endif
#if PPC_DECODE_CACHE
	decode_cache_p = decode_cache;
#endif
}

void powerpc_block_info::invalidate()
{
#if PPC_DECODE_CACHE
	// Don't do anything if this is a predecoded block
	if (di)
		return;
#endif
#if DYNGEN_DIRECT_BLOCK_CHAINING
	for (int i = 0; i < MAX_TARGETS; i++) {
		link_info * const tli = &li[i];
		uint32 tpc = tli->jmp_pc;
		// For any jump within page boundaries, reset the jump address
		// to the target block resolver (trampoline)
		if (tpc != INVALID_PC && ((tpc ^ pc) >> 12) == 0)
			dg_set_jmp_target(tli->jmp_addr, tli->jmp_resolve_addr);
	}
#endif
}

void powerpc_cpu::invalidate_cache_range(uintptr start, uintptr end)
{
	D(bug("Invalidate cache block [%08x - %08x]\n", start, end));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
#if DYNGEN_DIRECT_BLOCK_CHAINING
	if (use_jit) {
		// Invalidate on page boundaries
		start &= -4096;
		end = (end + 4095) & -4096;
		D(bug("    at page boundaries [%08x - %08x]\n", start, end));
	}
#endif
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	my_block_cache.clear_range(start, end);
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	ppc_jit_aarch64_invalidate_range((uint32_t)start, (uint32_t)end);
#endif
#endif
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Evict aarch64 JIT blocks whose start PC falls in the invalidated range.
	 * Mac OS signals code modification via icbi/isync (the interpreter's
	 * execute_icbi/execute_isync funnel here); without this, blocks compiled
	 * from pre-write memory would keep executing stale translations forever.
	 * Relevant when guest code is written at runtime (extension loading,
	 * relocated stubs, self-modifying application code). */
	ppc_jit_aarch64_invalidate_range((uint32_t)start, (uint32_t)end);
#endif
}
