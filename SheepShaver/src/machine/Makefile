# Standalone unit tests for the machine layer (no emulator build needed).
CXX      = clang++
CXXFLAGS = -std=c++11 -Wall -g
CPPFLAGS = -I../include -DMACHINE_PROFILE_STANDALONE_TEST

TESTS = test_machine_profile test_mmio_bus test_dev_scc8530 test_dev_via6522 test_a64_mmio_decode test_mmio_machfault

test: $(TESTS)
	./test_machine_profile
	./test_mmio_bus
	./test_dev_scc8530
	./test_dev_via6522
	./test_a64_mmio_decode
	./test_mmio_machfault

test_machine_profile: test_machine_profile.cpp machine_profile.cpp ../include/machine_profile.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_machine_profile.cpp machine_profile.cpp

test_mmio_bus: test_mmio_bus.cpp mmio_bus.cpp ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_bus.cpp mmio_bus.cpp

test_dev_scc8530: test_dev_scc8530.cpp dev_scc8530.cpp mmio_bus.cpp ../include/dev_scc8530.h ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_scc8530.cpp dev_scc8530.cpp

test_dev_via6522: test_dev_via6522.cpp dev_via6522.cpp ../include/dev_via6522.h ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_via6522.cpp dev_via6522.cpp

test_a64_mmio_decode: test_a64_mmio_decode.cpp a64_mmio_decode.cpp ../include/a64_mmio_decode.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_a64_mmio_decode.cpp a64_mmio_decode.cpp

test_mmio_machfault: test_mmio_machfault.cpp mmio_machfault.cpp mmio_bus.cpp a64_mmio_decode.cpp \
    ../include/mmio_bus.h ../include/a64_mmio_decode.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_machfault.cpp mmio_machfault.cpp \
	    mmio_bus.cpp a64_mmio_decode.cpp

clean:
	rm -f $(TESTS)
	rm -rf $(addsuffix .dSYM,$(TESTS))

.PHONY: test clean
