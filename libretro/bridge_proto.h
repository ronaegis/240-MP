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
 *  - a shared memory block (mp240_shm, then the pixels of both layers). 240-MP
 *    keeps its descriptor inheritable, so the processes it starts (the mpv
 *    stand-in, libretro/mpv_bridge.c) can map it too.
 *      layers: the UI layer (240-MP) and the video layer (mpv), each a triple
 *              buffer. The producer renders into its back buffer, then
 *              exchanges it with `middle` (setting MP240_FRESH); the core
 *              exchanges its front buffer with `middle` when MP240_FRESH is
 *              set. Neither side ever waits for the other. The core shows the
 *              video layer while `video_owner` is a live process.
 *      audio:  MP240_AUDIO_STREAMS single producer / single consumer rings of
 *              48 kHz s16 stereo frames, mixed by the core. A producer claims
 *              a ring by storing its pid in `owner`. The core drains exactly
 *              one video frame's worth per retro_run, so a producer that only
 *              writes when there is room is paced by the CRT's clock.
 *
 * Shared by C and C++: atomics use the GCC/Clang __atomic builtins.
 */
#ifndef MP240_BRIDGE_PROTO_H
#define MP240_BRIDGE_PROTO_H

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#define MP240_BRIDGE_MAGIC   0x42303432u /* "240B" */
#define MP240_BRIDGE_VERSION 2

#define MP240_MAX_WIDTH      720
#define MP240_MAX_HEIGHT     576
#define MP240_FRAME_BYTES    (MP240_MAX_WIDTH * MP240_MAX_HEIGHT * 4)

#define MP240_AUDIO_RATE     48000
#define MP240_AUDIO_FRAMES   8192 /* ring capacity in stereo frames, power of two */
#define MP240_AUDIO_STREAMS  4

#define MP240_FRESH          4u   /* `middle` flag: holds a frame the core has not taken */

enum { MP240_LAYER_UI = 0, MP240_LAYER_VIDEO = 1 };

/* Pixels are XRGB8888 as libretro defines it: one native-endian uint32
 * 0x00RRGGBB per pixel (bytes B, G, R, X on aarch64), rows top to bottom. */
typedef struct {
   uint32_t width, height, pitch;
   uint32_t seq;
} mp240_frame_info;

typedef struct {
   uint32_t middle;                 /* buffer index 0-2 | MP240_FRESH */
   mp240_frame_info frame[3];
} mp240_layer;

typedef struct {
   uint32_t owner;                  /* producer pid, 0 = free */
   uint32_t write;                  /* frames written (wraps), owned by the producer */
   uint32_t read;                   /* frames read (wraps), owned by the core */
   int16_t  data[MP240_AUDIO_FRAMES * 2];
} mp240_audio_stream;

typedef struct {
   uint32_t magic, version;
   uint32_t mode_width, mode_height; /* current video mode, written by the core */
   mp240_layer layer[2];
   uint32_t video_owner;            /* pid of the video layer's producer, 0 = none */
   uint32_t video_front;            /* the core's buffer of the video layer, see mp240_video_back() */
   mp240_audio_stream audio[MP240_AUDIO_STREAMS];
} mp240_shm;

/* Pixel data of buffer i of a layer, 64-byte aligned after the header. */
#define MP240_FRAME_OFFSET(layer, i) \
   ((((uint32_t)sizeof(mp240_shm) + 63u) & ~63u) + ((uint32_t)(layer) * 3u + (uint32_t)(i)) * MP240_FRAME_BYTES)
#define MP240_SHM_SIZE MP240_FRAME_OFFSET(2, 0)

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

/* A pid slot is free when empty or when its process is gone (crashed). */
static inline int mp240_pid_alive(uint32_t pid)
{
   return pid != 0 && (kill((pid_t)pid, 0) == 0 || errno != ESRCH);
}

/* Takes a free audio stream for this process; -1 when all are in use. */
static inline int mp240_audio_claim(mp240_shm *shm)
{
   uint32_t self = (uint32_t)getpid();
   int i;
   for (i = 0; i < MP240_AUDIO_STREAMS; i++) {
      mp240_audio_stream *s = &shm->audio[i];
      uint32_t owner = mp240_load(&s->owner);
      if (mp240_pid_alive(owner))
         continue;
      if (__atomic_compare_exchange_n(&s->owner, &owner, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
         /* Start empty: whatever a dead owner left is dropped. */
         mp240_store(&s->write, mp240_load(&s->read));
         return i;
      }
   }
   return -1;
}

static inline void mp240_audio_release(mp240_shm *shm, int stream)
{
   if (stream >= 0)
      mp240_store(&shm->audio[stream].owner, 0);
}

/* Frames queued and not yet played. */
static inline uint32_t mp240_audio_queued(mp240_shm *shm, int stream)
{
   mp240_audio_stream *s = &shm->audio[stream];
   return s->write - mp240_load(&s->read);
}

/* Appends up to `frames` stereo frames; returns how many fit. */
static inline uint32_t mp240_audio_write(mp240_shm *shm, int stream, const int16_t *pcm, uint32_t frames)
{
   mp240_audio_stream *s = &shm->audio[stream];
   uint32_t w = s->write, room = MP240_AUDIO_FRAMES - (w - mp240_load(&s->read)), i;
   if (frames > room)
      frames = room;
   for (i = 0; i < frames; i++) {
      uint32_t slot = (w + i) & (MP240_AUDIO_FRAMES - 1);
      s->data[slot * 2] = pcm[i * 2];
      s->data[slot * 2 + 1] = pcm[i * 2 + 1];
   }
   mp240_store(&s->write, w + frames);
   return frames;
}

/* The video layer's free buffer for a new producer: the one that is neither
 * the core's front nor `middle`. While no producer runs only the core moves
 * buffers, and a take swaps exactly those two, so a pair read on either side of
 * a swap shows up as equal values and is read again. */
static inline uint32_t mp240_video_back(mp240_shm *shm)
{
   for (;;) {
      uint32_t f = mp240_load(&shm->video_front) & 3u;
      uint32_t m = mp240_load(&shm->layer[MP240_LAYER_VIDEO].middle) & 3u;
      if (f != m && f < 3 && m < 3 && f == (mp240_load(&shm->video_front) & 3u))
         return 3u - f - m;
   }
}

/* Publishes the back buffer of a layer and returns the new back buffer. */
static inline uint32_t mp240_publish(mp240_shm *shm, int layer, uint32_t back)
{
   return mp240_exchange(&shm->layer[layer].middle, back | MP240_FRESH) & 3u;
}

#endif
