/*
 *  jit-heartbeat.hpp - Terminal (stderr) heartbeat for SheepShaver runs
 *
 *  Prints a one-line liveness + stats heartbeat to stderr (and mirrors it to the
 *  diag log file) on a decaying cadence: every 10s for the first minute of guest
 *  execution, then every 60s.  Purpose: make "is it still alive, and what is it
 *  doing?" answerable from the terminal the emulator was launched in, without
 *  tailing the diag log in a second terminal.
 *
 *  Design notes:
 *   - Header-only, no globals shared across call sites: each call site owns an
 *     hb_state.  Driven by the existing 5-second heartbeat check in ppc-cpu.cpp
 *     (10 and 60 are multiples of 5), so there are no new timers/threads and no
 *     added per-block cost.
 *   - A fully-hung dispatch loop produces NO heartbeat lines — that silence is
 *     itself the signal (same semantics as the existing file heartbeat).
 *   - RSS/CPU sampling failures degrade to "rss=? cpu=?", never break execution.
 *
 *  See docs/superpowers/specs/2026-06-03-terminal-heartbeat-design.md
 */

#ifndef JIT_HEARTBEAT_HPP
#define JIT_HEARTBEAT_HPP

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

/* Per-call-site heartbeat state. Zero-initialize (static storage does this). */
struct hb_state {
	double   next_due;     /* next emission time; 0 = not started (first due at 10s) */
	uint64_t prev_blocks;  /* block count at last emission (for rate) */
	double   prev_t;       /* wall time at last emission (for rate) */
	double   prev_cpu_s;   /* process CPU seconds at last emission (for cpu%) */
};

/* Current resident set size in MB, or -1 if unavailable. */
static inline double hb_rss_mb(void)
{
#ifdef __APPLE__
	mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	              (task_info_t)&info, &count) == KERN_SUCCESS)
		return info.resident_size / (1024.0 * 1024.0);
	return -1;
#else
	/* Portable fallback: peak RSS (not current), ru_maxrss is KB on Linux. */
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) == 0)
		return ru.ru_maxrss / 1024.0;
	return -1;
#endif
}

/* Process CPU seconds (user+sys), or -1 if unavailable. */
static inline double hb_cpu_seconds(void)
{
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return -1;
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 +
	       ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}

/* Format a large count compactly: 4567 -> "4567", 812345678 -> "812M", 4.1e9 -> "4.1G" */
static inline void hb_fmt_count(uint64_t v, char *buf, size_t n)
{
	if (v >= 10000000000ULL)
		snprintf(buf, n, "%.1fG", v / 1e9);
	else if (v >= 10000000ULL)
		snprintf(buf, n, "%lluM", (unsigned long long)(v / 1000000ULL));
	else
		snprintf(buf, n, "%llu", (unsigned long long)v);
}

/* Format elapsed time compactly: "30s", "2m", "75m" */
static inline void hb_fmt_time(double now, char *buf, size_t n)
{
	if (now < 120.0)
		snprintf(buf, n, "%.0fs", now);
	else
		snprintf(buf, n, "%dm", (int)(now / 60.0));
}

/*
 * Tick the heartbeat.  Call from inside the existing 5s heartbeat block.
 * Emits to stderr (always) and log_file (if non-NULL) when due; otherwise no-op.
 *
 *   st        - per-call-site state (static at the call site)
 *   log_file  - diag log FILE* or NULL
 *   jit_mode  - true: print comp= and jNK/jDR/jRAM/j2i; false: iNK/iDR/iRAM/i2j
 *   now       - seconds since run start (jit_elapsed_s())
 *   blocks    - total blocks executed by this loop
 *   compiled  - compiled block count (ignored when !jit_mode)
 *   rgn       - region counters indexed by RGN_NK/RGN_DR/RGN_RAM (size >= 3)
 *   trans     - j2i transitions (jit_mode) or i2j transitions (!jit_mode)
 */
static inline void hb_tick(hb_state *st, FILE *log_file, bool jit_mode, double now,
                           uint64_t blocks, uint32_t compiled,
                           const uint64_t *rgn, uint64_t trans)
{
	if (st->next_due == 0)
		st->next_due = 10.0;
	if (now < st->next_due)
		return;

	/* Rate over the window since the last emission (or since start for the first). */
	double dt = now - st->prev_t;
	double rate_m = (dt > 0) ? (blocks - st->prev_blocks) / dt / 1e6 : 0;

	/* CPU% of one core over the same window. */
	double cpu_s = hb_cpu_seconds();
	double cpu_pct = -1;
	if (cpu_s >= 0 && st->prev_cpu_s >= 0 && dt > 0)
		cpu_pct = 100.0 * (cpu_s - st->prev_cpu_s) / dt;

	double rss = hb_rss_mb();

	char tbuf[24], bbuf[24], r0[24], r1[24], r2[24], trbuf[24];
	hb_fmt_time(now, tbuf, sizeof tbuf);
	hb_fmt_count(blocks, bbuf, sizeof bbuf);
	hb_fmt_count(rgn[0], r0, sizeof r0);   /* RGN_NK  */
	hb_fmt_count(rgn[1], r1, sizeof r1);   /* RGN_DR  */
	hb_fmt_count(rgn[2], r2, sizeof r2);   /* RGN_RAM */
	hb_fmt_count(trans, trbuf, sizeof trbuf);

	char rssbuf[24], cpubuf[24];
	if (rss >= 0) snprintf(rssbuf, sizeof rssbuf, "%.0fMB", rss);
	else          snprintf(rssbuf, sizeof rssbuf, "?");
	if (cpu_pct >= 0) snprintf(cpubuf, sizeof cpubuf, "%.0f%%", cpu_pct);
	else              snprintf(cpubuf, sizeof cpubuf, "?");

	char line[256];
	if (jit_mode)
		snprintf(line, sizeof line,
		         "[HB %s] blocks=%s (%.1fM/s) comp=%u | jNK=%s jDR=%s jRAM=%s j2i=%s | rss=%s cpu=%s",
		         tbuf, bbuf, rate_m, compiled, r0, r1, r2, trbuf, rssbuf, cpubuf);
	else
		snprintf(line, sizeof line,
		         "[HB %s] blocks=%s (%.1fM/s) interp | iNK=%s iDR=%s iRAM=%s i2j=%s | rss=%s cpu=%s",
		         tbuf, bbuf, rate_m, r0, r1, r2, trbuf, rssbuf, cpubuf);

	fprintf(stderr, "%s\n", line);
	if (log_file) {
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}

	/* Advance: 10s steps for the first minute, then 60s steps, with catch-up so a
	 * debugger pause doesn't cause a burst of lines. */
	st->next_due += (st->next_due < 60.0) ? 10.0 : 60.0;
	while (st->next_due <= now)
		st->next_due += 60.0;

	st->prev_blocks = blocks;
	st->prev_t = now;
	st->prev_cpu_s = cpu_s;
}

#endif /* JIT_HEARTBEAT_HPP */
