# Standalone unit tests for the machine layer (no emulator build needed).
CXX      = clang++
CXXFLAGS = -std=c++11 -Wall -g
CPPFLAGS = -I../include -DMACHINE_PROFILE_STANDALONE_TEST

TESTS = test_machine_profile test_mmio_bus test_mmio_aperture test_dev_scc8530 test_dev_via6522 test_a64_mmio_decode test_mmio_machfault test_virt_clock test_event_sched test_exc_core test_exc_chain test_exc_inject test_dev_cuda test_adb_stub test_dev_openpic test_paged_mmu test_openfirmware_ci test_trampoline_loader test_trampoline_ofci_shim test_shm_arena_spike test_shm_arena_multientry

test: $(TESTS)
	./test_machine_profile
	./test_mmio_bus
	./test_mmio_aperture
	./test_dev_scc8530
	./test_dev_via6522
	./test_a64_mmio_decode
	./test_mmio_machfault
	./test_virt_clock
	./test_event_sched
	./test_exc_core
	./test_exc_chain
	./test_exc_inject
	./test_dev_cuda
	./test_adb_stub
	./test_dev_openpic
	./test_paged_mmu
	./test_openfirmware_ci
	./test_trampoline_loader
	./test_trampoline_ofci_shim
	./test_shm_arena_spike
	./test_shm_arena_multientry

test_machine_profile: test_machine_profile.cpp machine_profile.cpp ../include/machine_profile.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_machine_profile.cpp machine_profile.cpp

test_mmio_bus: test_mmio_bus.cpp mmio_bus.cpp ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_bus.cpp mmio_bus.cpp

# T-F6: aperture must not extend the trap hull (fails before Task B, passes after)
test_mmio_aperture: test_mmio_aperture.cpp mmio_bus.cpp ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_aperture.cpp mmio_bus.cpp

test_dev_scc8530: test_dev_scc8530.cpp dev_scc8530.cpp mmio_bus.cpp ../include/dev_scc8530.h ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_scc8530.cpp dev_scc8530.cpp

test_dev_via6522: test_dev_via6522.cpp dev_via6522.cpp dev_cuda.cpp event_sched.cpp mmio_bus.cpp \
                  ../include/dev_via6522.h ../include/dev_cuda.h ../include/event_sched.h ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_via6522.cpp dev_via6522.cpp dev_cuda.cpp event_sched.cpp mmio_bus.cpp

test_a64_mmio_decode: test_a64_mmio_decode.cpp a64_mmio_decode.cpp ../include/a64_mmio_decode.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_a64_mmio_decode.cpp a64_mmio_decode.cpp

test_mmio_machfault: test_mmio_machfault.cpp mmio_machfault.cpp mmio_bus.cpp a64_mmio_decode.cpp \
    ../include/mmio_bus.h ../include/a64_mmio_decode.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_machfault.cpp mmio_machfault.cpp \
	    mmio_bus.cpp a64_mmio_decode.cpp

test_virt_clock: test_virt_clock.cpp virt_clock.cpp ../include/virt_clock.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_virt_clock.cpp virt_clock.cpp

test_event_sched: test_event_sched.cpp event_sched.cpp ../include/event_sched.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_event_sched.cpp event_sched.cpp

test_exc_core: test_exc_core.cpp exc_core.cpp ../include/exc_core.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_exc_core.cpp exc_core.cpp

# SS_M18 S3 T2: the host->NK EXT-injection shim (LOAD-BEARING). PLANTED-TABLE
# micro-test (G(T2) observable C1) — pure module + ExcEnter, no kpx_cpu link.
test_exc_inject: test_exc_inject.cpp exc_inject.cpp exc_core.cpp ../include/exc_inject.h ../include/exc_core.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_exc_inject.cpp exc_inject.cpp exc_core.cpp

# M3b Wave 2 W2-0: the delivery-decision chain (ExcDeliveryDecision/ExcEdgeReRaise)
# composed with the real VirtClock latch (U1-U13, EE-CHAIN-RECON.md §C.1 + rev 2).
test_exc_chain: test_exc_chain.cpp exc_core.cpp virt_clock.cpp ../include/exc_core.h ../include/virt_clock.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_exc_chain.cpp exc_core.cpp virt_clock.cpp

# M3b Wave 1 (Tasks 1+2). test_dev_cuda mocks the ADB handler (injected callback)
# so it does NOT link adb_stub.cpp — the two modules build/test independently.
# (No event_sched link either: dev_cuda is lazy-only, plan rev 2 M4.)
test_dev_cuda: test_dev_cuda.cpp dev_cuda.cpp ../include/dev_cuda.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_cuda.cpp dev_cuda.cpp

test_adb_stub: test_adb_stub.cpp adb_stub.cpp ../include/adb_stub.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_adb_stub.cpp adb_stub.cpp

# M3b Wave 2 (stop-rule disposition: model + unit tests, EXC_EXTERNAL wiring deferred)
test_dev_openpic: test_dev_openpic.cpp dev_openpic.cpp ../include/dev_openpic.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_openpic.cpp dev_openpic.cpp

# SS_M18 S1 Task A2: mechanism-agnostic PPC classic MMU translation core.
# Standalone (no kpx_cpu link); spec-derived differential-implementation oracle.
test_paged_mmu: test_paged_mmu.cpp paged_mmu.cpp ../include/paged_mmu.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_paged_mmu.cpp paged_mmu.cpp

# SS_M18 S2a: OF-CI callback + Core99 device-tree model.
# Standalone (no kpx_cpu link); structurally inert (no emulator caller).
test_openfirmware_ci: test_openfirmware_ci.cpp openfirmware_ci.cpp ../include/openfirmware_ci.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_openfirmware_ci.cpp openfirmware_ci.cpp

# SS_M18 S2b T1: staged-asset MacOS.elf ELF loader. Standalone (no kpx_cpu link);
# the pure parse/place core is compiled with TRAMPOLINE_LOADER_STANDALONE_TEST
# (the emulator wrapper is excluded). Placement half of G2b.a.
test_trampoline_loader: test_trampoline_loader.cpp trampoline_loader.cpp ../include/trampoline_loader.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -DTRAMPOLINE_LOADER_STANDALONE_TEST -o $@ test_trampoline_loader.cpp trampoline_loader.cpp

# SS_M18 S2b T2: the guest-callable r5 OF-CI marshalling shim (LOAD-BEARING).
# Standalone (no kpx_cpu link): the pure marshalling core is compiled with
# TRAMPOLINE_OFCI_SHIM_STANDALONE_TEST (the emulator wrapper excluded) and linked
# against openfirmware_ci.cpp; the planted-array micro-test drives a guest BE-32
# CHRP array THROUGH the shim's EXEC_NATIVE guest-entry opcode (G2b.shim).
test_trampoline_ofci_shim: test_trampoline_ofci_shim.cpp trampoline_ofci_shim.cpp openfirmware_ci.cpp \
                           ../include/trampoline_ofci_shim.h ../include/openfirmware_ci.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -DTRAMPOLINE_OFCI_SHIM_STANDALONE_TEST -o $@ \
	    test_trampoline_ofci_shim.cpp trampoline_ofci_shim.cpp openfirmware_ci.cpp

# SS_M18 S3-Task-0 item-0: Dolphin SHM-arena platform-feasibility spike.
# Standalone (no kpx_cpu, no paged_mmu link); proves the mach VM aliasing/remap
# primitive on macOS arm64. Inert: linked into no emulator binary.
test_shm_arena_spike: test_shm_arena_spike.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_shm_arena_spike.cpp

# SS_M18 S3-Task-0 (live-MMU de-risk follow-on): multi-entry transaction spike.
# Standalone (no kpx_cpu, no paged_mmu link); proves the SET re-alias / cross-region
# concurrent-reader / mixed-perm-fork / remap-latency questions the single-remap spike
# left OWED. Inert: linked into no emulator binary.
test_shm_arena_multientry: test_shm_arena_multientry.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_shm_arena_multientry.cpp

clean:
	rm -f $(TESTS)
	rm -rf $(addsuffix .dSYM,$(TESTS))

.PHONY: test clean
