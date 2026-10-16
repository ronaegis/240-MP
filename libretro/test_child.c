/*
 * Test pattern child for the 240mp_libretro core: speaks bridge_proto.h
 * without Qt, so the core can be checked on its own. Put this program's path
 * in a .lr content file to run it instead of 240-MP.
 *
 * Shows moving colour bars and one box per RetroPad button (lit while held),
 * plays a 440 Hz tone, logs input to stderr (/tmp/240mp.log) and quits when
 * L and R are held together, which sends ReplayOS back to its menu.
 */
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bridge_proto.h"

#define BUTTON_L 10
#define BUTTON_R 11

static mp240_shm *shm;
static uint32_t back = 2, width, height;
static int stream = -1;
static double phase;

static void fill(uint32_t *px, uint32_t pitch_px, uint32_t x0, uint32_t y0,
                 uint32_t w, uint32_t h, uint32_t color)
{
   uint32_t x, y;
   for (y = y0; y < y0 + h && y < height; y++)
      for (x = x0; x < x0 + w && x < width; x++)
         px[y * pitch_px + x] = color;
}

static void render(uint32_t frame, uint16_t joypad)
{
   static const uint32_t bars[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                    0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
   uint32_t *px = (uint32_t *)((uint8_t *)shm + MP240_FRAME_OFFSET(MP240_LAYER_UI, back));
   uint32_t x, y, b, box = width / 20;

   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         px[y * width + x] = bars[((x + frame) * 8 / width) % 8];
   for (b = 0; b < 16; b++)
      fill(px, width, box + b * (box + box / 4), height - 2 * box, box, box,
           (joypad & (1u << b)) ? 0xff8000 : 0x404040);
   fill(px, width, frame % width, height / 3, 4, height / 3, 0x000000);

   shm->layer[MP240_LAYER_UI].frame[back].width = width;
   shm->layer[MP240_LAYER_UI].frame[back].height = height;
   shm->layer[MP240_LAYER_UI].frame[back].pitch = width * 4;
   shm->layer[MP240_LAYER_UI].frame[back].seq = frame;
   back = mp240_publish(shm, MP240_LAYER_UI, back);
}

/* Keep about 50 ms queued; the core's constant drain paces us. */
static void feed_audio(void)
{
   int16_t pcm[2];
   if (stream < 0 && (stream = mp240_audio_claim(shm)) < 0)
      return;
   while (mp240_audio_queued(shm, stream) < MP240_AUDIO_RATE / 20) {
      pcm[0] = pcm[1] = (int16_t)(sin(phase) * 6000);
      phase += 2 * M_PI * 440 / MP240_AUDIO_RATE;
      mp240_audio_write(shm, stream, pcm, 1);
   }
}

int main(void)
{
   const char *s = getenv("MP240_BRIDGE_SOCK"), *m = getenv("MP240_BRIDGE_SHM");
   int sock, shm_fd;
   uint16_t last = 0;
   mp240_msg msg;

   if (!s || !m) {
      fprintf(stderr, "test_child: run me from the 240mp_libretro core\n");
      return 1;
   }
   sock = atoi(s);
   shm_fd = atoi(m);
   shm = mmap(NULL, MP240_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
   if (shm == MAP_FAILED || shm->magic != MP240_BRIDGE_MAGIC || shm->version != MP240_BRIDGE_VERSION) {
      fprintf(stderr, "test_child: bad shared memory\n");
      return 1;
   }
   setvbuf(stderr, NULL, _IOLBF, 0);
   fprintf(stderr, "test_child: started\n");

   for (;;) {
      ssize_t r = recv(sock, &msg, sizeof msg, 0);
      if (r <= 0) {
         fprintf(stderr, "test_child: core gone\n");
         return 0;
      }
      switch (msg.type) {
      case MP240_MSG_MODE:
         width = msg.u.mode.width;
         height = msg.u.mode.height;
         fprintf(stderr, "test_child: mode %ux%u%s @ %.2f\n", width, height,
                 msg.u.mode.interlaced ? "i" : "p", msg.u.mode.fps);
         break;
      case MP240_MSG_TICK:
         if (msg.u.tick.joypad[0] != last)
            fprintf(stderr, "test_child: frame %u joypad %04x\n", msg.u.tick.frame, msg.u.tick.joypad[0]);
         last = msg.u.tick.joypad[0];
         if ((last & (1u << BUTTON_L)) && (last & (1u << BUTTON_R))) {
            fprintf(stderr, "test_child: L+R, quitting\n");
            return 0;
         }
         if (width)
            render(msg.u.tick.frame, last);
         feed_audio();
         break;
      case MP240_MSG_KEY:
         fprintf(stderr, "test_child: key %u %s\n", msg.u.key.keycode, msg.u.key.down ? "down" : "up");
         break;
      case MP240_MSG_QUIT:
         fprintf(stderr, "test_child: quit\n");
         return 0;
      }
   }
}
