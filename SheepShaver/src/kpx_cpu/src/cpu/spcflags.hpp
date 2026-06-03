/*
 *  spcflags.hpp - CPU special flags
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

#ifndef SPCFLAGS_H
#define SPCFLAGS_H

#include <atomic>

/**
 *		Basic special flags
 **/

enum {
	SPCFLAG_CPU_EXEC_RETURN			= 1 << 0,	// Return from emulation loop
	SPCFLAG_CPU_TRIGGER_INTERRUPT	= 1 << 1,	// Trigger user interrupt
	SPCFLAG_CPU_HANDLE_INTERRUPT	= 1 << 2,	// Call user interrupt handler
	SPCFLAG_CPU_ENTER_MON			= 1 << 3,	// Enter cxmon
	SPCFLAG_JIT_EXEC_RETURN			= 1 << 4,	// Return from compiled code
};

/* Lock-free spcflags using std::atomic.  The VBL timer thread sets
 * TRIGGER_INTERRUPT from a signal handler at 60 Hz; the old spinlock
 * implementation serialized every set/clear with the dispatch loop's
 * polling reads.  Atomics eliminate the lock overhead entirely. */
class basic_spcflags
{
	std::atomic<uint32> mask;

public:

	basic_spcflags()
		{ init(); }

	void init()
		{ mask.store(0, std::memory_order_relaxed); }

	bool empty() const
		{ return (mask.load(std::memory_order_relaxed) == 0); }

	bool test(uint32 v) const
		{ return (mask.load(std::memory_order_relaxed) & v); }

	void init(uint32 v)
		{ mask.store(v, std::memory_order_release); }

	uint32 get() const
		{ return mask.load(std::memory_order_relaxed); }

	void set(uint32 v)
		{ mask.fetch_or(v, std::memory_order_release); }

	void clear(uint32 v)
		{ mask.fetch_and(~v, std::memory_order_release); }
};

#endif /* SPCFLAGS_H */
