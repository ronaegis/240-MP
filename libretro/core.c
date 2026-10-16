/*
 * 240mp_libretro - ReplayOS core that runs 240-MP on the CRT.
 *
 * ReplayOS owns the display, audio and controllers and only runs libretro
 * cores, so 240-MP runs as a child process of this core and renders
 * off-screen; the two talk over a socket and shared memory (bridge_proto.h).
 * Qt is never loaded into the frontend: ReplayOS dlclose()s a core on every
 * exit, and a crash in 240-MP must not take the frontend down.
 *
 * This core is the clock master: each retro_run sends one TICK with the
 * input, shows the newest frame the child has finished and plays exactly one
 * frame's worth of audio from the ring.
 *
 * The content file picks what to run: empty (the usual "240-MP.lr") starts
 * MP240_DEFAULT_CHILD; otherwise its first line is the program to start
 * (used to run libretro/test_child).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "libretro.h"
#include "bridge_proto.h"

#define MP240_DEFAULT_CHILD "/opt/240mp/bin/240mp-bridge"
#define CHILD_LOG           "/tmp/240mp.log"
#define CORE_LOG            "/tmp/240mp_core.log"
#define CHILD_FD_SOCK       3
#define CHILD_FD_SHM        4
#define QUIT_TIMEOUT_MS     3000
#define FPS                 59.94
#define MAX_AUDIO_PER_RUN   2048

extern char **environ;

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static bool use_bitmasks;

static pid_t child = -1;
static int sock = -1;
static mp240_shm *shm;
static uint32_t front = 0;     /* triple-buffer index owned by the core */
static bool have_frame;
static bool child_gone;
static uint32_t frame_count;
static double audio_acc;
static uint32_t *black;

typedef struct { const char *value; unsigned width, height; bool interlaced; } video_mode;
static const video_mode modes[] = {
   { "640x480i", 640, 480, true },
   { "320x240p", 320, 240, false },
};
static const video_mode *mode = &modes[0];

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* ReplayOS keeps only warnings and errors, so everything also goes to CORE_LOG. */
static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   char line[512];
   va_list va;
   FILE *f;
   va_start(va, fmt);
   vsnprintf(line, sizeof line, fmt, va);
   va_end(va);
   log_cb(level, "[240mp] %s\n", line);
   if ((f = fopen(CORE_LOG, "a"))) {
      fprintf(f, "%s\n", line);
      fclose(f);
   }
}

static void send_msg(const mp240_msg *m)
{
   if (sock < 0)
      return;
   /* Never block the frontend: a stalled child just misses ticks. */
   if (send(sock, m, sizeof *m, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN)
      child_gone = true;
}

static void send_mode(void)
{
   mp240_msg m = { .type = MP240_MSG_MODE };
   m.u.mode.width = mode->width;
   m.u.mode.height = mode->height;
   m.u.mode.fps = (float)FPS;
   m.u.mode.interlaced = mode->interlaced;
   send_msg(&m);
}

static void read_mode_option(void)
{
   struct retro_variable var = { "240mp_video_mode", NULL };
   unsigned i;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value)
      return;
   for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
      if (!strcmp(var.value, modes[i].value))
         mode = &modes[i];
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
   mp240_msg m = { .type = MP240_MSG_KEY };
   m.u.key.down = down;
   m.u.key.keycode = keycode;
   m.u.key.character = character;
   m.u.key.modifiers = key_modifiers;
   send_msg(&m);
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "240mp_video_mode", "Video mode; 640x480i|320x240p" },
      { NULL, NULL },
   };
   bool no_game = true;
   struct retro_log_callback logging;

   environ_cb = cb;
   log_cb = fallback_log;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log)
      log_cb = logging.log;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_init(void)
{
   use_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

void retro_deinit(void) {}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof *info);
   info->library_name = "240-MP";
   info->library_version = "1.0";
   info->valid_extensions = "lr|240mp";
   info->need_fullpath = true;
   info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof *info);
   info->geometry.base_width = mode->width;
   info->geometry.base_height = mode->height;
   info->geometry.max_width = MP240_MAX_WIDTH;
   info->geometry.max_height = MP240_MAX_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps = FPS;
   info->timing.sample_rate = MP240_AUDIO_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) {}

/* First line of the content file, or the default launcher if it is empty. */
static void child_path(const char *content, char *out, size_t size)
{
   FILE *f = content ? fopen(content, "r") : NULL;
   out[0] = '\0';
   if (f) {
      if (fgets(out, (int)size, f))
         out[strcspn(out, "\r\n")] = '\0';
      fclose(f);
   }
   if (!out[0])
      snprintf(out, size, "%s", MP240_DEFAULT_CHILD);
}

static bool spawn_child(const char *path, int child_sock, int shm_fd)
{
   posix_spawn_file_actions_t fa;
   posix_spawnattr_t attr;
   sigset_t none, all;
   char sock_env[32], shm_env[32];
   char *argv[] = { (char *)path, NULL };
   char **envp;
   size_t n = 0, i;
   int err;

   /* Move both above 3 and 4 first, so dup2 onto one cannot clobber the other. */
   child_sock = fcntl(child_sock, F_DUPFD_CLOEXEC, 100);
   shm_fd = fcntl(shm_fd, F_DUPFD_CLOEXEC, 100);
   if (child_sock < 0 || shm_fd < 0) {
      if (child_sock >= 0) close(child_sock);
      if (shm_fd >= 0) close(shm_fd);
      return false;
   }

   /* The frontend's environment plus the two descriptor numbers. */
   while (environ[n])
      n++;
   envp = calloc(n + 3, sizeof *envp);
   if (!envp) {
      close(child_sock);
      close(shm_fd);
      return false;
   }
   for (i = 0; i < n; i++)
      envp[i] = environ[i];
   snprintf(sock_env, sizeof sock_env, "MP240_BRIDGE_SOCK=%d", CHILD_FD_SOCK);
   snprintf(shm_env, sizeof shm_env, "MP240_BRIDGE_SHM=%d", CHILD_FD_SHM);
   envp[n] = sock_env;
   envp[n + 1] = shm_env;

   /* Only the bridge descriptors and a log file reach the child: an inherited
    * DRM or ALSA descriptor would keep the frontend's devices open. Both fds
    * are close-on-exec, so dup2 onto 3 and 4 is what keeps them. */
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_addopen(&fa, 1, CHILD_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   posix_spawn_file_actions_adddup2(&fa, 1, 2);
   posix_spawn_file_actions_adddup2(&fa, child_sock, CHILD_FD_SOCK);
   posix_spawn_file_actions_adddup2(&fa, shm_fd, CHILD_FD_SHM);
   posix_spawn_file_actions_addclosefrom_np(&fa, CHILD_FD_SHM + 1);

   /* The frontend may block or ignore signals; the child starts clean. */
   sigemptyset(&none);
   sigfillset(&all);
   posix_spawnattr_init(&attr);
   posix_spawnattr_setsigmask(&attr, &none);
   posix_spawnattr_setsigdefault(&attr, &all);
   posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

   err = posix_spawn(&child, path, &fa, &attr, argv, envp);
   posix_spawn_file_actions_destroy(&fa);
   posix_spawnattr_destroy(&attr);
   free(envp);
   close(child_sock);
   close(shm_fd);
   if (err) {
      core_log(RETRO_LOG_ERROR, "cannot start %s: %s", path, strerror(err));
      child = -1;
      return false;
   }
   core_log(RETRO_LOG_INFO, "started %s (pid %d)", path, (int)child);
   return true;
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_keyboard_callback kb = { keyboard_cb };
   char path[1024];
   int pair[2] = { -1, -1 }, shm_fd = -1;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
      core_log(RETRO_LOG_ERROR, "XRGB8888 not supported");
      return false;
   }
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
   read_mode_option();

   shm_fd = memfd_create("240mp-bridge", MFD_CLOEXEC);
   if (shm_fd < 0 || ftruncate(shm_fd, MP240_SHM_SIZE) < 0)
      goto fail;
   shm = mmap(NULL, MP240_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
   if (shm == MAP_FAILED) {
      shm = NULL;
      goto fail;
   }
   shm->magic = MP240_BRIDGE_MAGIC;
   shm->version = MP240_BRIDGE_VERSION;
   shm->middle = 1;  /* core owns 0, child starts with 2 */
   front = 0;

   if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
      goto fail;
   child_path(game ? game->path : NULL, path, sizeof path);
   if (!spawn_child(path, pair[1], shm_fd))
      goto fail;
   close(pair[1]);
   close(shm_fd);
   sock = pair[0];

   black = calloc((size_t)MP240_MAX_WIDTH * MP240_MAX_HEIGHT, sizeof *black);
   have_frame = child_gone = false;
   frame_count = 0;
   audio_acc = 0;
   send_mode();
   return true;

fail:
   core_log(RETRO_LOG_ERROR, "bridge setup failed: %s", strerror(errno));
   if (pair[0] >= 0) close(pair[0]);
   if (pair[1] >= 0) close(pair[1]);
   if (shm_fd >= 0) close(shm_fd);
   if (shm) munmap(shm, MP240_SHM_SIZE);
   shm = NULL;
   return false;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type; (void)info; (void)num;
   return false;
}

static bool reap_child(int timeout_ms)
{
   struct timespec ts = { 0, 10 * 1000000 };
   int waited;
   for (waited = 0; waited <= timeout_ms; waited += 10) {
      if (waitpid(child, NULL, WNOHANG) == child)
         return true;
      nanosleep(&ts, NULL);
   }
   return false;
}

void retro_unload_game(void)
{
   if (child > 0) {
      mp240_msg m = { .type = MP240_MSG_QUIT };
      send_msg(&m);
      if (!reap_child(QUIT_TIMEOUT_MS)) {
         core_log(RETRO_LOG_WARN, "240-MP did not exit, killing it");
         kill(child, SIGKILL);
         waitpid(child, NULL, 0);
      }
      child = -1;
   }
   if (sock >= 0)
      close(sock);
   sock = -1;
   if (shm)
      munmap(shm, MP240_SHM_SIZE);
   shm = NULL;
   free(black);
   black = NULL;
}

static void check_child(void)
{
   char buf[sizeof(mp240_msg)];
   ssize_t r = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
   if (r == 0 || (r < 0 && errno != EAGAIN))
      child_gone = true;
   if (child_gone) {
      /* 240-MP quit (or crashed): go back to the ReplayOS menu. */
      core_log(RETRO_LOG_INFO, "240-MP exited");
      close(sock);
      sock = -1;
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
   }
}

static void send_tick(void)
{
   mp240_msg m = { .type = MP240_MSG_TICK };
   unsigned port, id;
   m.u.tick.frame = frame_count;
   for (port = 0; port < MP240_PORTS; port++) {
      uint16_t bits = 0;
      if (use_bitmasks)
         bits = (uint16_t)input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      else
         for (id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; id++)
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id))
               bits |= (uint16_t)(1u << id);
      m.u.tick.joypad[port] = bits;
      m.u.tick.analog[port][0] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      m.u.tick.analog[port][1] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      m.u.tick.analog[port][2] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
      m.u.tick.analog[port][3] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
   }
   send_msg(&m);
}

static void present_video(void)
{
   const mp240_frame_info *f;
   bool fresh = false;

   if (mp240_load(&shm->middle) & MP240_FRESH) {
      front = mp240_exchange(&shm->middle, front) & 3u;
      have_frame = fresh = true;
   }
   if (!have_frame) {
      video_cb(black, mode->width, mode->height, mode->width * 4);
      return;
   }
   f = &shm->frame[front];
   if (f->width == 0 || f->width > MP240_MAX_WIDTH || f->height == 0 ||
       f->height > MP240_MAX_HEIGHT || f->pitch < f->width * 4 ||
       (size_t)f->pitch * f->height > MP240_FRAME_BYTES) {
      video_cb(black, mode->width, mode->height, mode->width * 4);
      return;
   }
   /* An unchanged frame is a dupe: the frontend shows the last one again. */
   video_cb(fresh ? (const uint8_t *)shm + MP240_FRAME_OFFSET(front) : NULL,
            f->width, f->height, f->pitch);
}

/* Exactly one CRT frame of audio per run, padded with silence: consuming at
 * a constant rate is what paces mpv's playback to the CRT clock. */
static void play_audio(void)
{
   static int16_t buf[MAX_AUDIO_PER_RUN * 2];
   uint32_t w = mp240_load(&shm->audio_write);
   uint32_t r = shm->audio_read;
   uint32_t avail = w - r, take, i;
   unsigned n;

   audio_acc += MP240_AUDIO_RATE / FPS;
   n = (unsigned)audio_acc;
   audio_acc -= n;
   if (n > MAX_AUDIO_PER_RUN)
      n = MAX_AUDIO_PER_RUN;
   if (avail > MP240_AUDIO_FRAMES)  /* corrupt counters: resync */
      avail = 0, r = w;
   take = avail < n ? avail : n;
   for (i = 0; i < take; i++) {
      uint32_t s = (r + i) & (MP240_AUDIO_FRAMES - 1);
      buf[i * 2] = shm->audio[s * 2];
      buf[i * 2 + 1] = shm->audio[s * 2 + 1];
   }
   memset(buf + take * 2, 0, (n - take) * 2 * sizeof *buf);
   mp240_store(&shm->audio_read, r + take);
   audio_batch_cb(buf, n);
}

void retro_run(void)
{
   bool updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      const video_mode *old = mode;
      read_mode_option();
      if (mode != old) {
         struct retro_system_av_info av;
         retro_get_system_av_info(&av);
         environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
         send_mode();
      }
   }

   input_poll_cb();
   if (sock >= 0) {
      send_tick();
      check_child();
   }
   present_video();
   play_audio();
   frame_count++;
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
