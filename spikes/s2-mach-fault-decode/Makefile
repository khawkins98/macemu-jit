# Spike S2 — Mach fault-decode keystone proof. macOS arm64 only.
# mach_exc_server comes from MIG (same mechanism the OS uses; sigsegv.cpp
# relies on the equivalent symbol). MIG generates mach_excServer.{c,h} from
# the SDK's mach_exc.defs.

SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
CXX     := clang++
CC      := clang
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra
TARGET  := s2-fault-decode

all: $(TARGET)

mach_excServer.c mach_excServer.h: $(SDKROOT)/usr/include/mach/mach_exc.defs
	mig -arch arm64 $<

mach_excServer.o: mach_excServer.c mach_excServer.h
	$(CC) -O2 -c -o $@ mach_excServer.c

main.o: main.cpp mach_excServer.h
	$(CXX) $(CXXFLAGS) -c -o $@ main.cpp

$(TARGET): main.o mach_excServer.o
	$(CXX) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o mach_excServer.* mach_excUser.c mach_exc.h

.PHONY: all run clean
