#pragma once
#include "NVN.hpp" // NVN types

// fsrSwitch: post-processing (sharpening, later anti-aliasing) of the finished frame, right before present.
// Built into SaltyNX Core next to the logo renderer, works the same way: own NVN queue, offline compiled
// shaders (UAM + tools/dksh2nvn.py), the game's frame is copied into a texture of ours and the result is
// drawn back into the frame.
namespace FsrNVN {
	// Returns the address of an NVN function (nvnDeviceGetProcAddress).
	typedef void* (*Resolver)(const char* name);

	// True when the effect is enabled for this game (config read in FsrSwitch_Init).
	bool Wanted();

	// Call right after the game's nvnDeviceInitialize (see LogoNVN::PrepareEarly for why).
	void PrepareEarly(NVN::Device* device, Resolver resolve);

	// Right before present: target holds the finished frame, crop is the visible area (zero size = whole texture).
	void Process(NVN::Device* device, const NVN::Queue* queue, const NVN::Texture* target, const NVN::Rectangle& crop, Resolver resolve);
}

extern "C" {
	// Called once from Core's main (after NX_FPS): reads config, reserves memory for the frame copy.
	void FsrSwitch_Init(void);
	// Added to Core's main.c: reserves more heap after whatever was reserved before. Returns 0 on failure.
	uintptr_t SaltySDCore_ReserveMemoryAppend(size_t size);
}
