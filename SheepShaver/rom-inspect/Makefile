# ROM Inspector — standalone Mac ROM validator for SheepShaver
#
# Reports whether DecodeROM()/PatchROM() would accept a ROM file, without
# launching the emulator. Shares the decode + type-detection code with the
# emulator via src/include/rom_decode.hpp.

CXX      = g++
CXXFLAGS = -g -O2 -std=c++11 -Wall -Wno-unused-function
INCLUDES = -I../src/include

ROM      ?=

SRCS     = rom-inspect.cpp

.PHONY: all clean run

all: rom-inspect

rom-inspect: rom-inspect.cpp ../src/include/rom_decode.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ rom-inspect.cpp

run: rom-inspect
	./rom-inspect $(ROM)

clean:
	rm -f rom-inspect
