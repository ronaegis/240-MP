/*
 * mpv stand-in for 240-MP under ReplayOS.
 *
 * Under ReplayOS the frontend owns the display and the sound card, so the mpv
 * program can't show anything. This program takes mpv's place: the bundle
 * installs it as bin/mpv next to 240mp, where mpvbin::locate() finds it first,
 * so MpvController, AmbientModeBackend and WeatherBackend start it with the
 * same command line and talk to it over the same IPC socket as real mpv.
 *
 * It runs libmpv and, through the shared memory 240-MP inherited from the
 * ReplayOS core (libretro/bridge_proto.h):
 *  - renders video with the libmpv OpenGL render API on a headless EGL
 *    context (GPU render node, hardware decoding) at the CRT mode's size and
 *    publishes it to the video layer, which the core shows instead of the UI;
 *  - sends audio through mpv's pcm output into a FIFO, and from there into an
 *    audio stream that the core drains at the CRT's rate. Writing only when
 *    the stream has room is what paces playback to the CRT clock.
 *
 * Command line: what mpv accepts, in the forms 240-MP uses: files and URLs,
 * "--", --name=value, --name, --no-name, and the list shorthands only the mpv
 * program understands (--script, --sub-file, --<list>-append), which are
 * collected and set as whole lists before mpv starts. Options that choose or place a window
 * (--vo, --fullscreen, --fs-screen, ...) are ignored: output always goes to
 * the bridge. Exit codes follow mpv: 0 played, 2 nothing could be played,
 * 3 some files failed, 4 quit by a signal, 1 bad options or setup failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <gbm.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include "bridge_proto.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

/* Audio queued in the core's stream before we wait: about two CRT frames.
 * mpv takes the pcm output's writes as played, so this is most of the delay
 * between picture and sound. */
#define AUDIO_TARGET_FRAMES 1600

/* No room for this long means nobody drains the stream (the core is paused
 * or gone): drop audio instead of blocking mpv, which would block quitting. */
#define AUDIO_STALL_NS (500 * 1000000LL)

/* After a signal, how long mpv gets to quit before we exit anyway. */
#define QUIT_GRACE_S 3

static mpv_handle *mpv;
static mp240_shm *shm;
static char fifo_path[64];
static volatile bool stopping;    /* quitting: drop audio instead of pacing it */
static volatile bool audio_exit;  /* mpv is destroyed: the FIFO reader may end */
static volatile bool got_signal;

/* ---- options ------------------------------------------------------------ */

static const char *const ignored_options[] = {
   "vo", "gpu-context", "gpu-api", "fullscreen", "fs", "native-fs",
   "fs-screen", "fs-screen-name", "drm-device", "drm-connector", "drm-mode",
   "ao", "ao-pcm-file", NULL,
};

static bool ignored(const char *name)
{
   const char *const *o;
   for (o = ignored_options; *o; o++)
      if (!strcmp(name, *o))
         return true;
   return false;
}

/* List options given piecewise on the command line. libmpv's
 * mpv_set_option_string() knows neither the mpv program's shorthands nor
 * "-append", and its string form of path lists splits on ':', which would cut
 * URLs apart, so each list is collected here and set as one node. */
#define MAX_LISTS       8
#define MAX_LIST_ITEMS 64

typedef struct {
   char name[64];
   bool is_map;             /* key-value list (script-opts) */
   int count;
   char *keys[MAX_LIST_ITEMS];
   char *values[MAX_LIST_ITEMS];
} list_option;

static list_option lists[MAX_LISTS];
static int list_count;

static const struct { const char *alias, *list; } list_aliases[] = {
   { "script", "scripts" },
   { "sub-file", "sub-files" },
   { "audio-file", "audio-files" },
   { "external-file", "external-files" },
   { NULL, NULL },
};

static list_option *list_named(const char *name, bool is_map)
{
   int i;
   for (i = 0; i < list_count; i++)
      if (!strcmp(lists[i].name, name))
         return &lists[i];
   if (list_count == MAX_LISTS)
      return NULL;
   snprintf(lists[list_count].name, sizeof lists[list_count].name, "%s", name);
   lists[list_count].is_map = is_map;
   return &lists[list_count++];
}

static bool list_add(list_option *l, const char *key, size_t key_len, const char *value)
{
   if (!l || l->count == MAX_LIST_ITEMS)
      return false;
   l->keys[l->count] = key ? strndup(key, key_len) : NULL;
   l->values[l->count] = strdup(value);
   l->count++;
   return true;
}

/* "k=v,k=v" into a key-value list (later keys win, as in mpv). */
static bool map_add_pairs(list_option *l, const char *pairs)
{
   const char *p = pairs;
   while (*p) {
      const char *end = strchr(p, ','), *eq;
      size_t len = end ? (size_t)(end - p) : strlen(p);
      char *item = strndup(p, len);
      bool ok;
      if (!(eq = strchr(item, '='))) {
         free(item);
         return false;
      }
      ok = list_add(l, item, (size_t)(eq - item), eq + 1);
      free(item);
      if (!ok)
         return false;
      p += len + (end ? 1 : 0);
   }
   return true;
}

/* Returns 1 if `name` was a list form and was collected, 0 if not, -1 on error. */
static int collect_list_option(const char *name, const char *value)
{
   char base[64];
   size_t n = strlen(name);
   int i;

   for (i = 0; list_aliases[i].alias; i++)
      if (!strcmp(name, list_aliases[i].alias))
         return list_add(list_named(list_aliases[i].list, false), NULL, 0, value) ? 1 : -1;
   if (!strcmp(name, "script-opts")) {
      list_option *l = list_named("script-opts", true);
      if (!l)
         return -1;
      l->count = 0;  /* a plain set replaces what came before */
      return map_add_pairs(l, value) ? 1 : -1;
   }
   if (n > 7 && !strcmp(name + n - 7, "-append") && n - 7 < sizeof base) {
      snprintf(base, sizeof base, "%.*s", (int)(n - 7), name);
      if (!strcmp(base, "script-opts"))
         return map_add_pairs(list_named(base, true), value) ? 1 : -1;
      return list_add(list_named(base, false), NULL, 0, value) ? 1 : -1;
   }
   return 0;
}

static bool set_lists(void)
{
   int i, j, err;
   for (i = 0; i < list_count; i++) {
      list_option *l = &lists[i];
      mpv_node items[MAX_LIST_ITEMS];
      mpv_node_list list = { l->count, items, l->is_map ? l->keys : NULL };
      mpv_node node = { .format = l->is_map ? MPV_FORMAT_NODE_MAP : MPV_FORMAT_NODE_ARRAY, .u.list = &list };
      for (j = 0; j < l->count; j++)
         items[j] = (mpv_node){ .format = MPV_FORMAT_STRING, .u.string = l->values[j] };
      if ((err = mpv_set_option(mpv, l->name, MPV_FORMAT_NODE, &node)) < 0) {
         fprintf(stderr, "mpv_bridge: --%s: %s\n", l->name, mpv_error_string(err));
         return false;
      }
   }
   return true;
}

/* "--name=value", "--name" (yes) or "--no-name" (no). */
static bool apply_option(const char *arg)
{
   char name[256];
   const char *value, *eq = strchr(arg + 2, '=');
   int err;

   if (eq) {
      snprintf(name, sizeof name, "%.*s", (int)(eq - arg - 2), arg + 2);
      value = eq + 1;
   } else if (!strncmp(arg + 2, "no-", 3)) {
      snprintf(name, sizeof name, "%s", arg + 5);
      value = "no";
   } else {
      snprintf(name, sizeof name, "%s", arg + 2);
      value = "yes";
   }
   if (ignored(name))
      return true;
   switch (collect_list_option(name, value)) {
   case 1:
      return true;
   case -1:
      fprintf(stderr, "mpv_bridge: %s: cannot add to the list\n", arg);
      return false;
   }
   err = mpv_set_option_string(mpv, name, value);
   if (err < 0) {
      fprintf(stderr, "mpv_bridge: %s: %s\n", arg, mpv_error_string(err));
      return false;
   }
   return true;
}

static void set_default(const char *name, const char *value)
{
   int err = mpv_set_option_string(mpv, name, value);
   if (err < 0)
      fprintf(stderr, "mpv_bridge: default %s=%s: %s\n", name, value, mpv_error_string(err));
}

/* ---- video: libmpv render API -> video layer ----------------------------- */

static pthread_t render_thread;
static bool render_running;
static pthread_mutex_t render_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_cond = PTHREAD_COND_INITIALIZER;
static bool render_update, render_stop;
static int render_ready;  /* 0 starting, 1 ready, -1 failed: files load after this */

static void set_render_ready(int state)
{
   pthread_mutex_lock(&render_lock);
   render_ready = state;
   pthread_cond_broadcast(&render_cond);
   pthread_mutex_unlock(&render_lock);
}

static void on_render_update(void *ctx)
{
   (void)ctx;
   pthread_mutex_lock(&render_lock);
   render_update = true;
   pthread_cond_signal(&render_cond);
   pthread_mutex_unlock(&render_lock);
}

static void *get_proc_address(void *ctx, const char *name)
{
   (void)ctx;
   return (void *)eglGetProcAddress(name);
}

/* A GLES 3 context without a window, on the first GPU render node. */
static bool egl_init(EGLDisplay *out_dpy, EGLContext *out_ctx)
{
   static const EGLint config_attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
   static const EGLint context_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   struct gbm_device *gbm = NULL;
   EGLDisplay dpy;
   EGLConfig config;
   EGLContext ctx;
   EGLint n;
   char path[32];
   int i, fd = -1;

   for (i = 128; i < 136 && fd < 0; i++) {
      snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
      fd = open(path, O_RDWR | O_CLOEXEC);
   }
   if (fd < 0 || !(gbm = gbm_create_device(fd)) || !get_display) {
      fprintf(stderr, "mpv_bridge: no GPU render node\n");
      return false;
   }
   dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, config_attrs, &config, 1, &n) || n < 1) {
      fprintf(stderr, "mpv_bridge: EGL setup failed (0x%x)\n", eglGetError());
      return false;
   }
   ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "mpv_bridge: no surfaceless GLES 3 context (0x%x)\n", eglGetError());
      return false;
   }
   *out_dpy = dpy;
   *out_ctx = ctx;
   return true;
}

static void *render_main(void *arg)
{
   mpv_render_context *rc = NULL;
   mpv_opengl_init_params gl_init = { get_proc_address, NULL };
   mpv_render_param create_params[] = {
      { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
      { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
      { MPV_RENDER_PARAM_INVALID, NULL },
   };
   EGLDisplay dpy;
   EGLContext ctx;
   GLuint tex = 0, fbo = 0;
   int fbo_w = 0, fbo_h = 0, err;
   uint32_t back;
   bool bgra;
   (void)arg;

   if (!egl_init(&dpy, &ctx)) {
      set_render_ready(-1);
      return NULL;
   }
   bgra = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_read_format_bgra") != NULL;
   if ((err = mpv_render_context_create(&rc, mpv, create_params)) < 0) {
      fprintf(stderr, "mpv_bridge: render context: %s\n", mpv_error_string(err));
      set_render_ready(-1);
      return NULL;
   }
   mpv_render_context_set_update_callback(rc, on_render_update, NULL);
   set_render_ready(1);
   back = mp240_video_back(shm);

   for (;;) {
      int w, h, flip = 0, block = 0;
      uint8_t *pixels;
      mp240_frame_info *info;

      pthread_mutex_lock(&render_lock);
      while (!render_update && !render_stop)
         pthread_cond_wait(&render_cond, &render_lock);
      render_update = false;
      if (render_stop) {
         pthread_mutex_unlock(&render_lock);
         break;
      }
      pthread_mutex_unlock(&render_lock);

      if (!(mpv_render_context_update(rc) & MPV_RENDER_UPDATE_FRAME))
         continue;

      /* Render at the CRT mode's size; mpv scales and letterboxes. */
      w = (int)mp240_load(&shm->mode_width);
      h = (int)mp240_load(&shm->mode_height);
      if (w <= 0 || w > MP240_MAX_WIDTH || h <= 0 || h > MP240_MAX_HEIGHT)
         w = 640, h = 480;
      if (w != fbo_w || h != fbo_h) {
         if (fbo) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
         }
         glGenTextures(1, &tex);
         glBindTexture(GL_TEXTURE_2D, tex);
         glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
         glGenFramebuffers(1, &fbo);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
         fbo_w = w;
         fbo_h = h;
      }

      {
         mpv_opengl_fbo target = { (int)fbo, w, h, 0 };
         /* Never wait for the frame's display time: the core's clock decides
          * when it is shown, and waiting here stalls every frame. */
         mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &target },
            { MPV_RENDER_PARAM_FLIP_Y, &flip },  /* top row first in glReadPixels */
            { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
            { MPV_RENDER_PARAM_INVALID, NULL },
         };
         mpv_render_context_render(rc, params);
      }

      pixels = (uint8_t *)shm + MP240_FRAME_OFFSET(MP240_LAYER_VIDEO, back);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glReadPixels(0, 0, w, h, bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      if (!bgra) {
         int i;
         for (i = 0; i < w * h; i++) {
            uint8_t t = pixels[i * 4];
            pixels[i * 4] = pixels[i * 4 + 2];
            pixels[i * 4 + 2] = t;
         }
      }
      info = &shm->layer[MP240_LAYER_VIDEO].frame[back];
      info->width = (uint32_t)w;
      info->height = (uint32_t)h;
      info->pitch = (uint32_t)w * 4;
      info->seq++;
      back = mp240_publish(shm, MP240_LAYER_VIDEO, back);
      mp240_store(&shm->video_owner, (uint32_t)getpid());
      mpv_render_context_report_swap(rc);
   }

   /* Back to the UI; the render context must go before mpv itself. */
   if (mp240_load(&shm->video_owner) == (uint32_t)getpid())
      mp240_store(&shm->video_owner, 0);
   mpv_render_context_free(rc);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   return NULL;
}

/* ---- audio: pcm FIFO -> audio stream ------------------------------------- */

static pthread_t audio_thread;
static bool audio_running;

static long long now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *audio_main(void *arg)
{
   static uint8_t buf[4096];
   size_t have = 0;
   int fd = (int)(intptr_t)arg, stream = -1;

   /* Runs until mpv is destroyed: mpv blocks writing a FIFO nobody reads,
    * and while it does it cannot quit. */
   while (!audio_exit) {
      struct pollfd p = { fd, POLLIN, 0 };
      ssize_t r;
      uint32_t frames, done = 0;

      if (poll(&p, 1, 100) <= 0)
         continue;
      r = read(fd, buf + have, sizeof buf - have);
      if (r <= 0) {
         /* No writer (between files, or mpv has no audio): nothing to do. */
         struct timespec ts = { 0, 10 * 1000000 };
         nanosleep(&ts, NULL);
         continue;
      }
      have += (size_t)r;
      if (stopping) {
         have = 0;
         continue;
      }
      frames = (uint32_t)(have / 4);
      if (stream < 0 && (stream = mp240_audio_claim(shm)) < 0) {
         fprintf(stderr, "mpv_bridge: every audio stream is in use, dropping audio\n");
         have = 0;
         continue;
      }
      /* Wait for room: holding mpv's writes back is what paces playback. */
      long long waiting_since = now_ns();
      while (done < frames && !stopping) {
         uint32_t queued = mp240_audio_queued(shm, stream);
         if (queued >= AUDIO_TARGET_FRAMES) {
            struct timespec ts = { 0, 2 * 1000000 };
            if (now_ns() - waiting_since > AUDIO_STALL_NS)
               break;  /* drop the rest of this chunk */
            nanosleep(&ts, NULL);
            continue;
         }
         waiting_since = now_ns();
         done += mp240_audio_write(shm, stream, (const int16_t *)buf + done * 2,
                                   frames - done < AUDIO_TARGET_FRAMES - queued
                                      ? frames - done : AUDIO_TARGET_FRAMES - queued);
      }
      memmove(buf, buf + frames * 4, have - frames * 4);
      have -= frames * 4;
   }
   mp240_audio_release(shm, stream);
   return NULL;
}

/* A FIFO for mpv's pcm output, opened for reading before mpv opens it for
 * writing (so mpv never blocks there), with a one-page pipe buffer to keep
 * the hidden delay small. */
static int open_fifo(void)
{
   int fd;
   snprintf(fifo_path, sizeof fifo_path, "/tmp/240mp-mpv-%d.pcm", (int)getpid());
   unlink(fifo_path);
   if (mkfifo(fifo_path, 0600) < 0 || (fd = open(fifo_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) {
      fprintf(stderr, "mpv_bridge: %s: %s\n", fifo_path, strerror(errno));
      return -1;
   }
   fcntl(fd, F_SETPIPE_SZ, 4096);
   return fd;
}

/* ---- signals -------------------------------------------------------------- */

static void *signal_main(void *arg)
{
   sigset_t *set = arg;
   int sig;
   if (sigwait(set, &sig) != 0)
      return NULL;
   got_signal = true;
   stopping = true;  /* the audio thread stops holding mpv back */
   {
      /* Async: a synchronous command would wait for the very core we are
       * trying to unstick, and the watchdog below would never run. */
      const char *quit[] = { "quit", NULL };
      mpv_command_async(mpv, 0, quit);
   }
   sleep(QUIT_GRACE_S);
   fprintf(stderr, "mpv_bridge: mpv did not quit, exiting\n");
   _exit(4);
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
   static sigset_t sigs;
   const char *shm_env = getenv("MP240_BRIDGE_SHM");
   const char **files;
   int nfiles = 0, i, afd, failed = 0, loaded = 0, exit_code;
   bool options_done = false, video;
   pthread_t sig_thread;
   char *vid, *start;
   void *p;

   if (!shm_env) {
      fprintf(stderr, "mpv_bridge: only runs under 240-MP's ReplayOS core\n");
      return 1;
   }
   p = mmap(NULL, MP240_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, atoi(shm_env), 0);
   if (p == MAP_FAILED || ((mp240_shm *)p)->magic != MP240_BRIDGE_MAGIC ||
       ((mp240_shm *)p)->version != MP240_BRIDGE_VERSION) {
      fprintf(stderr, "mpv_bridge: bridge shared memory unavailable\n");
      return 1;
   }
   shm = p;

   /* Signals are taken by one thread, before libmpv starts its own. */
   sigemptyset(&sigs);
   sigaddset(&sigs, SIGTERM);
   sigaddset(&sigs, SIGINT);
   sigaddset(&sigs, SIGHUP);
   pthread_sigmask(SIG_BLOCK, &sigs, NULL);
   signal(SIGPIPE, SIG_IGN);
   /* Never outlive 240-MP: if it dies, quit as if told to. */
   prctl(PR_SET_PDEATHSIG, SIGTERM);
   if (getppid() == 1)
      return 1;

   if (!(mpv = mpv_create()))
      return 1;
   afd = open_fifo();
   if (afd < 0)
      return 1;

   /* Behave like the mpv program (libmpv's own defaults differ), then output
    * through the bridge. The command line can change all but the outputs. */
   set_default("config", "yes");
   set_default("terminal", "yes");
   set_default("osc", "yes");
   set_default("input-default-bindings", "yes");
   set_default("idle", "once");
   set_default("hwdec", "drm,no");  /* Pi 5: HEVC in hardware, the rest in software */
   set_default("profile", "fast");
   set_default("vo", "libmpv");
   set_default("ao", "pcm");
   set_default("ao-pcm-file", fifo_path);
   set_default("ao-pcm-waveheader", "no");
   set_default("audio-format", "s16");
   set_default("audio-samplerate", "48000");
   set_default("audio-channels", "stereo");

   files = calloc((size_t)argc, sizeof *files);
   for (i = 1; i < argc; i++) {
      if (!options_done && !strcmp(argv[i], "--"))
         options_done = true;
      else if (!options_done && !strncmp(argv[i], "--", 2)) {
         if (!apply_option(argv[i]))
            return 1;
      } else
         files[nfiles++] = argv[i];
   }
   if (nfiles == 0) {
      fprintf(stderr, "mpv_bridge: nothing to play\n");
      return 1;
   }
   if (!set_lists())
      return 1;
   if (mpv_initialize(mpv) < 0) {
      fprintf(stderr, "mpv_bridge: mpv failed to start\n");
      return 1;
   }
   pthread_create(&sig_thread, NULL, signal_main, &sigs);

   vid = mpv_get_property_string(mpv, "vid");
   video = !vid || strcmp(vid, "no") != 0;
   mpv_free(vid);
   if (video && pthread_create(&render_thread, NULL, render_main, NULL) == 0) {
      /* mpv opens its video output when a file loads, which needs the render
       * context in place: wait for it. */
      render_running = true;
      pthread_mutex_lock(&render_lock);
      while (render_ready == 0)
         pthread_cond_wait(&render_cond, &render_lock);
      pthread_mutex_unlock(&render_lock);
   }
   audio_running = pthread_create(&audio_thread, NULL, audio_main, (void *)(intptr_t)afd) == 0;

   /* The mpv program builds its playlist from the files, shuffles it if
    * asked, and starts at --playlist-start; do the same. */
   for (i = 0; i < nfiles; i++) {
      const char *cmd[] = { "loadfile", files[i], "append", NULL };
      mpv_command(mpv, cmd);
   }
   {
      int shuffle = 0;
      mpv_get_property(mpv, "shuffle", MPV_FORMAT_FLAG, &shuffle);
      if (shuffle)
         mpv_command_string(mpv, "playlist-shuffle");
   }
   start = mpv_get_property_string(mpv, "playlist-start");
   {
      int64_t pos = (start && atoi(start) > 0 && atoi(start) < nfiles) ? atoi(start) : 0;
      mpv_set_property(mpv, "playlist-pos", MPV_FORMAT_INT64, &pos);
   }
   mpv_free(start);

   for (;;) {
      mpv_event *e = mpv_wait_event(mpv, -1);
      if (e->event_id == MPV_EVENT_SHUTDOWN)
         break;
      if (e->event_id == MPV_EVENT_FILE_LOADED)
         loaded++;
      if (e->event_id == MPV_EVENT_END_FILE &&
          ((mpv_event_end_file *)e->data)->reason == MPV_END_FILE_REASON_ERROR)
         failed++;
   }

   stopping = true;
   if (render_running) {
      pthread_mutex_lock(&render_lock);
      render_stop = true;
      pthread_cond_signal(&render_cond);
      pthread_mutex_unlock(&render_lock);
      pthread_join(render_thread, NULL);
   }
   mpv_terminate_destroy(mpv);
   audio_exit = true;
   if (audio_running)
      pthread_join(audio_thread, NULL);
   close(afd);
   unlink(fifo_path);

   exit_code = got_signal ? 4 : failed && !loaded ? 2 : failed ? 3 : 0;
   return exit_code;
}
