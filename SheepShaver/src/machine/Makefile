# Standalone unit test for machine_profile (no emulator build needed).
CXX      = clang++
CXXFLAGS = -std=c++11 -Wall -g
CPPFLAGS = -I../include -DMACHINE_PROFILE_STANDALONE_TEST

test: test_machine_profile
	./test_machine_profile

test_machine_profile: test_machine_profile.cpp machine_profile.cpp ../include/machine_profile.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_machine_profile.cpp machine_profile.cpp

clean:
	rm -f test_machine_profile
	rm -rf test_machine_profile.dSYM

.PHONY: test clean
