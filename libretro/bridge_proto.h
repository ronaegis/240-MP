/*
 * Link between the ReplayOS core (libretro/core.c, runs inside the ReplayOS
 * frontend) and the 240-MP process it spawns (src/bridge/, or the test
 * pattern child libretro/test_child.c).
 *
 * The core creates both channels and passes them to the child as inherited
 * file descriptors, named by MP240_BRIDGE_SOCK and MP240_BRIDGE_SHM:
 *
 *  - a SOCK_SEQPACKET socket carrying fixed-size mp240_msg packets. The core
 *    sends MODE once at start (and on every mode change), then one TICK per
 *    retro_run carrying the input state; the child renders a frame per TICK.
 *    EOF on either side means the other process is gone.
 *  - a shared memory block (mp240_shm, then three frames of pixels):
 *      video: triple buffer. The child renders into its back buffer, then
 *             exchanges it with `middle` (setting MP240_FRESH); the core
 *             exchanges its front buffer with `middle` when MP240_FRESH is
 *             set. Neither side ever waits for the other.
 *      audio: single producer / single consumer ring of 48 kHz s16 stereo
 *             frames. The core drains exactly one video frame's worth per
 *             retro_run, so a child that only writes when there is room is
 *             paced by the CRT's clock.
 *
 * Shared by C and C++: atomics use the GCC/Clang __atomic builtins.
 */
#ifndef MP240_BRIDGE_PROTO_H
#define MP240_BRIDGE_PROTO_H

#include <stdint.h>

#define MP240_BRIDGE_MAGIC   0x42303432u /* "240B" */
#define MP240_BRIDGE_VERSION 1

#define MP240_MAX_WIDTH      720
#define MP240_MAX_HEIGHT     576
#define MP240_FRAME_BYTES    (MP240_MAX_WIDTH * MP240_MAX_HEIGHT * 4)

#define MP240_AUDIO_RATE     48000
#define MP240_AUDIO_FRAMES   8192 /* ring capacity in stereo frames, power of two */

#define MP240_FRESH          4u   /* `middle` flag: holds a frame the core has not taken */

/* Pixels are XRGB8888 as libretro defines it: one native-endian uint32
 * 0x00RRGGBB per pixel (bytes B, G, R, X on aarch64), rows top to bottom. */
typedef struct {
   uint32_t width, height, pitch;
   uint32_t seq;
} mp240_frame_info;

typedef struct {
   uint32_t magic, version;
   uint32_t middle;                 /* buffer index 0-2 | MP240_FRESH */
   mp240_frame_info frame[3];
   uint32_t audio_write;            /* frames written (wraps), owned by the child */
   uint32_t audio_read;             /* frames read (wraps), owned by the core */
   int16_t  audio[MP240_AUDIO_FRAMES * 2];
} mp240_shm;

/* Pixel data of buffer i, 64-byte aligned after the header. */
#define MP240_FRAME_OFFSET(i) \
   ((((uint32_t)sizeof(mp240_shm) + 63u) & ~63u) + (uint32_t)(i) * MP240_FRAME_BYTES)
#define MP240_SHM_SIZE MP240_FRAME_OFFSET(3)

enum {
   /* core -> child */
   MP240_MSG_MODE = 1,  /* mode: render frames of this size from now on */
   MP240_MSG_TICK = 2,  /* tick: one CRT frame elapsed, current input */
   MP240_MSG_KEY  = 3,  /* key: keyboard event from the frontend */
   MP240_MSG_QUIT = 4   /* exit cleanly (the core is unloading) */
};

/* RetroPad buttons, in RETRO_DEVICE_ID_JOYPAD_* bit order. */
#define MP240_PORTS 4

typedef struct {
   uint32_t type;
   union {
      struct { uint32_t width, height; float fps; uint32_t interlaced; } mode;
      struct { uint32_t frame; uint16_t joypad[MP240_PORTS]; int16_t analog[MP240_PORTS][4]; } tick;
      struct { uint32_t down, keycode, character; uint16_t modifiers; } key;
   } u;
} mp240_msg;

static inline uint32_t mp240_load(const uint32_t *p)
{
   return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void mp240_store(uint32_t *p, uint32_t v)
{
   __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline uint32_t mp240_exchange(uint32_t *p, uint32_t v)
{
   return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}

#endif
