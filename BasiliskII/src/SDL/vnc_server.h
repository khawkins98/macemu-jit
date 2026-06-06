#pragma once

#include "my_sdl.h"

// Sentinel placed in SDL event's `which` field to mark VNC-injected input.
// video_sdl3.cpp uses this to pass VNC events through the host-input lockout filter.
// Value 'VNC\0' (0x564E4300) is far outside the range of real SDL device instance IDs.
#define VNC_SYNTHETIC_INPUT_ID ((SDL_MouseID)0x564E4300u)

void VNCServerInitFromPrefs();
void VNCServerShutdown();
void VNCServerUpdate(SDL_Surface *surface, const SDL_Rect &updated_rect);
void VNCServerProcessEvents();
