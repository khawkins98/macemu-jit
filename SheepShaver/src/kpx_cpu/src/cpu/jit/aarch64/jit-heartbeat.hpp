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
#include <unistd.h>
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
	/* Anomaly-rule baselines (see "Warning matrix" below) */
	int      emissions;        /* HB lines emitted so far (rules need a baseline) */
	double   avg_rate_m;       /* running average block rate, M/s (EMA) */
	uint32_t prev_compiled;    /* compiled count at last emission */
	int      comp_frozen_hbs;  /* consecutive HBs with compiled unchanged at high rate */
	uint64_t prev_trans;       /* transition count at last emission */
	uint64_t prev_oth;         /* OTH-region block count at last emission */
	double   first_rss_mb;     /* RSS at first emission (runaway baseline) */
	double   prev_rss_mb;      /* RSS at last emission */
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

	/* ---- Warning matrix (canonical copy: SheepShaver/docs/DIAGNOSTICS.md) ----
	 *
	 *  Signal                  SUSPECT (yellow)              WARN (red)
	 *  ----------------------  ----------------------------  ---------------------------
	 *  Execution rate          < 50% of running average      < 0.5M blk/s after 30s
	 *  Compile freeze (JIT)    comp unchanged 2 HBs @ >1M/s  unchanged 5+ HBs
	 *  JIT<->interp trans      > 100K/s                      > 1M/s
	 *  Wild PC (OTH region)    any growth                    > 1% of all blocks
	 *  Memory (RSS)            +10% between HBs after 60s    2x initial or > 2GB
	 *  CPU utilization         < 80% after 30s               < 50%
	 *
	 *  Deliberately NOT a rule: "same PC across heartbeats" — that is a sampling
	 *  artifact, not a hang signal (LEARNINGS.md, session 5 retraction).  Do not
	 *  add it back.
	 *
	 *  Guards: no rules on the first emission (no baseline); rules whose inputs
	 *  are unavailable (rss/cpu < 0) silently skip. */
	const char *findings[6];
	int n_findings = 0;
	int severity = 0;  /* 0 = ok, 1 = suspect, 2 = warn */
#define HB_FLAG(sev, msg) do { \
		if (n_findings < 6) findings[n_findings++] = (msg); \
		if ((sev) > severity) severity = (sev); \
	} while (0)
	if (st->emissions >= 1) {
		/* Execution rate */
		if (now > 30.0 && rate_m < 0.5)
			HB_FLAG(2, "rate collapsed");
		else if (st->avg_rate_m > 0 && rate_m < 0.5 * st->avg_rate_m)
			HB_FLAG(1, "rate dropped >50%");
		/* Compile freeze (JIT mode only): same compiled blocks repeating at speed */
		if (jit_mode && rate_m > 1.0) {
			if (compiled == st->prev_compiled) {
				st->comp_frozen_hbs++;
				if (st->comp_frozen_hbs >= 5)
					HB_FLAG(2, "comp frozen 5+ HBs (repeat-loop hang?)");
				else if (st->comp_frozen_hbs >= 2)
					HB_FLAG(1, "comp frozen");
			} else
				st->comp_frozen_hbs = 0;
		}
		/* JIT<->interpreter transition thrash */
		if (dt > 0) {
			double trans_per_s = (trans - st->prev_trans) / dt;
			if (trans_per_s > 1e6)
				HB_FLAG(2, "transition thrash >1M/s");
			else if (trans_per_s > 1e5)
				HB_FLAG(1, "transitions >100K/s");
		}
		/* Wild PC: execution outside RAM/ROM (OTH region) */
		if (rgn[3] * 100 > blocks && blocks > 0)
			HB_FLAG(2, "OTH-region >1% of blocks");
		else if (rgn[3] > st->prev_oth)
			HB_FLAG(1, "OTH-region execution");
		/* Memory */
		if (rss >= 0) {
			if ((st->first_rss_mb > 0 && rss > 2.0 * st->first_rss_mb) || rss > 2048.0)
				HB_FLAG(2, "RSS runaway");
			else if (now > 60.0 && st->prev_rss_mb > 0 && rss > 1.10 * st->prev_rss_mb)
				HB_FLAG(1, "RSS +10%");
		}
		/* CPU starvation */
		if (now > 30.0 && cpu_pct >= 0) {
			if (cpu_pct < 50.0)
				HB_FLAG(2, "cpu starved");
			else if (cpu_pct < 80.0)
				HB_FLAG(1, "cpu low");
		}
	}
#undef HB_FLAG

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

	char line[512];
	int len;
	if (jit_mode)
		len = snprintf(line, sizeof line,
		         "[HB %s] blocks=%s (%.1fM/s) comp=%u | jNK=%s jDR=%s jRAM=%s j2i=%s | rss=%s cpu=%s",
		         tbuf, bbuf, rate_m, compiled, r0, r1, r2, trbuf, rssbuf, cpubuf);
	else
		len = snprintf(line, sizeof line,
		         "[HB %s] blocks=%s (%.1fM/s) interp | iNK=%s iDR=%s iRAM=%s i2j=%s | rss=%s cpu=%s",
		         tbuf, bbuf, rate_m, r0, r1, r2, trbuf, rssbuf, cpubuf);

	/* Append findings: "[WARN: a; b]" / "[SUSPECT: a]".  Plain text — color is
	 * applied only around the stderr copy, never stored in the line itself. */
	if (severity > 0 && len > 0 && (size_t)len < sizeof line) {
		len += snprintf(line + len, sizeof line - len, "  [%s: ",
		                severity == 2 ? "WARN" : "SUSPECT");
		for (int i = 0; i < n_findings && len > 0 && (size_t)len < sizeof line; i++)
			len += snprintf(line + len, sizeof line - len, "%s%s",
			                i ? "; " : "", findings[i]);
		if (len > 0 && (size_t)len < sizeof line)
			len += snprintf(line + len, sizeof line - len, "]");
	}

	/* stderr: ANSI color (yellow=suspect, red=warn) only when it's a real terminal */
	if (severity > 0 && isatty(fileno(stderr)))
		fprintf(stderr, "%s%s\033[0m\n", severity == 2 ? "\033[31m" : "\033[33m", line);
	else
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

	/* Update baselines for the next emission's rules. */
	st->prev_blocks = blocks;
	st->prev_t = now;
	st->prev_cpu_s = cpu_s;
	st->avg_rate_m = (st->emissions == 0) ? rate_m
	                 : 0.7 * st->avg_rate_m + 0.3 * rate_m;
	st->prev_compiled = compiled;
	st->prev_trans = trans;
	st->prev_oth = rgn[3];
	if (st->emissions == 0 && rss >= 0)
		st->first_rss_mb = rss;
	if (rss >= 0)
		st->prev_rss_mb = rss;
	st->emissions++;
}

#endif /* JIT_HEARTBEAT_HPP */
