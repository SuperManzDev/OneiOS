// SPDX-License-Identifier: MIT
// liboneios-display.so: the native half of OneiOSDisplay (java/OneiOSDisplay.java). It owns the
// socket, the mapped frame and the copy into the layer's buffers; the Java half owns the layers.
#define _GNU_SOURCE
#include "xhd.h"
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define FORMAT_BGRA_8888 XHD_BUFFER_BGRA_8888    // HAL_PIXEL_FORMAT_BGRA_8888: the guest's 8-bit bytes as they are
#define FORMAT_RGBX_8888 XHD_BUFFER_RGBX_8888
#define FORMAT_RGBA_FP16 XHD_BUFFER_RGBA_FP16    // the guest's half floats as they are

// ANativeWindow_setBuffersDataSpace and _get are API 28 and libnativewindow's (the Makefile links it: libandroid does
// not have them); the library is built for 30, so they are there.
struct context { int fd; struct xhd_frame frame; };

// SIGTERM, SIGINT and SIGHUP end the run loop through a pipe, so the Java half removes the layers.
static int signal_pipe[2] = { -1, -1 };
static volatile int stopping;
static void on_signal(int number)
{
  int saved = errno;
  char byte = (char)number;
  stopping = 1;
  if (signal_pipe[1] >= 0 && write(signal_pipe[1], &byte, 1) < 0) { }
  errno = saved;
}

JNIEXPORT jlong JNICALL Java_OneiOSDisplay_nOpen(JNIEnv *env, jclass type, jstring socket_path, jint wait_ms)
{
  (void)type;
  if (signal_pipe[0] < 0 && !pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK)) {
    struct sigaction action = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
  }
  const char *path = (*env)->GetStringUTFChars(env, socket_path, NULL);
  if (!path) return 0;
  struct context *c = calloc(1, sizeof *c);
  int fd = c ? xhd_connect(path, wait_ms, &stopping) : -ENOMEM;
  if (fd < 0) fprintf(stderr, "oneios-display: %s: %s\n", path, stopping ? "stopped while waiting" : strerror(-fd));
  (*env)->ReleaseStringUTFChars(env, socket_path, path);
  if (fd < 0) { free(c); return 0; }
  c->fd = fd;
  c->frame.memfd = -1;
  return (jlong)(intptr_t)c;
}

// Reports the panel's mode, then takes xnuhost's hello. JNI_FALSE: no usable hello (nClose is still the caller's).
JNIEXPORT jboolean JNICALL Java_OneiOSDisplay_nHello(JNIEnv *env, jclass type, jlong handle, jint width, jint height, jint hz)
{
  (void)env; (void)type;
  struct context *c = (struct context *)(intptr_t)handle;
  int error = xhd_send_panel(c->fd, (uint32_t)width, (uint32_t)height, (uint32_t)hz);
  if (error) fprintf(stderr, "oneios-display: the panel's mode could not be sent: %s\n", strerror(-error));
  error = xhd_recv_hello(c->fd, &c->frame, 10000);
  if (error) fprintf(stderr, "oneios-display: no usable hello from xnuhost: %s\n", strerror(-error));
  return error ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT jintArray JNICALL Java_OneiOSDisplay_nInfo(JNIEnv *env, jclass type, jlong handle)
{
  (void)type;
  struct context *c = (struct context *)(intptr_t)handle;
  jint values[4] = { (jint)c->frame.width, (jint)c->frame.height, (jint)c->frame.stride, (jint)c->frame.format };
  jintArray array = (*env)->NewIntArray(env, 4);
  if (array) (*env)->SetIntArrayRegion(env, array, 0, 4, values);
  return array;
}

JNIEXPORT jintArray JNICALL Java_OneiOSDisplay_nFit(JNIEnv *env, jclass type, jint sw, jint sh, jint dw, jint dh)
{
  (void)type;
  struct xhd_fit fit;
  xhd_fit((uint32_t)sw, (uint32_t)sh, (uint32_t)dw, (uint32_t)dh, &fit);
  jint values[4] = { (jint)fit.x, (jint)fit.y, (jint)fit.w, (jint)fit.h };
  jintArray array = (*env)->NewIntArray(env, 4);
  if (array) (*env)->SetIntArrayRegion(env, array, 0, 4, values);
  return array;
}

// Posts frames until the socket closes (0) or a signal asks to stop (1); negative on an error.
// Pacing: the next buffer is only handed out when surfaceflinger has taken the last one, so
// ANativeWindow_lock blocks to the panel's vsync, and every flush that came meanwhile is one post.
// The buffer layout and dataspace a frame format gets (xhd_buffer_for): {buffer, dataspace}.
JNIEXPORT jintArray JNICALL Java_OneiOSDisplay_nBuffer(JNIEnv *env, jclass type, jint format)
{
  (void)type;
  int dataspace = 0;
  jint values[2] = { (jint)xhd_buffer_for((uint32_t)format, &dataspace), (jint)dataspace };
  jintArray array = (*env)->NewIntArray(env, 2);
  if (array) (*env)->SetIntArrayRegion(env, array, 0, 2, values);
  return array;
}

// Tags the window's buffers with the dataspace: what the numbers in them mean, so that surfaceflinger turns them into
// the panel's colours (One UI's colour mode decides how; nothing of it is changed here). Logs what the window has.
static void set_dataspace(ANativeWindow *window, int dataspace)
{
  int error = ANativeWindow_setBuffersDataSpace(window, dataspace);
  int now = ANativeWindow_getBuffersDataSpace(window);
  if (error) fprintf(stderr, "oneios-display: the dataspace %s (%#x) was refused (%d); the buffers say %s (%#x)\n",
                     xhd_dataspace_name(dataspace), (unsigned)dataspace, error, xhd_dataspace_name(now), (unsigned)now);
  else fprintf(stderr, "oneios-display: buffers tagged %s (dataspace %#x)\n", xhd_dataspace_name(now), (unsigned)now);
}

static const char *format_name(int format)
{
  return format == FORMAT_BGRA_8888 ? "BGRA_8888" : format == FORMAT_RGBX_8888 ? "RGBX_8888" : format == FORMAT_RGBA_FP16 ? "RGBA_FP16" : "other";
}

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static double seconds_since(const struct timespec *t0)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

// The copy loop's thread at the top clock while it runs: uclamp.min 1024 (sched_setattr(2), Linux 5.3; KEEP_POLICY
// and KEEP_PARAMS: SCHED_OTHER and its nice stay, no realtime thread). Each frame is one copy of the damage into the
// layer's buffer (about 1 ms for 1080x2340 at the top clock) between the buffer queue's vsync-paced lock and post; with
// the phone's CPU floors left to the governor in a session (the switch's clocks_tops writes cpuinfo_min_freq), the copy
// must not wait for its ramp. Idle (poll) costs nothing. ONEIOS_DISPLAY_UCLAMP_MIN=N (0..1024) in the environment sets
// another floor, 0 none (displayctl pool gives 0: a VM of crosvm's frames, at most 45 a second of a few rows each, need
// no top clock, and a phone kept at it heats). 0 or -errno; -ECANCELED when the floor is 0 (nothing set).
static unsigned copy_loop_floor = 1024;
static int copy_loop_clamp(void)
{
  const char *wanted = getenv("ONEIOS_DISPLAY_UCLAMP_MIN");
  if (wanted && *wanted) {
    char *end = NULL;
    unsigned long v = strtoul(wanted, &end, 10);
    if (!*end && v <= 1024) copy_loop_floor = (unsigned)v;
  }
  if (!copy_loop_floor) return -ECANCELED;
#if defined(__linux__) && defined(SYS_sched_setattr)
  struct { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t priority; uint64_t runtime, deadline, period;
           uint32_t util_min, util_max; } attr = { .size = 56, .flags = 0x18u | 0x20u, .util_min = copy_loop_floor };
  _Static_assert(sizeof attr == 56, "struct sched_attr, SCHED_ATTR_SIZE_VER1");
  return syscall(SYS_sched_setattr, 0, &attr, 0) ? -errno : 0;
#else
  return -ENOSYS;
#endif
}

// ---- the present path without the CPU caches' maintenance (ONEIOS_DISPLAY_PRESENT=ahb)
// The window path's buffers are the CPU's cached ones (Surface::lock asks for SW_READ_OFTEN | SW_WRITE_OFTEN), and each
// lock and each post cleans and invalidates the CPU's caches over the whole buffer, whatever the damage (the S25,
// 2026-11-23, simpleperf on displayctl test at 1080x2340 and 60 frames a second: dcache_inval_poc 36%, dcache_clean_poc
// 31%, arch_sync_dma_for_cpu 9%, arch_sync_dma_for_device 9%, the copy itself 10%: about 1% of a core for each frame a
// second). Here the frames go into AHardwareBuffers of this process's own that the CPU only writes (CPU_WRITE_RARELY:
// not cached by the CPU, nothing to maintain), on a child of the layer's surface (ASurfaceControl_createFromWindow: the
// matrix and the position OneiOSDisplay.java set on the layer place it), posted with ASurfaceTransaction. Each buffer
// gets what it misses: the damage of every frame since it was last filled. Three buffers, at most two transactions in
// flight, a buffer filled again only after the compositor's release fence. 8-bit frames only. Opt in (displayctl pool
// sets it): the hosted session keeps the window path.
#define AHB_SLOTS 3
// The child layer OneiOSDisplay.java made for the frames (nChild), or NULL: ahb_open then asks for one of the window's.
static ASurfaceControl *given_child;

// ASurfaceControl_fromJava is Android 14's (API 34); this library is built for API 30, so it is looked up.
JNIEXPORT jboolean JNICALL Java_OneiOSDisplay_nChild(JNIEnv *env, jclass type, jobject surface_control)
{
  (void)type;
  typedef ASurfaceControl *(*from_java_t)(JNIEnv *, jobject);
  void *android = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
  if (!android) android = dlopen("libandroid.so", RTLD_NOW);
  from_java_t from_java = android ? (from_java_t)dlsym(android, "ASurfaceControl_fromJava") : NULL;
  if (!from_java) {
    fprintf(stderr, "oneios-display: no ASurfaceControl_fromJava (Android 14 and later)\n");
    return JNI_FALSE;
  }
  if (given_child) ASurfaceControl_release(given_child);
  given_child = from_java(env, surface_control);
  return given_child ? JNI_TRUE : JNI_FALSE;
}
struct ahb_present {
  ASurfaceControl *sc;
  AHardwareBuffer *buffer[AHB_SLOTS];
  uint32_t stride[AHB_SLOTS];          // in pixels
  int release[AHB_SLOTS];              // the compositor's release fence for each, or -1
  struct xhd_rect stale[AHB_SLOTS];    // what each misses
  int shown, layout, inflight, wake[2];
  pthread_mutex_t lock;
};
struct ahb_done { struct ahb_present *p; int replaced; };

static void ahb_completed(void *context, ASurfaceTransactionStats *stats)
{
  struct ahb_done *done = context;
  struct ahb_present *p = done->p;
  int fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, p->sc);   // the buffer this one replaced
  pthread_mutex_lock(&p->lock);
  if (done->replaced >= 0) {
    if (p->release[done->replaced] >= 0) close(p->release[done->replaced]);
    p->release[done->replaced] = fence;
  } else if (fence >= 0) {
    close(fence);
  }
  p->inflight--;
  pthread_mutex_unlock(&p->lock);
  free(done);
  char byte = 1;
  if (write(p->wake[1], &byte, 1) < 0) { }
}

static int ahb_inflight(struct ahb_present *p)
{
  pthread_mutex_lock(&p->lock);
  int n = p->inflight;
  pthread_mutex_unlock(&p->lock);
  return n;
}

static void ahb_wait(struct ahb_present *p, int ms)
{
  struct pollfd w = { p->wake[0], POLLIN, 0 };
  if (poll(&w, 1, ms) > 0) {
    char sink[16];
    while (read(p->wake[0], sink, sizeof sink) > 0) { }
  }
}

static void ahb_close(struct ahb_present *p)
{
  if (p->sc) {
    ASurfaceTransaction *t = ASurfaceTransaction_create();
    if (t) {
      ASurfaceTransaction_setVisibility(t, p->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
      ASurfaceTransaction_reparent(t, p->sc, NULL);
      ASurfaceTransaction_apply(t);
      ASurfaceTransaction_delete(t);
    }
  }
  // The completions refer to p: it is let go only when none is left (else kept: the process ends soon anyway).
  for (int i = 0; i < 20 && p->wake[0] >= 0 && ahb_inflight(p) > 0; i++) ahb_wait(p, 50);
  if (p->wake[0] >= 0 && ahb_inflight(p) > 0) return;
  for (int k = 0; k < AHB_SLOTS; k++) {
    if (p->buffer[k]) AHardwareBuffer_release(p->buffer[k]);
    if (p->release[k] >= 0) close(p->release[k]);
  }
  if (p->sc) ASurfaceControl_release(p->sc);
  if (p->wake[0] >= 0) { close(p->wake[0]); close(p->wake[1]); }
  pthread_mutex_destroy(&p->lock);
  free(p);
}

// The buffers and the child surface; NULL (said) when this phone does not give them: the window path then.
static struct ahb_present *ahb_open(ANativeWindow *window, const struct xhd_frame *f, int format)
{
  if (f->format != XHD_FORMAT_BGRX || (format != FORMAT_BGRA_8888 && format != FORMAT_RGBX_8888)) return NULL;
  struct ahb_present *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  pthread_mutex_init(&p->lock, NULL);
  p->shown = -1;
  p->wake[0] = p->wake[1] = -1;
  for (int k = 0; k < AHB_SLOTS; k++) {
    p->release[k] = -1;
    p->stale[k] = (struct xhd_rect){ 0, 0, f->width, f->height };
  }
  AHardwareBuffer_Desc d = { .width = f->width, .height = f->height, .layers = 1, .format = (uint32_t)format,
                             .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY };
  if (!AHardwareBuffer_isSupported(&d)) {
    d.format = FORMAT_RGBX_8888;
    if (!AHardwareBuffer_isSupported(&d)) {
      fprintf(stderr, "oneios-display: no write-only buffers here: the window path\n");
      ahb_close(p);
      return NULL;
    }
  }
  p->layout = (int)d.format;
  p->sc = given_child ? given_child : ASurfaceControl_createFromWindow(window, "OneiOS frame");
  given_child = NULL;
  int ok = p->sc != NULL && !pipe2(p->wake, O_CLOEXEC | O_NONBLOCK), failed = 0;
  if (!p->sc) fprintf(stderr, "oneios-display: no child surface of the layer's window (ASurfaceControl_createFromWindow)\n");
  for (int k = 0; ok && k < AHB_SLOTS; k++) {
    failed = AHardwareBuffer_allocate(&d, &p->buffer[k]);
    ok = !failed;
    if (ok) {
      AHardwareBuffer_Desc got;
      AHardwareBuffer_describe(p->buffer[k], &got);
      p->stride[k] = got.stride;
    } else {
      fprintf(stderr, "oneios-display: buffer %d of %ux%u format %u usage %#llx: AHardwareBuffer_allocate %d\n", k, d.width, d.height,
              d.format, (unsigned long long)d.usage, failed);
    }
  }
  if (!ok) {
    fprintf(stderr, "oneios-display: the write-only buffers or the child surface could not be made: the window path\n");
    ahb_close(p);
    return NULL;
  }
  fprintf(stderr, "oneios-display: %d write-only buffers (%s, CPU_WRITE_RARELY: no CPU cache to maintain) on a child surface\n",
          AHB_SLOTS, format_name(p->layout));
  return p;
}

// One frame: damage d into the next buffer with what it misses, posted. 0, or -1 when a call failed.
static int ahb_post(struct ahb_present *p, const struct xhd_frame *f, struct xhd_rect d, int dataspace, int first, struct xhd_rect *copied)
{
  for (int k = 0; k < AHB_SLOTS; k++) {
    if (p->stale[k].w) xhd_rect_union(&p->stale[k], &d);
    else p->stale[k] = d;
  }
  for (int i = 0; i < 40 && ahb_inflight(p) >= 2; i++) ahb_wait(p, 25);     // at most two in flight (a second or so)
  const int k = (p->shown + 1) % AHB_SLOTS;
  pthread_mutex_lock(&p->lock);
  int fence = p->release[k];
  p->release[k] = -1;
  pthread_mutex_unlock(&p->lock);
  if (fence >= 0) {
    struct pollfd w = { fence, POLLIN, 0 };
    poll(&w, 1, 200);
    close(fence);
  }
  struct xhd_rect r = p->stale[k];
  ARect area = { (int32_t)r.x, (int32_t)r.y, (int32_t)(r.x + r.w), (int32_t)(r.y + r.h) };
  void *bits = NULL;
  if (AHardwareBuffer_lock(p->buffer[k], AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, r.w ? &area : NULL, &bits) || !bits) return -1;
  if (r.w && xhd_rect_clip(&r, f->width, f->height)) xhd_copy(bits, (size_t)p->stride[k] * 4, p->layout, f->pixels, f->stride, f->format, r);
  int written = -1;
  if (AHardwareBuffer_unlock(p->buffer[k], &written)) return -1;
  p->stale[k] = (struct xhd_rect){ 0, 0, 0, 0 };
  ASurfaceTransaction *t = ASurfaceTransaction_create();
  struct ahb_done *done = malloc(sizeof *done);
  if (!t || !done) {
    if (written >= 0) close(written);
    free(done);
    if (t) ASurfaceTransaction_delete(t);
    return -1;
  }
  ASurfaceTransaction_setBuffer(t, p->sc, p->buffer[k], written);           // the transaction owns the fence
  ARect damage = { (int32_t)d.x, (int32_t)d.y, (int32_t)(d.x + d.w), (int32_t)(d.y + d.h) };
  ASurfaceTransaction_setDamageRegion(t, p->sc, &damage, 1);
  if (first) {
    ASurfaceTransaction_setVisibility(t, p->sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_setBufferTransparency(t, p->sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    ASurfaceTransaction_setBufferDataSpace(t, p->sc, (enum ADataSpace)dataspace);
  }
  *done = (struct ahb_done){ p, p->shown };
  ASurfaceTransaction_setOnComplete(t, done, ahb_completed);
  pthread_mutex_lock(&p->lock);
  p->inflight++;
  pthread_mutex_unlock(&p->lock);
  ASurfaceTransaction_apply(t);
  ASurfaceTransaction_delete(t);
  p->shown = k;
  *copied = r;
  return 0;
}

// ready_path: SESSION/ready (--session), or NULL (--socket): the first post waits for it (xhd_hold_first). Until the
// first post the layer has no buffer and the OneOS boot screen under it shows.
JNIEXPORT jint JNICALL Java_OneiOSDisplay_nRun(JNIEnv *env, jclass type, jlong handle, jobject surface, jint format, jint dataspace,
                                               jstring ready_path)
{
  (void)type;
  struct context *c = (struct context *)(intptr_t)handle;
  const struct xhd_frame *f = &c->frame;
  ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
  if (!window) { fprintf(stderr, "oneios-display: no native window for the layer\n"); return -1; }
  int clamp = copy_loop_clamp();
  if (clamp == -ECANCELED) fprintf(stderr, "oneios-display: the copy loop keeps the governor's clock (ONEIOS_DISPLAY_UCLAMP_MIN=0)\n");
  else if (clamp) fprintf(stderr, "oneios-display: the copy loop keeps the governor's clock: uclamp.min not set (%s)\n", strerror(-clamp));
  else fprintf(stderr, "oneios-display: the copy loop at uclamp.min %u while it runs\n", copy_loop_floor);
  ANativeWindow_setBuffersGeometry(window, (int32_t)f->width, (int32_t)f->height, format);
  set_dataspace(window, dataspace);
  char ready_file[600] = "";
  if (ready_path) {
    const char *p = (*env)->GetStringUTFChars(env, ready_path, NULL);
    if (p) { snprintf(ready_file, sizeof ready_file, "%s", p); (*env)->ReleaseStringUTFChars(env, ready_path, p); }
  }
  const char *present = getenv("ONEIOS_DISPLAY_PRESENT");
  struct ahb_present *ahb = present && !strcmp(present, "ahb") ? ahb_open(window, f, format) : NULL;
  struct xhd_rect pending = { 0, 0, f->width, f->height };
  unsigned long frames = 0, held_flushes = 0;
  uint64_t lock_ns = 0, copy_ns = 0, post_ns = 0, copied_bytes = 0;   // where a frame's time goes (the log at the end)
  bool held = false;
  struct timespec hold_start;
  int result;
  for (;;) {
    // Held: the guest's frame is not shown yet, the damage adds up, and `ready` is looked for at every flush and
    // every XHD_HOLD_POLL_MS between them (a guest that drew its frame with content and then nothing more).
    bool hold = frames == 0 && xhd_hold_first(ready_file);
    if (hold && !held) {
      held = true;
      clock_gettime(CLOCK_MONOTONIC, &hold_start);
      fprintf(stderr, "oneios-display: the first frame waits for %s (xnuhost's first frame of the guest's own with content); the boot screen stays\n", ready_file);
    }
    struct pollfd fds[2] = { { c->fd, POLLIN, 0 }, { signal_pipe[0], POLLIN, 0 } };
    int ready = poll(fds, 2, hold ? XHD_HOLD_POLL_MS : pending.w ? 0 : -1);
    if (ready < 0 && errno != EINTR) { result = -3; break; }
    if (stopping || (ready > 0 && fds[1].revents)) { result = 1; break; }
    if (xhd_drain(c->fd, f, &pending) < 0) { result = 0; break; }
    if (hold) { if (ready > 0) held_flushes++; continue; }
    if (!pending.w) continue;
    if (ahb) {
      struct xhd_rect r = { 0, 0, 0, 0 };
      const uint64_t t0 = now_ns();
      if (ahb_post(ahb, f, pending, dataspace, frames == 0, &r)) {
        fprintf(stderr, "oneios-display: a write-only buffer could not be posted\n");
        result = -2;
        break;
      }
      post_ns += now_ns() - t0;
      copied_bytes += (uint64_t)r.w * r.h * 4;
      if (xhd_acks_wanted()) xhd_send_ack(c->fd, (uint32_t)frames + 1, &r);
      if (frames++ == 0) fprintf(stderr, "oneios-display: first frame posted (write-only buffers, %ux%u; the guest's frame: %s)\n",
                                 f->width, f->height, xh_pixels_name(f->format));
      pending = (struct xhd_rect){ 0, 0, 0, 0 };
      continue;
    }
    // The first posts fill whole buffers; after that the window copies back what the last frame
    // had and hands out the rectangle that has to be drawn (it may be larger than what was asked).
    ARect dirty = { (int32_t)pending.x, (int32_t)pending.y, (int32_t)(pending.x + pending.w), (int32_t)(pending.y + pending.h) };
    if (frames < 4) dirty = (ARect){ 0, 0, (int32_t)f->width, (int32_t)f->height };
    ANativeWindow_Buffer buffer;
    const uint64_t t_lock = now_ns();
    int error = ANativeWindow_lock(window, &buffer, &dirty);
    const uint64_t t_locked = now_ns();
    if (!error) lock_ns += t_locked - t_lock;
    if (error) {
      if (frames == 0 && format != FORMAT_RGBX_8888) {
        // No B,G,R,A buffers here: R,G,B,X and a swap. No RGBA_FP16 ones: B,G,R,A, the half floats encoded to sRGB
        // and clamped (what is outside sRGB is lost), tagged sRGB.
        int next = format == FORMAT_RGBA_FP16 ? FORMAT_BGRA_8888 : FORMAT_RGBX_8888;
        fprintf(stderr, "oneios-display: no buffer in format %d %s (%d), using %s\n", format, format_name(format), error, format_name(next));
        format = next;
        ANativeWindow_setBuffersGeometry(window, (int32_t)f->width, (int32_t)f->height, format);
        if (dataspace == XHD_DATASPACE_SCRGB_LINEAR || dataspace == XHD_DATASPACE_SCRGB) set_dataspace(window, dataspace = XHD_DATASPACE_SRGB);
        continue;
      }
      fprintf(stderr, "oneios-display: the layer's buffer could not be locked (%d)\n", error);
      result = -2;
      break;
    }
    struct xhd_rect r = { 0, 0, 0, 0 };
    if (dirty.left < 0) dirty.left = 0;
    if (dirty.top < 0) dirty.top = 0;
    if (dirty.right > dirty.left && dirty.bottom > dirty.top) {
      r = (struct xhd_rect){ (uint32_t)dirty.left, (uint32_t)dirty.top, (uint32_t)(dirty.right - dirty.left), (uint32_t)(dirty.bottom - dirty.top) };
      uint32_t width = (uint32_t)buffer.width < f->width ? (uint32_t)buffer.width : f->width;
      uint32_t height = (uint32_t)buffer.height < f->height ? (uint32_t)buffer.height : f->height;
      // The buffer's own format decides the copy (RGBA_8888 and RGBX_8888 are both R,G,B bytes).
      int layout = buffer.format == FORMAT_BGRA_8888 || buffer.format == FORMAT_RGBA_FP16 ? buffer.format : FORMAT_RGBX_8888;
      size_t bytes = layout == FORMAT_RGBA_FP16 ? 8 : 4;
      if (buffer.width > 0 && buffer.height > 0 && buffer.stride >= buffer.width && xhd_rect_clip(&r, width, height)) {
        xhd_copy(buffer.bits, (size_t)buffer.stride * bytes, layout, f->pixels, f->stride, f->format, r);
        copied_bytes += (uint64_t)r.w * r.h * bytes;
      }
    }
    const uint64_t t_copied = now_ns();
    ANativeWindow_unlockAndPost(window);
    copy_ns += t_copied - t_locked;
    post_ns += now_ns() - t_copied;
    if (xhd_acks_wanted()) xhd_send_ack(c->fd, (uint32_t)frames + 1, &r);   // the frame was copied: the sender may write again
    if (frames++ == 0) {
      char waited[96] = "";
      if (held) snprintf(waited, sizeof waited, "; held %.1f s for session/ready, %lu wakes for flushes not shown", seconds_since(&hold_start), held_flushes);
      fprintf(stderr, "oneios-display: first frame posted (%dx%d buffer, stride %d, format %d %s, dataspace %#x %s; the guest's frame: %s%s)\n",
              buffer.width, buffer.height, buffer.stride, buffer.format, format_name(buffer.format), (unsigned)ANativeWindow_getBuffersDataSpace(window),
              xhd_dataspace_name(ANativeWindow_getBuffersDataSpace(window)), xh_pixels_name(f->format), waited);
    }
    pending = (struct xhd_rect){ 0, 0, 0, 0 };
  }
  if (ahb) ahb_close(ahb);
  ANativeWindow_release(window);
  fprintf(stderr, "oneios-display: %s after %lu frames\n", result == 0 ? "the socket closed" : result == 1 ? "asked to stop" : "error", frames);
  if (frames)
    fprintf(stderr, "oneios-display: a frame's time: the buffer's lock %llu us (with the wait for it), the copy %llu us (%llu KiB), the post %llu us\n",
            (unsigned long long)(lock_ns / frames / 1000), (unsigned long long)(copy_ns / frames / 1000),
            (unsigned long long)(copied_bytes / frames / 1024), (unsigned long long)(post_ns / frames / 1000));
  return result;
}

JNIEXPORT void JNICALL Java_OneiOSDisplay_nClose(JNIEnv *env, jclass type, jlong handle)
{
  (void)env; (void)type;
  struct context *c = (struct context *)(intptr_t)handle;
  if (!c) return;
  close(c->fd);
  xhd_frame_close(&c->frame);
  free(c);
}
