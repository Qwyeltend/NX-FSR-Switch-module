#pragma once
#include <stdint.h>

// Block in SaltyNX's shared memory (0x1000 bytes, also used by NX-FPS / ReverseNX).
// SaltyNX Core reserves it at game start; the overlay finds it by its magic (4-byte aligned search)
// and checks the title ID. All fields are 32-bit so the block works at any 4-byte aligned offset.

#define FSR_SHARED_MAGIC   0x58525346u  /* "FSRX" */
#define FSR_SHARED_VERSION 2u

enum FsrFlip {
    FSR_FLIP_AUTO = 0,   // follow the game's NVN window origin mode
    FSR_FLIP_OFF = 1,
    FSR_FLIP_ON = 2,
};

enum FsrState {
    FSR_STATE_WAITING = 0,     // waiting for the first NVN frame
    FSR_STATE_RUNNING = 1,
    FSR_STATE_FAILED = 2,      // init failed, see sdmc:/config/fsrSwitch/log_<titleid>.txt
    FSR_STATE_NO_MEMORY = 3,   // effect was off at game start: restart the game to use it
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t titleIdLow;
    uint32_t titleIdHigh;

    // Control: written by the overlay (initial values from the .ini), read by Core every frame.
    uint32_t enabled;      // 0/1
    uint32_t sharpness;    // 0..1000
    uint32_t split;        // 0/1, effect only on the left half
    uint32_t flip;         // FsrFlip
    uint32_t antialias;    // 0..1000, 0 = off

    // Status: written by Core.
    uint32_t state;        // FsrState
    uint32_t width;        // frame texture size
    uint32_t height;
    uint32_t format;       // NVN format of the frame
    uint32_t originMode;   // NVN window origin mode (0 lower left, 1 upper left), 0xFFFFFFFF unknown
    uint32_t flipActive;   // flip actually used
    uint32_t gpuTimeNs;    // GPU time of the pass, average of the last measurements
    uint32_t frames;       // processed frames
} FsrSharedBlock;
