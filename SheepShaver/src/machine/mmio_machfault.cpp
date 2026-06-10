/*  mmio_machfault.cpp - JIT-path MMIO dispatch (Mach fault -> decode -> bus -> writeback). */
#include "mmio_bus.h"
bool MMIOMachFaultDispatch(uint32_t guest_addr, void *thread_state64)
{
	(void)guest_addr; (void)thread_state64;
	return false;   // filled in by M1 Task 6
}
