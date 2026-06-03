/* Standalone test for jit-heartbeat.hpp warning rules.
 * Feeds synthetic stats into hb_tick() and prints what each scenario produces.
 * stderr is piped (non-TTY) so output must be plain text, no ANSI codes. */
#include <stdio.h>
#include "jit-heartbeat.hpp"

int main()
{
	fprintf(stderr, "--- Scenario 1: healthy boot (expect NO warnings after baseline) ---\n");
	{
		hb_state st = {};
		uint64_t rgn[4] = {0, 0, 0, 0};
		/* 30M blk/s sustained, comp growing, no OTH */
		rgn[0] = 100000000; rgn[1] = 200000000;
		hb_tick(&st, NULL, true, 10.0, 300000000ULL, 1000, rgn, 1000);
		rgn[0] += 100000000; rgn[1] += 200000000;
		hb_tick(&st, NULL, true, 20.0, 600000000ULL, 1200, rgn, 2000);
		rgn[0] += 100000000; rgn[1] += 200000000;
		hb_tick(&st, NULL, true, 30.0, 900000000ULL, 1400, rgn, 3000);
	}

	fprintf(stderr, "\n--- Scenario 2: rate collapse (expect WARN rate collapsed) ---\n");
	{
		hb_state st = {};
		uint64_t rgn[4] = {300000000, 0, 0, 0};
		hb_tick(&st, NULL, true, 10.0, 300000000ULL, 1000, rgn, 100);
		hb_tick(&st, NULL, true, 20.0, 600000000ULL, 1100, rgn, 200);
		/* 20s pass, only 1000 more blocks -> ~0 M/s */
		hb_tick(&st, NULL, true, 40.0, 600001000ULL, 1100, rgn, 200);
	}

	fprintf(stderr, "\n--- Scenario 3: compile freeze at speed (expect SUSPECT at HB3, WARN at HB6+) ---\n");
	{
		hb_state st = {};
		uint64_t rgn[4] = {0, 0, 0, 0};
		uint64_t blocks = 0;
		for (int i = 1; i <= 7; i++) {
			blocks += 300000000ULL;   /* 30M/s, healthy rate */
			rgn[0] = blocks;
			hb_tick(&st, NULL, true, i * 10.0, blocks, 5000 /* frozen */, rgn, 100);
		}
	}

	fprintf(stderr, "\n--- Scenario 4: OTH-region execution + transition thrash (expect both flagged) ---\n");
	{
		hb_state st = {};
		uint64_t rgn[4] = {300000000, 0, 0, 0};
		hb_tick(&st, NULL, true, 10.0, 300000000ULL, 1000, rgn, 0);
		/* next HB: OTH grows, transitions jump by 50M in 10s (5M/s) */
		rgn[0] += 280000000; rgn[3] += 1000;
		hb_tick(&st, NULL, true, 20.0, 580001000ULL, 1100, rgn, 50000000ULL);
	}

	fprintf(stderr, "\n--- Scenario 5: interpreter mode line format ---\n");
	{
		hb_state st = {};
		uint64_t rgn[4] = {50000000, 100000000, 20000000, 0};
		hb_tick(&st, NULL, false, 10.0, 170000000ULL, 0, rgn, 500);
	}

	return 0;
}
