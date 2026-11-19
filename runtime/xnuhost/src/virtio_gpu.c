// SPDX-License-Identifier: MIT
// virtio-gpu device (VirtIO 1.2, 5.7).
// 2D: the scanout the guest's display driver presents frames on (UniXNU-droidvm's DroidVMVirtioGpuDisplay.cpp under
// IOMobileFramebufferLegacy); the first frame says the guest is up.
// 3D (xh_virtio_gpu_create_3d): crosvm's DRM native context as UniXNU-droidvm's DroidVMVirtioGpu3D.cpp drives it --
// capset 6, CTX_CREATE, RESOURCE_CREATE_BLOB (HOST3D for a context's shmem, HOST3D_GUEST over guest pages),
// MAP_BLOB answered with MAP_INFO_POOL and the offset in /arm-io/virtio-gpu drm-host-pool, SUBMIT_3D with per-ring
// fences (a fenced command is returned when its fence retires). The renderer is behind gpu3d.h (drm2kgsl on KGSL).
#include "xnuhost.h"
#include "gpu3d.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>

enum {
  CMD_GET_DISPLAY_INFO = 0x100, CMD_RESOURCE_CREATE_2D, CMD_RESOURCE_UNREF, CMD_SET_SCANOUT, CMD_RESOURCE_FLUSH,
  CMD_TRANSFER_TO_HOST_2D, CMD_RESOURCE_ATTACH_BACKING, CMD_RESOURCE_DETACH_BACKING,
  CMD_GET_CAPSET_INFO = 0x108, CMD_GET_CAPSET = 0x109, CMD_RESOURCE_CREATE_BLOB = 0x10c, CMD_SET_SCANOUT_BLOB = 0x10d,
  CMD_CTX_CREATE = 0x200, CMD_CTX_DESTROY, CMD_CTX_ATTACH_RESOURCE, CMD_CTX_DETACH_RESOURCE,
  CMD_SUBMIT_3D = 0x207, CMD_RESOURCE_MAP_BLOB, CMD_RESOURCE_UNMAP_BLOB,
  RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO, RESP_OK_CAPSET_INFO, RESP_OK_CAPSET, RESP_OK_MAP_INFO = 0x1106,
  RESP_ERR_UNSPEC = 0x1200, RESP_ERR_OUT_OF_MEMORY, RESP_ERR_INVALID_SCANOUT_ID, RESP_ERR_INVALID_RESOURCE_ID,
  RESP_ERR_INVALID_CONTEXT_ID, RESP_ERR_INVALID_PARAMETER,
};
enum { FMT_BGRA = 1, FMT_BGRX = 2, FMT_ARGB = 3, FMT_XRGB = 4, FMT_RGBA = 67, FMT_XBGR = 68, FMT_ABGR = 121, FMT_RGBX = 134 };
// Half floats (R,G,B,A, linear extended sRGB): xnuhost's own code, include/xnuhost/pixels.h.
#define FMT_RGBA16F XH_VIRTIO_GPU_FORMAT_RGBA16F

static bool format_known(uint32_t f)
{
  return f == FMT_BGRA || f == FMT_BGRX || f == FMT_ARGB || f == FMT_XRGB || f == FMT_RGBA || f == FMT_XBGR || f == FMT_ABGR ||
         f == FMT_RGBX || f == FMT_RGBA16F;
}

// Bytes a pixel of a scanout format takes in guest memory.
static unsigned format_bpp(uint32_t f) { return f == FMT_RGBA16F ? 8u : 4u; }

// The display's pixels (display-format=): what a resource's host copy and a blob's flush are written in.
static unsigned display_format(void)
{
  xh_display *d = xh_display_get();
  return d && d->format ? d->format : XH_PIXELS_BGRX8;      // 0: a display made before formats (B,G,R,X)
}

typedef struct { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; } gpu_hdr;
typedef struct { uint32_t x, y, w, h; } gpu_rect;
typedef xh_gpu_entry gpu_entry;
enum { FLAG_FENCE = 1, FLAG_RING_IDX = 2, MAP_INFO_POOL = 1u << 31 };
enum { F_VIRGL = 1u << 0, F_RESOURCE_BLOB = 1u << 3, F_CONTEXT_INIT = 1u << 4, F_CREATE_GUEST_HANDLE = 1u << 6 };

#define MAX_RES 64
#define MAX_BACKING 16384              // as MAX_BLOB_ENTRIES: a 2D surface of scattered 4 KiB pages (1440x3120 has 4388)
typedef struct {
  uint32_t id, format, width, height;
  uint8_t *pixels;                 // host copy, in the display's pixels (dformat: B,G,R,X 8-bit or R,G,B,A half floats)
  unsigned dformat, dbpp;
  gpu_entry *backing;
  unsigned nbacking;
} resource;

#define MAX_CTX 128
#define MAX_BLOB_ENTRIES 16384         // DroidVMVirtioGpu.h MaxEntries
#define BLOB_SLOTS (1u << 15)          // open addressing, linear probing: blob resource id -> blob
#define WORKER_IOV 256

// A blob resource. Guest blobs (GUEST: the display's surfaces; HOST3D_GUEST: a context's BOs) keep the guest's
// segments, so a blob can be a scanout; HOST3D blobs (a context's shmem) are the renderer's alone.
typedef struct {
  uint32_t id, ctx, mem;
  uint64_t size;
  gpu_entry *entries;
  unsigned nentries;
} blob;

// A fenced command whose chain goes back to the guest when its context's ring passes the fence.
typedef struct pending {
  struct pending *next;
  uint16_t head;
  uint32_t written, ctx, ring;
  uint64_t fence, gen;
} pending;

typedef struct {
  unsigned width, height;
  resource res[MAX_RES];
  uint32_t scanout_resource;
  bool first_frame;
  // 3D. The worker is the only thread that changes the state above and the tables below. `pl` guards the list of
  // pending fences and the worker's bookkeeping; `gen` (bumped at every device reset) changes under the device's lock.
  xh_gpu3d *b;
  xh_virtio *dev;
  uint32_t ctx[MAX_CTX];               // live context ids
  bool ctx_fast[MAX_CTX];              // that context's commands are the fast lane's (the render server's: "xnu:backboardd")
  blob *blobs;                         // BLOB_SLOTS of them; id 0 = free
  unsigned nblobs;
  // The blobs the host holds (2026-11-13): bytes of the live ones, made and unref'd since the start, and the last line's
  // figures (blob_note). Set where the worker changes the table.
  uint64_t blob_bytes, blobs_made, blobs_unref, blob_note_us, noted_made, noted_unref;
  struct { uint32_t width, height, format, stride, offset; } scan;      // the scanout when it is a blob
  pthread_mutex_t pl;
  pthread_cond_t idle;
  pending *fences;
  uint64_t gen, kicks, served;
  bool reset_requested;
  int wake;                            // eventfd: the control queue was kicked (written from the trap handler)
  // Two lanes. The queue's thread serves the render server's contexts and every command of no context (the display,
  // the 2D commands) at once, and hands every other context's commands, in their order, to the slow lane's thread.
  // An app that makes 100 blobs at its launch (1-30 ms each on the phone: udmabufs, KGSL's import and bind) then no
  // longer holds back the render server's next frame and the display's flush behind them. `cmd` makes the two take
  // turns at the state above and the renderer (a frame waits for one blob at most, not for all of them).
  pthread_mutex_t cmd;
  pthread_cond_t slow_cv;
  struct slow_job *slow_head, **slow_tail;
  uint64_t slow_queued, slow_done;
  // The render server's lane first (2026-11-19). `cmd` is a plain mutex: the thread that unlocks it can lock it again
  // before the waiter it woke runs, so the slow lane, with a queue of an app's blobs, made blob after blob while the
  // render server's next command waited for all of them, not for one. `fast_waiting` is 1 while the queue's thread
  // waits for `cmd` (fast_lock); the slow lane gives way before each of its commands while it is (slow_lock: `slow_yield`
  // and `turn_cv` under `pl`). `lanes` guards ctx[] and ctx_fast[] for the queue's thread, which reads them to choose a
  // command's lane without `cmd`: before, a blob in progress held back even that choice, and so every command behind it.
  // Writers hold `cmd` and `lanes`. Lock order: cmd, then pl or lanes; nothing is taken while `lanes` is held.
  pthread_mutex_t lanes;
  atomic_uint fast_waiting;
  atomic_bool slow_yield;
  pthread_cond_t turn_cv;
  // The 10 s line of the render server's lane (lane_note), under `cmd`: its commands, its waits for `cmd` over 1 ms and
  // the longest; the slow lane's count of giving way (`gave_way`, its own thread) and the last line's figure of it.
  uint64_t lane_cmds, lane_waits, lane_wait_max, lane_note_us, gave_noted;
  atomic_ullong gave_way;
} gpu_state;

typedef struct slow_job {
  struct slow_job *next;
  uint16_t head;
  unsigned n;
  uint64_t gen;
  xh_viov iov[];
} slow_job;

static resource *find(gpu_state *g, uint32_t id)
{
  for (unsigned i = 0; id && i < MAX_RES; i++) if (g->res[i].id == id) return &g->res[i];
  return NULL;
}

static void release(resource *r)
{
  free(r->pixels);
  free(r->backing);
  memset(r, 0, sizeof *r);
}

// Byte order of a format's pixel in guest memory -> B,G,R,X.
static void convert(uint8_t *to, const uint8_t *from, uint32_t format, size_t pixels)
{
  switch (format) {
  case FMT_BGRA: case FMT_BGRX: memcpy(to, from, pixels * 4); break;
  case FMT_RGBA: case FMT_RGBX:
    for (size_t i = 0; i < pixels; i++, to += 4, from += 4) { to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = from[3]; }
    break;
  case FMT_ARGB: case FMT_XRGB:
    for (size_t i = 0; i < pixels; i++, to += 4, from += 4) { to[0] = from[3]; to[1] = from[2]; to[2] = from[1]; to[3] = from[0]; }
    break;
  default:                         // ABGR, XBGR
    for (size_t i = 0; i < pixels; i++, to += 4, from += 4) { to[0] = from[1]; to[1] = from[2]; to[2] = from[3]; to[3] = from[0]; }
  }
}

// A row of a scanout format's pixels into the display's (display_format()). Half floats into half floats are copied;
// into 8 bits they are encoded to sRGB and clamped (what is outside sRGB and above white is lost, as in the guest's
// own CPU conversion); 8-bit pixels into half floats are decoded to linear light.
static void convert_to(uint8_t *to, unsigned dformat, const uint8_t *from, uint32_t format, size_t pixels)
{
  if (format == FMT_RGBA16F) {
    if (dformat == XH_PIXELS_RGBA16F) memcpy(to, from, pixels * 8);
    else xh_pixels_rgba16f_to_8(to, from, pixels, 0);
    return;
  }
  if (dformat != XH_PIXELS_RGBA16F) { convert(to, from, format, pixels); return; }
  if (format == FMT_BGRA || format == FMT_BGRX) { xh_pixels_bgrx8_to_rgba16f(to, from, pixels); return; }
  uint8_t row[4 * 256];                                // another byte order: through B,G,R,X, 256 pixels at a time
  for (size_t done = 0; done < pixels;) {
    size_t n = pixels - done < 256 ? pixels - done : 256;
    convert(row, from + done * 4, format, n);
    xh_pixels_bgrx8_to_rgba16f(to + done * 8, row, n);
    done += n;
  }
}

// Copies `length` bytes at linear `offset` of a resource's backing into `out`.
static bool backing_read(const resource *r, uint64_t offset, uint8_t *out, uint64_t length)
{
  for (unsigned i = 0; i < r->nbacking && length; i++) {
    uint64_t size = r->backing[i].length;
    if (offset >= size) { offset -= size; continue; }
    uint64_t n = size - offset < length ? size - offset : length;
    const uint8_t *p = xh_virtio_translate(r->backing[i].addr + offset, n);
    if (!p) return false;
    memcpy(out, p, n);
    out += n; length -= n; offset = 0;
  }
  return length == 0;
}

// The guest's display driver also scans out the boot framebuffer itself (DroidVMVirtioGpuDisplay.cpp's `boot`
// resource: the kernel console until the compositor's first frame, and later whenever a blank or CPU-copied frame was
// drawn there). That picture is shown like any other, but it is the console's. A frame of a surface of the guest's
// own goes to main.c's ready rule (ready.c) with whether the display's frame now shows anything: backboardd's first
// frames are black, and the guest is ready (session/ready) at the first that is not (2026-11-15). Called before the
// frame is posted to the display helper, so that session/ready is there when the helper sees the frame.
// `ready` (g->first_frame) stops the calls once the rule has said yes.
static void frame_shown(gpu_state *g, const gpu_entry *entries, unsigned n)
{
  if (g->first_frame || (n && g_info.video[0] && entries[0].addr == g_info.video[0])) return;
  xh_display *d = xh_display_get();
  bool lit = d && xh_pixels_lit(d->pixels, d->stride, d->width, d->height, d->format);
  if (xh_guest_frame(lit)) g->first_frame = true;
}

static bool rect_ok(const gpu_rect *r, uint32_t w, uint32_t h)
{
  return r->w && r->h && r->x <= w && r->y <= h && r->w <= w - r->x && r->h <= h - r->y;
}

// ---- 3D --------------------------------------------------------------------------------------------------------------
static unsigned blob_hash(uint32_t id) { return (id * 2654435761u) >> 17 & (BLOB_SLOTS - 1); }

static blob *blob_find(gpu_state *g, uint32_t id)
{
  if (!id) return NULL;
  for (unsigned i = blob_hash(id); g->blobs[i].id; i = (i + 1) & (BLOB_SLOTS - 1)) if (g->blobs[i].id == id) return &g->blobs[i];
  return NULL;
}

static blob *blob_add(gpu_state *g, uint32_t id)
{
  if (g->nblobs >= BLOB_SLOTS / 4 * 3) return NULL;
  unsigned i = blob_hash(id);
  while (g->blobs[i].id) i = (i + 1) & (BLOB_SLOTS - 1);
  g->blobs[i] = (blob){ .id = id };
  g->nblobs++;
  return &g->blobs[i];
}

static void blob_del(gpu_state *g, blob *b)
{
  unsigned i = (unsigned)(b - g->blobs), j = i;
  free(b->entries);
  g->nblobs--;
  g->blob_bytes -= b->size;
  for (;;) {                                         // backward shift: the probe chains stay whole
    g->blobs[i].id = 0;
    for (;;) {
      j = (j + 1) & (BLOB_SLOTS - 1);
      if (!g->blobs[j].id) return;
      unsigned k = blob_hash(g->blobs[j].id);
      if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
      break;
    }
    g->blobs[i] = g->blobs[j];
    i = j;
  }
}

static uint32_t *ctx_find(gpu_state *g, uint32_t id)
{
  for (unsigned i = 0; id && i < MAX_CTX; i++) if (g->ctx[i] == id) return &g->ctx[i];
  return NULL;
}


// The copy of each scanout flush, one log line per 10 s while frames come: `display: 10 s: flushes N, copy mean X us
// max Y us (Z MiB)`. The device's one thread does it, so the guest's next commands (a frame's submissions, its blobs)
// wait behind it.
static uint64_t flush_now_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void flush_note(uint64_t took, uint64_t bytes)
{
  static uint64_t since, n, sum, max, total;
  uint64_t now = flush_now_us();
  if (!since) since = now;
  n++; sum += took; total += bytes; if (took > max) max = took;
  if (now - since >= 10000000u) {
    LOGI("display: %llu s: flushes %llu, copy mean %llu us max %llu us (%llu MiB)", (unsigned long long)((now - since) / 1000000u),
         (unsigned long long)n, (unsigned long long)(sum / n), (unsigned long long)max, (unsigned long long)(total >> 20));
    since = now; n = sum = max = total = 0;
  }
}

// RESOURCE_FLUSH of a blob: when it is the scanout (SET_SCANOUT_BLOB), its rows go from the guest's pages to the display.
static uint32_t flush_blob(gpu_state *g, const blob *b, const gpu_rect *rect)
{
  xh_display *d = xh_display_get();
  if (b->id != g->scanout_resource || !g->scan.width || !d || !d->pixels) return RESP_OK_NODATA;
  if (!rect_ok(rect, g->scan.width, g->scan.height)) return RESP_ERR_INVALID_PARAMETER;
  uint32_t w = rect->x < d->width ? (rect->w < d->width - rect->x ? rect->w : d->width - rect->x) : 0;
  uint32_t hgt = rect->y < d->height ? (rect->h < d->height - rect->y ? rect->h : d->height - rect->y) : 0;
  // Rows straight from the guest's pages into the display's buffer, the blob's segments walked once for the whole
  // rectangle: rows lie at rising offsets. (Each row used to be read into a buffer of its own, the segment list searched
  // from its start for every row: 3120 rows of a 1440x3120 frame over about 1100 segments of 16 KiB pages.)
  // A half-float scanout is 8 bytes a pixel in the guest; the display's pixels are d->format's (display-format=).
  uint32_t status = RESP_OK_NODATA;
  uint64_t t0 = flush_now_us();
  unsigned seg = 0, gbpp = format_bpp(g->scan.format), dformat = display_format(), dbpp = xh_pixels_bpp(dformat);
  uint64_t seg_start = 0;                            // the blob offset at which entries[seg] begins
  for (uint32_t y = 0; y < hgt && w && status == RESP_OK_NODATA; y++) {
    uint64_t at = g->scan.offset + (uint64_t)(rect->y + y) * g->scan.stride + (uint64_t)rect->x * gbpp, left = (uint64_t)w * gbpp;
    uint8_t *to = d->pixels + (size_t)(rect->y + y) * d->stride + (size_t)rect->x * dbpp;
    uint8_t carry[8];                                // a pixel split between two segments
    unsigned have = 0;
    if (at < seg_start) { seg = 0; seg_start = 0; }  // not expected: rows only rise
    while (left) {
      while (seg < b->nentries && at >= seg_start + b->entries[seg].length) seg_start += b->entries[seg++].length;
      if (seg >= b->nentries) { status = RESP_ERR_INVALID_PARAMETER; break; }
      uint64_t in = at - seg_start, n = b->entries[seg].length - in < left ? b->entries[seg].length - in : left;
      const uint8_t *from = xh_virtio_translate(b->entries[seg].addr + in, n);
      if (!from || (n & 3)) { status = RESP_ERR_INVALID_PARAMETER; break; }
      uint64_t off = 0;
      if (have) {
        off = gbpp - have < n ? gbpp - have : n;
        memcpy(carry + have, from, off);
        have += (unsigned)off;
        if (have == gbpp) { convert_to(to, dformat, carry, g->scan.format, 1); to += dbpp; have = 0; }
      }
      uint64_t whole = (n - off) / gbpp, rest = n - off - whole * gbpp;
      convert_to(to, dformat, from + off, g->scan.format, (size_t)whole);
      to += whole * dbpp;
      if (rest) { memcpy(carry, from + off + whole * gbpp, rest); have = (unsigned)rest; }
      at += n; left -= n;
    }
  }
  flush_note(flush_now_us() - t0, (uint64_t)w * hgt * gbpp);
  if (status == RESP_OK_NODATA && w && hgt) {
    frame_shown(g, b->entries, b->nentries);     // first: session/ready before the helper is told of the frame
    xh_display_flush(rect->x, rect->y, w, hgt);
  }
  return status;
}

static void push_pending(gpu_state *g, pending *p)
{
  xh_virtio_lock(g->dev);
  if (p->gen == g->gen) xh_virtio_push(g->dev, 0, p->head, p->written);       // a chain from before a reset is the old queue's
  xh_virtio_unlock(g->dev);
  free(p);
}

// Takes the pending fences that match off the list and returns their commands. ring < 0: every ring of the context.
static void retire(gpu_state *g, uint32_t ctx, int ring, uint64_t fence)
{
  pending *done = NULL, **tail = &done;
  pthread_mutex_lock(&g->pl);
  for (pending **link = &g->fences; *link;) {
    pending *p = *link;
    if (p->ctx == ctx && (ring < 0 || (p->ring == (uint32_t)ring && p->fence <= fence))) { *link = p->next; p->next = NULL; *tail = p; tail = &p->next; }
    else link = &p->next;
  }
  pthread_mutex_unlock(&g->pl);
  while (done) { pending *p = done; done = p->next; push_pending(g, p); }
}

static void gpu_retire(xh_virtio *dev, uint32_t ctx, uint32_t ring, uint64_t fence)
{
  retire(xh_virtio_opaque(dev), ctx, (int)ring, fence);
}

// The 3D commands and the blob ones. 0: not one of them (command() goes on to the 2D ones).
static uint32_t command3d(gpu_state *g, const gpu_hdr *h, const uint8_t *body, size_t left, uint8_t *resp, size_t resp_max, size_t *resp_size)
{
  xh_gpu3d *b = g->b;
  uint32_t a[4];
  switch (h->type) {
  case CMD_GET_CAPSET_INFO: {
    if (left < 8 || resp_max < sizeof *h + 16) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    uint32_t info[4] = { 0 };
    if (b && a[0] < b->capset_count(b)) b->capset_info(b, a[0], &info[0], &info[1], &info[2]);
    memcpy(resp + sizeof *h, info, sizeof info);
    *resp_size = sizeof *h + 16;
    return RESP_OK_CAPSET_INFO;
  }
  case CMD_GET_CAPSET: {
    if (left < 8) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 8);
    size_t n = b ? b->capset(b, a[0], a[1], resp + sizeof *h, resp_max - sizeof *h) : 0;
    if (!n) return RESP_ERR_INVALID_PARAMETER;
    *resp_size = sizeof *h + n;
    return RESP_OK_CAPSET;
  }
  case CMD_CTX_CREATE: {
    if (left < 8) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 8);
    size_t name = left - 8 < 64 ? left - 8 : 64;
    if (a[0] < name) name = a[0];
    if (!b || !h->ctx || ctx_find(g, h->ctx)) return RESP_ERR_INVALID_CONTEXT_ID;
    uint32_t *slot = NULL;
    for (unsigned i = 0; i < MAX_CTX && !slot; i++) if (!g->ctx[i]) slot = &g->ctx[i];
    if (!slot) return RESP_ERR_OUT_OF_MEMORY;
    int r = b->ctx_create(b, h->ctx, a[1] & 0xff, (const char *)body + 8, name);
    if (r) return r == -ENOMEM ? RESP_ERR_OUT_OF_MEMORY : RESP_ERR_UNSPEC;
    pthread_mutex_lock(&g->lanes);                   // the queue's thread reads the table without `cmd` (slow_lane)
    *slot = h->ctx;
    g->ctx_fast[slot - g->ctx] = name >= 14 && !memcmp(body + 8, "xnu:backboardd", 14);
    pthread_mutex_unlock(&g->lanes);
    // context_init's bits 8-23: the creating task's role and its thread's QoS (OneiOSAdreno.cpp oneios_gpu_hint).
    if (b->hint) b->hint(b, h->ctx, (const uint8_t[3]){ (uint8_t)(a[1] >> 8), (uint8_t)(a[1] >> 16), 0 });
    return RESP_OK_NODATA;
  }
  case CMD_CTX_DESTROY: {
    uint32_t *slot = ctx_find(g, h->ctx);
    if (!slot) return RESP_ERR_INVALID_CONTEXT_ID;
    b->ctx_destroy(b, h->ctx);
    pthread_mutex_lock(&g->lanes);
    *slot = 0;
    pthread_mutex_unlock(&g->lanes);
    retire(g, h->ctx, -1, 0);                        // its rings are gone: nothing would retire these
    return RESP_OK_NODATA;
  }
  case CMD_SUBMIT_3D: {
    if (left < 8) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    if (!ctx_find(g, h->ctx)) return RESP_ERR_INVALID_CONTEXT_ID;
    if (!a[0] || a[0] > left - 8) return RESP_ERR_INVALID_PARAMETER;
    if (b->hint) b->hint(b, h->ctx, h->pad);         // the submitting task's role, QoS and frame deadline
    return b->submit(b, h->ctx, body + 8, a[0]) ? RESP_ERR_UNSPEC : RESP_OK_NODATA;
  }
  case CMD_RESOURCE_CREATE_BLOB: {
    uint64_t wide[2];
    if (left < 32) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 16);                             // resource, blob_mem, blob_flags, nr_entries
    memcpy(wide, body + 16, 16);                     // blob_id, size
    if (!a[0] || find(g, a[0]) || blob_find(g, a[0])) return RESP_ERR_INVALID_RESOURCE_ID;
    bool guest = a[1] == XH_GPU_BLOB_GUEST || a[1] == XH_GPU_BLOB_HOST3D_GUEST, host = a[1] == XH_GPU_BLOB_HOST3D || a[1] == XH_GPU_BLOB_HOST3D_GUEST;
    if ((!guest && !host) || !wide[1] || a[3] > MAX_BLOB_ENTRIES || (guest ? !a[3] : a[3] != 0) || left - 32 < (size_t)a[3] * sizeof(gpu_entry)) return RESP_ERR_INVALID_PARAMETER;
    if (host && !ctx_find(g, h->ctx)) return RESP_ERR_INVALID_CONTEXT_ID;
    gpu_entry *entries = NULL;
    if (guest) {
      // The segments are guest RAM, whole host pages each (udmabuf takes nothing else), and cover the blob.
      uint64_t total = 0;
      if (!(entries = malloc(a[3] * sizeof *entries))) return RESP_ERR_OUT_OF_MEMORY;
      memcpy(entries, body + 32, a[3] * sizeof *entries);
      for (unsigned i = 0; i < a[3]; i++) {
        if (!entries[i].length || ((entries[i].addr | entries[i].length) & 0xfff) || !xh_virtio_translate(entries[i].addr, entries[i].length)) { free(entries); return RESP_ERR_INVALID_PARAMETER; }
        total += entries[i].length;
      }
      if (total < wide[1]) { free(entries); return RESP_ERR_INVALID_PARAMETER; }
    }
    blob *nb = blob_add(g, a[0]);
    if (!nb) { free(entries); return RESP_ERR_OUT_OF_MEMORY; }
    // The renderer makes every blob of a context, and a display blob it may be asked to attach to one later.
    int r = b && (host || (a[2] & XH_GPU_BLOB_FLAG_GUEST_HANDLE)) ? b->create_blob(b, host ? h->ctx : 0, a[0], a[1], a[2], wide[0], wide[1], entries, a[3]) : 0;
    if (r && host) { nb->entries = entries; blob_del(g, nb); return r == -ENOMEM ? RESP_ERR_OUT_OF_MEMORY : RESP_ERR_UNSPEC; }
    nb->ctx = host ? h->ctx : 0; nb->mem = a[1]; nb->size = wide[1]; nb->entries = entries; nb->nentries = a[3];
    g->blob_bytes += wide[1];
    g->blobs_made++;
    return RESP_OK_NODATA;
  }
  case CMD_RESOURCE_MAP_BLOB: {
    if (left < 16 || resp_max < sizeof *h + 8) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    blob *bl = blob_find(g, a[0]);
    if (!bl) return RESP_ERR_INVALID_RESOURCE_ID;
    uint64_t offset = 0;
    uint32_t info[2] = { 0 };
    // Only what lies in the pool can be mapped: the guest maps pool base + offset from its own RAM.
    if (!b || bl->mem != XH_GPU_BLOB_HOST3D || b->map_blob(b, a[0], &offset, &info[0]) || offset > UINT32_MAX) return RESP_ERR_INVALID_PARAMETER;
    info[0] |= MAP_INFO_POOL;
    info[1] = (uint32_t)offset;
    memcpy(resp + sizeof *h, info, sizeof info);
    *resp_size = sizeof *h + 8;
    return RESP_OK_MAP_INFO;
  }
  case CMD_RESOURCE_UNMAP_BLOB:
    if (left < 4) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    return blob_find(g, a[0]) ? RESP_OK_NODATA : RESP_ERR_INVALID_RESOURCE_ID;
  case CMD_CTX_ATTACH_RESOURCE: case CMD_CTX_DETACH_RESOURCE: {
    if (left < 4) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    if (!ctx_find(g, h->ctx)) return RESP_ERR_INVALID_CONTEXT_ID;
    if (!blob_find(g, a[0])) return RESP_ERR_INVALID_RESOURCE_ID;
    return b->attach(b, h->ctx, a[0], h->type == CMD_CTX_ATTACH_RESOURCE) ? RESP_ERR_UNSPEC : RESP_OK_NODATA;
  }
  case CMD_RESOURCE_UNREF: {
    if (left < 4) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 4);
    blob *bl = blob_find(g, a[0]);
    if (!bl) return 0;                               // a 2D resource, or nothing
    if (g->scanout_resource == a[0]) { g->scanout_resource = 0; g->scan.width = 0; }
    if (b) b->unref(b, a[0]);
    blob_del(g, bl);
    g->blobs_unref++;
    return RESP_OK_NODATA;
  }
  case CMD_SET_SCANOUT_BLOB: {
    gpu_rect rect;
    uint32_t v[14];                                  // scanout, resource, width, height, format, padding, strides[4], offsets[4]
    if (left < 16 + sizeof v) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&rect, body, 16);
    memcpy(v, body + 16, sizeof v);
    if (v[0] != 0) return RESP_ERR_INVALID_SCANOUT_ID;
    if (v[1] == 0) { g->scanout_resource = 0; g->scan.width = 0; return RESP_OK_NODATA; }
    blob *bl = blob_find(g, v[1]);
    if (!bl || !bl->entries) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!format_known(v[4])) return RESP_ERR_INVALID_PARAMETER;
    uint64_t row = (uint64_t)v[2] * format_bpp(v[4]);
    if (!v[2] || !v[3] || v[2] > 16384 || v[3] > 16384 || v[6] < row || (v[6] % format_bpp(v[4])) || !rect_ok(&rect, v[2], v[3]) ||
        v[10] + (uint64_t)(v[3] - 1) * v[6] + row > bl->size) return RESP_ERR_INVALID_PARAMETER;
    static unsigned half_said;
    if (v[4] == FMT_RGBA16F && g->scan.format != FMT_RGBA16F && half_said++ < 4)
      LOGI("display: the guest scans out half floats (R,G,B,A 16F, %ux%u, stride %u) onto a %s frame", v[2], v[3], v[6],
           xh_pixels_name(display_format()));
    g->scanout_resource = v[1];
    g->scan.width = v[2]; g->scan.height = v[3]; g->scan.format = v[4]; g->scan.stride = v[6]; g->scan.offset = v[10];
    return RESP_OK_NODATA;
  }
  }
  return 0;
}

// The blobs the host holds, one line per 10 s while they change: `gpu: 10 s: host blobs N (M MiB), made +A unref'd +B`.
// Against the guest's `oneios-gpu-memory` live count (and the sum of its own and wired MiB): the host holding more than
// the guest has live is blobs the guest released and the host kept; the same is the guest's own holding.
static void blob_note(gpu_state *g)
{
  uint64_t now = flush_now_us();
  if (!g->blob_note_us) g->blob_note_us = now;
  if (now - g->blob_note_us < 10000000u) return;
  if (g->blobs_made != g->noted_made || g->blobs_unref != g->noted_unref)
    LOGI("gpu: %llu s: host blobs %u (%llu MiB), made +%llu unref'd +%llu", (unsigned long long)((now - g->blob_note_us) / 1000000u),
         g->nblobs, (unsigned long long)(g->blob_bytes >> 20), (unsigned long long)(g->blobs_made - g->noted_made),
         (unsigned long long)(g->blobs_unref - g->noted_unref));
  g->blob_note_us = now;
  g->noted_made = g->blobs_made;
  g->noted_unref = g->blobs_unref;
}

static uint32_t command(gpu_state *g, const uint8_t *req, size_t size, uint8_t *resp, size_t resp_max, size_t *resp_size)
{
  const gpu_hdr *h = (const gpu_hdr *)req;
  const uint8_t *body = req + sizeof *h;
  size_t left = size - sizeof *h;
  *resp_size = sizeof *h;
  if (g->blobs) {
    blob_note(g);
    uint32_t status = command3d(g, h, body, left, resp, resp_max, resp_size);
    if (status) return status;
  }
  switch (h->type) {
  case CMD_GET_DISPLAY_INFO: {
    if (resp_max < sizeof *h + 16 * 24) return RESP_ERR_UNSPEC;
    memset(resp + sizeof *h, 0, 16 * 24);
    uint32_t mode[6] = { 0, 0, g->width, g->height, 1, 0 };
    memcpy(resp + sizeof *h, mode, sizeof mode);
    *resp_size = sizeof *h + 16 * 24;
    return RESP_OK_DISPLAY_INFO;
  }
  case CMD_RESOURCE_CREATE_2D: {
    uint32_t a[4];
    if (left < 16) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 16);
    if (!a[0] || find(g, a[0]) || (g->blobs && blob_find(g, a[0]))) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!a[2] || !a[3] || a[2] > 16384 || a[3] > 16384) return RESP_ERR_INVALID_PARAMETER;
    if (!format_known(a[1])) return RESP_ERR_INVALID_PARAMETER;
    resource *r = NULL;
    for (unsigned i = 0; i < MAX_RES && !r; i++) if (!g->res[i].id) r = &g->res[i];
    if (!r) return RESP_ERR_OUT_OF_MEMORY;
    r->dformat = display_format();
    r->dbpp = xh_pixels_bpp(r->dformat);
    r->pixels = calloc((size_t)a[2] * a[3], r->dbpp);
    if (!r->pixels) return RESP_ERR_OUT_OF_MEMORY;
    r->id = a[0]; r->format = a[1]; r->width = a[2]; r->height = a[3];
    return RESP_OK_NODATA;
  }
  case CMD_RESOURCE_UNREF: {
    uint32_t id;
    if (left < 4) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&id, body, 4);
    resource *r = find(g, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (g->scanout_resource == id) g->scanout_resource = 0;
    release(r);
    return RESP_OK_NODATA;
  }
  case CMD_RESOURCE_ATTACH_BACKING: {
    uint32_t a[2];
    if (left < 8) return RESP_ERR_INVALID_PARAMETER;
    memcpy(a, body, 8);
    resource *r = find(g, a[0]);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!a[1] || a[1] > MAX_BACKING || left - 8 < (size_t)a[1] * sizeof(gpu_entry)) return RESP_ERR_INVALID_PARAMETER;
    gpu_entry *entries = malloc(a[1] * sizeof *entries);
    if (!entries) return RESP_ERR_OUT_OF_MEMORY;
    memcpy(entries, body + 8, a[1] * sizeof *entries);
    free(r->backing);
    r->backing = entries;
    r->nbacking = a[1];
    return RESP_OK_NODATA;
  }
  case CMD_RESOURCE_DETACH_BACKING: {
    uint32_t id;
    if (left < 4) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&id, body, 4);
    resource *r = find(g, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    free(r->backing);
    r->backing = NULL;
    r->nbacking = 0;
    return RESP_OK_NODATA;
  }
  case CMD_SET_SCANOUT: {
    gpu_rect rect;
    uint32_t a[2];
    if (left < 24) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&rect, body, 16);
    memcpy(a, body + 16, 8);
    if (a[0] != 0) return RESP_ERR_INVALID_SCANOUT_ID;
    if (a[1] == 0) { g->scanout_resource = 0; return RESP_OK_NODATA; }      // scanout disabled
    resource *r = find(g, a[1]);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect_ok(&rect, r->width, r->height)) return RESP_ERR_INVALID_PARAMETER;
    g->scanout_resource = a[1];
    g->scan.width = 0;
    return RESP_OK_NODATA;
  }
  case CMD_TRANSFER_TO_HOST_2D: {
    gpu_rect rect;
    uint64_t offset;
    uint32_t id;
    if (left < 28) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&rect, body, 16);
    memcpy(&offset, body + 16, 8);
    memcpy(&id, body + 24, 4);
    resource *r = find(g, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect_ok(&rect, r->width, r->height) || !r->backing) return RESP_ERR_INVALID_PARAMETER;
    unsigned gbpp = format_bpp(r->format);
    uint8_t *row = malloc((size_t)rect.w * gbpp);
    if (!row) return RESP_ERR_OUT_OF_MEMORY;
    uint32_t status = RESP_OK_NODATA;
    for (uint32_t y = 0; y < rect.h; y++) {
      // `offset` is where the rectangle's first pixel is in the backing; rows follow at the resource's stride.
      if (!backing_read(r, offset + (uint64_t)y * r->width * gbpp, row, (uint64_t)rect.w * gbpp)) { status = RESP_ERR_INVALID_PARAMETER; break; }
      convert_to(r->pixels + ((size_t)(rect.y + y) * r->width + rect.x) * r->dbpp, r->dformat, row, r->format, rect.w);
    }
    free(row);
    return status;
  }
  case CMD_RESOURCE_FLUSH: {
    gpu_rect rect;
    uint32_t id;
    if (left < 20) return RESP_ERR_INVALID_PARAMETER;
    memcpy(&rect, body, 16);
    memcpy(&id, body + 16, 4);
    if (g->blobs && blob_find(g, id)) return flush_blob(g, blob_find(g, id), &rect);
    resource *r = find(g, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect_ok(&rect, r->width, r->height)) return RESP_ERR_INVALID_PARAMETER;
    xh_display *d = xh_display_get();
    if (id == g->scanout_resource && d && d->pixels && r->dbpp == xh_pixels_bpp(display_format())) {
      uint32_t w = rect.x < d->width ? (rect.w < d->width - rect.x ? rect.w : d->width - rect.x) : 0;
      uint32_t hgt = rect.y < d->height ? (rect.h < d->height - rect.y ? rect.h : d->height - rect.y) : 0;
      for (uint32_t y = 0; y < hgt && w; y++)
        memcpy(d->pixels + (size_t)(rect.y + y) * d->stride + (size_t)rect.x * r->dbpp,
               r->pixels + ((size_t)(rect.y + y) * r->width + rect.x) * r->dbpp, (size_t)w * r->dbpp);
      frame_shown(g, r->backing, r->nbacking);   // first: session/ready before the helper is told of the frame
      if (w && hgt) xh_display_flush(rect.x, rect.y, w, hgt);
    }
    return RESP_OK_NODATA;
  }
  }
  return RESP_ERR_UNSPEC;
}

// Runs the command of a chain and writes its response. Returns the response's length (0: a chain that is no command)
// and the command's header in `in`.
static int64_t serve(gpu_state *g, unsigned queue, xh_viov *iov, unsigned n, gpu_hdr *in, uint32_t *status)
{
  // Gather the readable parts (the command), find the writable ones (the response).
  size_t req_size = 0, resp_max = 0;
  for (unsigned i = 0; i < n; i++) { if (iov[i].writable) resp_max += iov[i].length; else req_size += iov[i].length; }
  if (req_size < sizeof(gpu_hdr) || req_size > (1u << 20) || resp_max < sizeof(gpu_hdr)) return 0;
  uint8_t *req = malloc(req_size), *resp = calloc(1, resp_max < 4096 ? 4096 : resp_max);
  if (!req || !resp) { free(req); free(resp); return 0; }
  size_t at = 0;
  for (unsigned i = 0; i < n; i++) if (!iov[i].writable) { memcpy(req + at, iov[i].data, iov[i].length); at += iov[i].length; }
  size_t resp_size = sizeof(gpu_hdr);
  gpu_hdr out = { 0 };
  memcpy(in, req, sizeof *in);
  out.type = queue == 0 ? command(g, req, req_size, resp, resp_max, &resp_size) : RESP_OK_NODATA;     // cursor queue: accepted, not drawn
  if (in->flags & FLAG_FENCE) { out.flags = in->flags & (FLAG_FENCE | FLAG_RING_IDX); out.fence = in->fence; out.ctx = in->ctx; out.ring = in->ring; }
  memcpy(resp, &out, sizeof out);
  if (resp_size > resp_max) resp_size = resp_max;
  at = 0;
  for (unsigned i = 0; i < n && at < resp_size; i++) {
    if (!iov[i].writable) continue;
    size_t len = resp_size - at < iov[i].length ? resp_size - at : iov[i].length;
    memcpy(iov[i].data, resp + at, len);
    at += len;
  }
  free(req);
  free(resp);
  *status = out.type;
  return (int64_t)resp_size;
}

static int64_t gpu_handle(xh_virtio *dev, unsigned queue, uint16_t head, xh_viov *iov, unsigned n)
{
  (void)head;
  gpu_hdr in;
  uint32_t status;
  return serve(xh_virtio_opaque(dev), queue, iov, n, &in, &status);
}

static uint32_t gpu_config(xh_virtio *dev, uint32_t offset, unsigned size)
{
  (void)size;
  gpu_state *g = xh_virtio_opaque(dev);
  // events_read, events_clear, num_scanouts = 1, num_capsets
  return offset == 8 ? 1 : offset == 12 && g->b ? g->b->capset_count(g->b) : 0;
}

static void teardown(gpu_state *g)
{
  for (unsigned i = 0; i < MAX_RES; i++) if (g->res[i].id) release(&g->res[i]);
  g->scanout_resource = 0;
  g->scan.width = 0;
  if (!g->blobs) return;
  for (unsigned i = 0; i < BLOB_SLOTS; i++) if (g->blobs[i].id) {
    if (g->b) g->b->unref(g->b, g->blobs[i].id);
    free(g->blobs[i].entries);
  }
  memset(g->blobs, 0, BLOB_SLOTS * sizeof *g->blobs);
  g->nblobs = 0;
  g->blob_bytes = 0;
  for (unsigned i = 0; i < MAX_CTX; i++) if (g->ctx[i]) {
    g->b->ctx_destroy(g->b, g->ctx[i]);
    pthread_mutex_lock(&g->lanes);
    g->ctx[i] = 0;
    pthread_mutex_unlock(&g->lanes);
  }
  pthread_mutex_lock(&g->pl);
  pending *list = g->fences;
  g->fences = NULL;
  pthread_mutex_unlock(&g->pl);
  while (list) { pending *p = list; list = p->next; free(p); }                // their chains were the old queue's
}

static void gpu_reset(xh_virtio *dev) { teardown(xh_virtio_opaque(dev)); }

// ---- the 3D device's control queue: its own thread -------------------------------------------------------------------
static void wake(gpu_state *g, bool reset)
{
  pthread_mutex_lock(&g->pl);
  g->kicks++;
  if (reset) g->reset_requested = true;
  pthread_mutex_unlock(&g->pl);
  uint64_t one = 1;
  while (write(g->wake, &one, 8) < 0 && errno == EINTR) {}
}

static void gpu_kick(xh_virtio *dev, unsigned queue) { (void)queue; wake(xh_virtio_opaque(dev), false); }

// With the device's lock held (virtio.c): the queues are already gone; the worker forgets the rest before its next command.
static void gpu_reset_3d(xh_virtio *dev)
{
  gpu_state *g = xh_virtio_opaque(dev);
  g->gen++;
  wake(g, true);
}

// A command of the fast lane that names a resource the slow lane has not made yet (an app's IOSurface the render
// server attaches, the unref of an app's blob) waits for the slow lane's commands that came before it, and runs again.
static bool missing_resource(uint32_t status)
{
  return status == RESP_ERR_INVALID_RESOURCE_ID;
}

enum { RESP_PENDING_RETRY = 1 };                     // work(): not answered; run it again once the slow lane caught up

static bool slow_pending(gpu_state *g)
{
  pthread_mutex_lock(&g->pl);
  bool pending = g->slow_done != g->slow_queued;
  pthread_mutex_unlock(&g->pl);
  return pending;
}

// Serves a chain and answers it (now, or when its fence retires). With `may_wait`, a command that names a resource
// the slow lane may still be making is not answered: RESP_PENDING_RETRY, and the caller runs it again later.
static uint32_t work(gpu_state *g, uint16_t head, xh_viov *iov, unsigned n, uint64_t gen, bool may_wait)
{
  gpu_hdr in;
  uint32_t status = 0;
  int64_t written = serve(g, 0, iov, n, &in, &status);
  if (may_wait && missing_resource(status) && slow_pending(g)) return RESP_PENDING_RETRY;
  // A command fenced on a context's ring is returned when the ring passes the fence; any other at once (a fence on
  // the device's own ring has nothing to wait for: the 2D commands are done when they return).
  bool hold = written > 0 && status < RESP_ERR_UNSPEC && (in.flags & FLAG_FENCE) && (in.flags & FLAG_RING_IDX) && ctx_find(g, in.ctx);
  pending *p = calloc(1, sizeof *p);
  if (!p) return status;                             // the chain is lost; so is the guest's command
  *p = (pending){ .head = head, .written = (uint32_t)written, .ctx = in.ctx, .ring = in.ring, .fence = in.fence, .gen = gen };
  if (!hold) { push_pending(g, p); return status; }
  pthread_mutex_lock(&g->pl);
  p->next = g->fences;
  g->fences = p;
  pthread_mutex_unlock(&g->pl);
  // May retire it before it returns. A refused fence is retired here.
  if (g->b->submit_fence(g->b, in.ctx, in.ring, in.fence)) retire(g, in.ctx, (int)in.ring, in.fence);
  return status;
}

// The header of a chain, read without taking it (false: shorter than a header).
static bool peek_hdr(const xh_viov *iov, unsigned n, gpu_hdr *h)
{
  size_t at = 0;
  for (unsigned i = 0; i < n && at < sizeof *h; i++) {
    if (iov[i].writable) continue;
    size_t k = iov[i].length < sizeof *h - at ? iov[i].length : sizeof *h - at;
    memcpy((uint8_t *)h + at, iov[i].data, k);
    at += k;
  }
  return at == sizeof *h;
}

// Whether a command is the slow lane's: a context's, and not the render server's. Context creation stays here (it
// decides the lane of what follows).
static bool slow_lane(gpu_state *g, const gpu_hdr *h)
{
  if (!h->ctx || h->type == CMD_CTX_CREATE) return false;
  for (unsigned i = 0; i < MAX_CTX; i++) if (g->ctx[i] == h->ctx) return !g->ctx_fast[i];
  return false;
}

// The render server's lane and the device's lock, one line per 10 s while it runs commands:
//   gpu: 10 s: the render server's lane: N commands, K waited over 1 ms for an app's command (longest X us); the app
//        lane gave way Y times
// K and X are what an app's blobs still cost the frames (at most the one in progress per command: slow_lock); Y is how
// often the slow lane let the render server go first. Under `cmd`.
static void lane_note(gpu_state *g, uint64_t now)
{
  if (!g->lane_note_us) g->lane_note_us = now;
  if (now - g->lane_note_us < 10000000u) return;
  uint64_t gave = atomic_load(&g->gave_way);
  LOGI("gpu: %llu s: the render server's lane: %llu commands, %llu waited over 1 ms for an app's command (longest %llu us); the app lane gave way %llu times",
       (unsigned long long)((now - g->lane_note_us) / 1000000u), (unsigned long long)g->lane_cmds, (unsigned long long)g->lane_waits,
       (unsigned long long)g->lane_wait_max, (unsigned long long)(gave - g->gave_noted));
  g->lane_note_us = now;
  g->lane_cmds = g->lane_waits = g->lane_wait_max = 0;
  g->gave_noted = gave;
}

// `cmd` for the queue's thread (the render server's commands, the display's, a reset). It counts itself waiting first,
// so that the slow lane, between two of an app's commands, gives way instead of taking the lock back (slow_lock).
static void fast_lock(gpu_state *g)
{
  atomic_fetch_add(&g->fast_waiting, 1);
  uint64_t asked = flush_now_us();
  pthread_mutex_lock(&g->cmd);
  uint64_t now = flush_now_us();
  // The slow lane may be waiting for its turn: it goes on to the lock (and waits there until this command is done).
  if (atomic_fetch_sub(&g->fast_waiting, 1) == 1 && atomic_load(&g->slow_yield)) {
    pthread_mutex_lock(&g->pl);
    pthread_cond_broadcast(&g->turn_cv);
    pthread_mutex_unlock(&g->pl);
  }
  g->lane_cmds++;
  if (now - asked > 1000u) g->lane_waits++;
  if (now - asked > g->lane_wait_max) g->lane_wait_max = now - asked;
  lane_note(g, now);
}

// `cmd` for the slow lane: after the render server's lane while that waits for it. `slow_yield` is set before
// `fast_waiting` is read again under `pl`, and fast_lock reads `slow_yield` after its decrement, so one of the two sees
// the other (sequentially consistent atomics) and the broadcast is not lost.
static void slow_lock(gpu_state *g)
{
  if (atomic_load(&g->fast_waiting)) {
    bool gave = false;
    pthread_mutex_lock(&g->pl);
    atomic_store(&g->slow_yield, true);
    while (atomic_load(&g->fast_waiting)) { gave = true; pthread_cond_wait(&g->turn_cv, &g->pl); }
    atomic_store(&g->slow_yield, false);
    pthread_mutex_unlock(&g->pl);
    if (gave) atomic_fetch_add(&g->gave_way, 1);
  }
  pthread_mutex_lock(&g->cmd);
}

static void *slow_worker(void *arg)
{
  gpu_state *g = arg;
  pthread_mutex_lock(&g->pl);
  for (;;) {
    while (!g->slow_head) pthread_cond_wait(&g->slow_cv, &g->pl);
    slow_job *j = g->slow_head;
    g->slow_head = j->next;
    if (!g->slow_head) g->slow_tail = &g->slow_head;
    pthread_mutex_unlock(&g->pl);
    xh_virtio_lock(g->dev);
    bool current = j->gen == g->gen;                 // a chain from before a reset is the old queue's: dropped
    xh_virtio_unlock(g->dev);
    if (current) {
      slow_lock(g);
      work(g, j->head, j->iov, j->n, j->gen, false);
      pthread_mutex_unlock(&g->cmd);
    }
    free(j);
    pthread_mutex_lock(&g->pl);
    g->slow_done++;
    pthread_cond_broadcast(&g->slow_cv);
    pthread_cond_broadcast(&g->idle);
  }
  return NULL;
}

// The frame path's thread at the top clock while it runs: uclamp.min 1024 on the calling thread (sched_setattr(2),
// Linux 5.3; KEEP_POLICY and KEEP_PARAMS: SCHED_OTHER and its nice stay, never a realtime policy, CLAUDE.md). This
// thread pops every command of the queue, runs the render server's (its submissions) and copies each scanout flush
// (flush_blob, about 1 ms for 1080x2340 at the top clock); the vCPUs carry their own clamps (cpu.c), this thread had
// none. With One UI's CPU floors left to the governor (the switch's clocks_tops writes cpuinfo_min_freq, not the top,
// since the S25 Ultra's sessions of 2026-11-19 ran at 93-105 C with the floors pinned), a copy started on a core at a
// low clock would wait for the governor's ramp; the clamp only counts while the thread runs, so an idle queue costs
// nothing. Returns 0 or -errno; the caller logs it.
static int frame_path_clamp(void)
{
#if defined(__linux__) && defined(SYS_sched_setattr)
  struct { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t priority; uint64_t runtime, deadline, period;
           uint32_t util_min, util_max; } attr = { .size = 56, .flags = 0x18u | 0x20u, .util_min = 1024 };
  _Static_assert(sizeof attr == 56, "struct sched_attr, SCHED_ATTR_SIZE_VER1");
  return syscall(SYS_sched_setattr, 0, &attr, 0) ? -errno : 0;
#else
  return -ENOSYS;
#endif
}

static void *worker(void *arg)
{
  gpu_state *g = arg;
  static _Thread_local xh_viov iov[WORKER_IOV];
  int clamp = frame_path_clamp();
  if (clamp) LOGI("gpu: the queue's thread (the render server's commands, the scanout copy) keeps the governor's clock: uclamp.min not set (%s)", strerror(-clamp));
  else LOGI("gpu: the queue's thread (the render server's commands, the scanout copy) at uclamp.min 1024 while it runs");
  for (;;) {
    uint64_t count;
    if (read(g->wake, &count, 8) < 0 && errno != EINTR) break;
    pthread_mutex_lock(&g->pl);
    uint64_t upto = g->kicks;
    bool reset = g->reset_requested;
    g->reset_requested = false;
    pthread_mutex_unlock(&g->pl);
    if (reset) { fast_lock(g); teardown(g); pthread_mutex_unlock(&g->cmd); }
    for (;;) {
      uint16_t head;
      xh_virtio_lock(g->dev);
      uint64_t gen = g->gen;
      unsigned n = xh_virtio_pop(g->dev, 0, &head, iov, WORKER_IOV);
      xh_virtio_unlock(g->dev);
      if (!n) break;
      gpu_hdr h;
      pthread_mutex_lock(&g->lanes);                 // not `cmd`: an app's blob in progress does not hold back the choice
      bool slow = peek_hdr(iov, n, &h) && slow_lane(g, &h);
      pthread_mutex_unlock(&g->lanes);
      if (slow) {
        slow_job *j = malloc(sizeof *j + n * sizeof *iov);
        if (!j) continue;                            // the chain is lost; so is the guest's command
        *j = (slow_job){ .head = head, .n = n, .gen = gen };
        memcpy(j->iov, iov, n * sizeof *iov);
        pthread_mutex_lock(&g->pl);
        *g->slow_tail = j;
        g->slow_tail = &j->next;
        g->slow_queued++;
        pthread_cond_broadcast(&g->slow_cv);
        pthread_mutex_unlock(&g->pl);
        continue;
      }
      pthread_mutex_lock(&g->pl);
      uint64_t before = g->slow_queued;              // the slow lane's commands that came before this one
      pthread_mutex_unlock(&g->pl);
      fast_lock(g);
      uint32_t status = work(g, head, iov, n, gen, true);
      pthread_mutex_unlock(&g->cmd);
      if (status == RESP_PENDING_RETRY) {
        pthread_mutex_lock(&g->pl);
        while (g->slow_done < before) pthread_cond_wait(&g->slow_cv, &g->pl);
        pthread_mutex_unlock(&g->pl);
        fast_lock(g);
        work(g, head, iov, n, gen, false);
        pthread_mutex_unlock(&g->cmd);
      }
    }
    pthread_mutex_lock(&g->pl);
    g->served = upto;
    pthread_cond_broadcast(&g->idle);
    pthread_mutex_unlock(&g->pl);
  }
  return NULL;
}

void xh_virtio_gpu_idle(xh_virtio *dev)
{
  gpu_state *g = xh_virtio_opaque(dev);
  if (!g->blobs) return;
  pthread_mutex_lock(&g->pl);
  while (g->served != g->kicks || g->slow_done != g->slow_queued) pthread_cond_wait(&g->idle, &g->pl);
  pthread_mutex_unlock(&g->pl);
}

static const xh_virtio_ops gpu_ops = { .device_id = 16, .features = 0, .nqueues = 2, .handle = gpu_handle,
                                       .config_read = gpu_config, .reset = gpu_reset };
static const xh_virtio_ops gpu_3d_ops = { .device_id = 16, .features = F_VIRGL | F_RESOURCE_BLOB | F_CONTEXT_INIT | F_CREATE_GUEST_HANDLE, .nqueues = 2,
                                          .handle = gpu_handle, .config_read = gpu_config, .reset = gpu_reset_3d, .manual = 1, .kick = gpu_kick };

xh_virtio *xh_virtio_gpu_create(uint64_t base, unsigned spi, unsigned width, unsigned height)
{
  gpu_state *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  g->width = width;
  g->height = height;
  return xh_virtio_create(base, spi, &gpu_ops, g, "virtio-gpu");
}

xh_virtio *xh_virtio_gpu_create_3d(uint64_t base, unsigned spi, unsigned width, unsigned height, xh_gpu3d *b)
{
  if (!b) return xh_virtio_gpu_create(base, spi, width, height);
  gpu_state *g = calloc(1, sizeof *g);
  pthread_t thread;
  if (!g) return NULL;
  g->width = width;
  g->height = height;
  g->b = b;
  g->blobs = calloc(BLOB_SLOTS, sizeof *g->blobs);
  g->wake = eventfd(0, EFD_CLOEXEC);
  pthread_mutex_init(&g->pl, NULL);
  pthread_cond_init(&g->idle, NULL);
  pthread_mutex_init(&g->cmd, NULL);
  pthread_cond_init(&g->slow_cv, NULL);
  pthread_mutex_init(&g->lanes, NULL);
  pthread_cond_init(&g->turn_cv, NULL);
  g->slow_tail = &g->slow_head;
  if (!g->blobs || g->wake < 0 || !(g->dev = xh_virtio_create(base, spi, &gpu_3d_ops, g, "virtio-gpu"))) return NULL;
  b->retire = gpu_retire;
  b->dev = g->dev;
  if (pthread_create(&thread, NULL, worker, g)) return NULL;
  pthread_detach(thread);
  if (pthread_create(&thread, NULL, slow_worker, g)) return NULL;
  pthread_detach(thread);
  return g->dev;
}
