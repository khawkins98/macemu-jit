# Standalone unit tests for the machine layer (no emulator build needed).
CXX      = clang++
CXXFLAGS = -std=c++11 -Wall -g
CPPFLAGS = -I../include -DMACHINE_PROFILE_STANDALONE_TEST

TESTS = test_machine_profile test_mmio_bus test_dev_scc8530 test_dev_via6522 test_a64_mmio_decode test_mmio_machfault test_virt_clock test_event_sched test_exc_core test_dev_cuda test_adb_stub

test: $(TESTS)
	./test_machine_profile
	./test_mmio_bus
	./test_dev_scc8530
	./test_dev_via6522
	./test_a64_mmio_decode
	./test_mmio_machfault
	./test_virt_clock
	./test_event_sched
	./test_exc_core
	./test_dev_cuda
	./test_adb_stub

test_machine_profile: test_machine_profile.cpp machine_profile.cpp ../include/machine_profile.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_machine_profile.cpp machine_profile.cpp

test_mmio_bus: test_mmio_bus.cpp mmio_bus.cpp ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_bus.cpp mmio_bus.cpp

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

# M3b Wave 1 (Tasks 1+2). test_dev_cuda mocks the ADB handler (injected callback)
# so it does NOT link adb_stub.cpp — the two modules build/test independently.
# (No event_sched link either: dev_cuda is lazy-only, plan rev 2 M4.)
test_dev_cuda: test_dev_cuda.cpp dev_cuda.cpp ../include/dev_cuda.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_cuda.cpp dev_cuda.cpp

test_adb_stub: test_adb_stub.cpp adb_stub.cpp ../include/adb_stub.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_adb_stub.cpp adb_stub.cpp

clean:
	rm -f $(TESTS)
	rm -rf $(addsuffix .dSYM,$(TESTS))

.PHONY: test clean
