/*
 *  test_nk_mmu_trace.cpp - standalone unit test for the NK MMU-write oracle recorder's
 *  pure format/classification logic (no emulator, no boot). Build: machine/Makefile.
 */

#include "nk_mmu_trace.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void test_kind_names(void)
{
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_MTSR), "MTSR") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_MTSRIN), "MTSRIN") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_MTSDR1), "MTSDR1") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_SC), "SC") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_EXT), "EXT") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(-1), "?") == 0);
	CHECK(strcmp(nk_mmu_trace_kind_name(NK_MMU_KIND_COUNT), "?") == 0);
}

static void test_classify_bat(void)
{
	int kind; uint32_t idx;
	/* IBAT0U=528 .. IBAT3L=535 ; DBAT0U=536 .. DBAT3L=543. */
	CHECK(nk_mmu_classify_bat_spr(528, &kind, &idx) == 1 && kind == NK_MMU_MTIBATU && idx == 0);
	CHECK(nk_mmu_classify_bat_spr(529, &kind, &idx) == 1 && kind == NK_MMU_MTIBATL && idx == 0);
	CHECK(nk_mmu_classify_bat_spr(535, &kind, &idx) == 1 && kind == NK_MMU_MTIBATL && idx == 3);
	CHECK(nk_mmu_classify_bat_spr(536, &kind, &idx) == 1 && kind == NK_MMU_MTDBATU && idx == 0);
	CHECK(nk_mmu_classify_bat_spr(537, &kind, &idx) == 1 && kind == NK_MMU_MTDBATL && idx == 0);
	CHECK(nk_mmu_classify_bat_spr(542, &kind, &idx) == 1 && kind == NK_MMU_MTDBATU && idx == 3);
	CHECK(nk_mmu_classify_bat_spr(543, &kind, &idx) == 1 && kind == NK_MMU_MTDBATL && idx == 3);
	/* Out of range -> 0, no write. */
	CHECK(nk_mmu_classify_bat_spr(527, &kind, &idx) == 0);
	CHECK(nk_mmu_classify_bat_spr(544, &kind, &idx) == 0);
	CHECK(nk_mmu_classify_bat_spr(25 /*SDR1*/, &kind, &idx) == 0);
}

static void test_format(void)
{
	char buf[128];
	size_t n = nk_mmu_trace_format(buf, sizeof(buf), 0, NK_MMU_MTSR,
	                               0x00f10000, 0x7c0001a4, 5, 0x20000000);
	const char *expect = "0 MTSR pc=00f10000 insn=7c0001a4 idx=5 val=20000000\n";
	CHECK(strcmp(buf, expect) == 0);
	CHECK(n == strlen(expect));

	/* SDR1 with high seq + idx=0. */
	n = nk_mmu_trace_format(buf, sizeof(buf), 42, NK_MMU_MTSDR1,
	                        0x00f10604, 0x7c1903a6, 0, 0x00010003);
	CHECK(strcmp(buf, "42 MTSDR1 pc=00f10604 insn=7c1903a6 idx=0 val=00010003\n") == 0);

	/* DBAT pair 1 lower. */
	n = nk_mmu_trace_format(buf, sizeof(buf), 7, NK_MMU_MTDBATL,
	                        0x00f152c4, 0x7c1383a6, 1, 0x10000002);
	CHECK(strcmp(buf, "7 MTDBATL pc=00f152c4 insn=7c1383a6 idx=1 val=10000002\n") == 0);
}

int main(void)
{
	test_kind_names();
	test_classify_bat();
	test_format();
	if (failures == 0)
		printf("test_nk_mmu_trace: ALL PASS\n");
	else
		printf("test_nk_mmu_trace: %d FAILURE(S)\n", failures);
	return failures ? 1 : 0;
}
