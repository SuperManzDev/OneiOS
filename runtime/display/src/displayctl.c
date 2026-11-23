// SPDX-License-Identifier: MIT
// displayctl: the display helper's servers on the phone. Each stands in for xnuhost: a frame in a memfd, served on a
// display.sock of its own, which oneios-display shows on a top layer over One UI (README.md).
//   displayctl test [--seconds N] [--size WxH] [--format bgrx|rgba16f] [--helper PATH] [-- helper options]
//     a moving pattern: the helper's own test.
//   displayctl pool --pid PID --pool-offset N --pool-bytes N --size WxH --stride N --seq-offset N [--fb-offset N]
//                   [--seconds N] [--helper PATH] [-- helper options]
//     the screen of the native kernel in a VM of Android's crosvm (docs/native.md, the display; `oneios-tool phone
//     vm-boot --display` starts it beside crosvm). A protected VM's memory is lent to the guest, and a host access to it
//     faults, all but crosvm's restricted DMA pool, where the VM's boot video lies (xnuloader-crosvm --display). crosvm
//     maps the pool as a mapping of its own of the guest's memfd: this finds that mapping in /proc/PID/maps (the pool's
//     offset in the memfd and its size must be the mapping's), opens the memfd through /proc/PID/map_files and maps the
//     boot video and the frame count's page, read-only: nothing else of the VM. The guest counts the frames it writes
//     into the boot video (OneiOSDisplay.cpp oneios_display_frame_done): a new count is a new frame, and the rows that
//     changed are copied into the helper's frame and flushed. Until the count is there (the kernel's console during
//     boot) the frame is compared four times a second. Pages that are not resident are not read: no read here makes
//     the host allocate the VM's memory. It ends with the VM (crosvm's process gone), at --seconds, on SIGTERM or
//     SIGINT, or when the helper leaves. --test-guest FILE (the host tests): a plain file stands for crosvm's memfd.
// The layer goes away when the socket closes; the helper is ended if it does not leave by itself.
#define _GNU_SOURCE
#include "xhd.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint32_t W = 1440, H = 3120;     // the guest's frame: the panel's native mode
static uint32_t FORMAT = XHD_FORMAT_BGRX, BPP = 4;
static uint8_t *pixels;
static volatile int interrupted;

// The wide band (--format rgba16f): the lowest quarter of the frame, eight patches from the left: sRGB's red, Display
// P3's red, sRGB's green, P3's green, sRGB's blue, P3's blue (as linear extended sRGB: components below 0 and above 1),
// white at 1.0 and white at 2.0 (EDR). On a path that keeps wide colour each P3 patch is more saturated than the
// sRGB one beside it; on one that clips to sRGB the two are the same. The 2.0 white is brighter than the 1.0 one only
// where surfaceflinger gives the layer HDR headroom.
static const float wide[8][3] = {
  { 1, 0, 0 }, { 1.2249f, -0.0420f, -0.0197f }, { 0, 1, 0 }, { -0.2247f, 1.0419f, -0.0786f },
  { 0, 0, 1 }, { 0, 0, 1.0979f }, { 1, 1, 1 }, { 2, 2, 2 },
};

// One pixel from 8-bit sRGB bytes (b, g, r) in the frame's format.
static void put8(uint8_t *p, uint8_t b, uint8_t g, uint8_t r)
{
  if (FORMAT == XHD_FORMAT_BGRX) { p[0] = b; p[1] = g; p[2] = r; p[3] = 255; return; }
  const uint16_t *t = xh_srgb8_to_half();
  uint16_t px[4] = { t[r], t[g], t[b], 0x3c00u };
  memcpy(p, px, 8);
}
static void on_signal(int n) { (void)n; interrupted = 1; }

static double now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void nap_ms(long ms)
{
  struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&t, NULL);
}

// Eight colour bars with a white frame two pixels wide (so a cut-off edge shows), B,G,R,X.
static void background(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  static const uint8_t bars[8][3] = { { 255, 255, 255 }, { 0, 255, 255 }, { 255, 255, 0 }, { 0, 255, 0 },
                                      { 255, 0, 255 }, { 0, 0, 255 }, { 255, 0, 0 }, { 40, 40, 40 } };
  for (uint32_t row = y; row < y + h && row < H; row++) {
    uint8_t *p = pixels + (size_t)row * W * BPP + (size_t)x * BPP;
    for (uint32_t col = x; col < x + w && col < W; col++, p += BPP) {
      int edge = col < 2 || row < 2 || col >= W - 2 || row >= H - 2;
      if (!edge && FORMAT == XHD_FORMAT_RGBA16F && row >= H - H / 4) {
        const float *c = wide[(uint64_t)col * 8 / W];
        uint16_t px[4] = { xh_half_from_float(c[0]), xh_half_from_float(c[1]), xh_half_from_float(c[2]), 0x3c00u };
        memcpy(p, px, 8);
        continue;
      }
      const uint8_t *c = bars[(uint64_t)col * 8 / W];
      uint8_t shade = (uint8_t)(128 + (uint64_t)row * 127 / H);      // darker at the top
      if (edge) put8(p, 255, 255, 255);
      else put8(p, (uint8_t)(c[0] * shade / 255), (uint8_t)(c[1] * shade / 255), (uint8_t)(c[2] * shade / 255));
    }
  }
}

static void box(uint32_t x, uint32_t y, uint32_t side)
{
  for (uint32_t row = y; row < y + side && row < H; row++) {
    uint8_t *p = pixels + (size_t)row * W * BPP + (size_t)x * BPP;
    for (uint32_t col = x; col < x + side && col < W; col++, p += BPP) put8(p, 255, 255, 255);
  }
}

static int usage(void)
{
  fprintf(stderr, "usage: displayctl test [--seconds N] [--size WxH] [--format bgrx|rgba16f] [--helper PATH] [-- helper options]\n"
                  "       displayctl pool --pid PID --pool-offset N --pool-bytes N --size WxH --stride N --seq-offset N [--fb-offset N]\n"
                  "                       [--seconds N] [--helper PATH] [-- helper options]\n");
  return 2;
}

// ---- the helper, for both modes

// Only non-disruptive display options may reach the helper. In particular --takeover changes panel ownership and
// --session/--socket override the diagnostic endpoint (and can re-enable the production ready-file hold).
// 0 the options may go, 1 a usage error, 2 refused (said).
static int helper_options(const char *mode, int argc, char **argv, int extra)
{
  for (int i = extra; i < argc; i++) {
    const char *opt = argv[i];
    if (!strcmp(opt, "--no-projection")) continue;
    if (!strcmp(opt, "--hz") || !strcmp(opt, "--format") || !strcmp(opt, "--dataspace")) {
      if (++i >= argc) return 1;
      continue;
    }
    fprintf(stderr, "displayctl: refusing %s: %s mode only accepts --hz, --format, --dataspace or --no-projection\n", opt, mode);
    return 2;
  }
  return 0;
}

// The helper: --helper's, or oneios-display beside this program. 0, or -1 (said).
static int find_helper(char *helper, size_t size)
{
  if (!*helper) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { perror("displayctl: /proc/self/exe"); return -1; }
    self[n] = 0;
    snprintf(helper, size, "%s/oneios-display", dirname(self));
  }
  if (access(helper, X_OK)) { fprintf(stderr, "displayctl: %s: %s\n", helper, strerror(errno)); return -1; }
  return 0;
}

struct served {
  char session[PATH_MAX], path[PATH_MAX + 32];
  int server, client;
  pid_t child;
};

// A session folder of its own, display.sock in it, and the helper started on it. 0, or -1 (said).
static int serve(struct served *s, const char *helper, int argc, char **argv, int extra)
{
  s->server = s->client = -1;
  s->child = -1;
  s->path[0] = 0;
  const char *tmp = getenv("TMPDIR");
  snprintf(s->session, sizeof s->session, "%s/displayctl.XXXXXX", tmp && *tmp ? tmp : "/data/local/tmp");
  if (!mkdtemp(s->session)) { fprintf(stderr, "displayctl: %s: %s\n", s->session, strerror(errno)); s->session[0] = 0; return -1; }
  snprintf(s->path, sizeof s->path, "%s/display.sock", s->session);
  s->server = xhd_listen(s->path);
  if (s->server < 0) { fprintf(stderr, "displayctl: socket: %s\n", strerror(-s->server)); return -1; }
  s->child = fork();
  if (s->child < 0) { perror("displayctl: fork"); return -1; }
  if (s->child == 0) {
    char **args = calloc((size_t)(argc - extra) + 6, sizeof *args);
    int k = 0;
    args[k++] = (char *)helper;
    // The frame is already there; only a real guest session needs session/ready. --socket tells OneiOSDisplay to post
    // the first frame immediately instead of waiting for an XNU-created marker forever.
    args[k++] = "--socket";
    args[k++] = s->path;
    args[k++] = "--wait";
    args[k++] = "5";
    for (int i = extra; i < argc; i++) args[k++] = argv[i];
    execv(helper, args);
    fprintf(stderr, "displayctl: %s: %s\n", helper, strerror(errno));
    _exit(127);
  }
  return 0;
}

// The helper is a Java runtime starting up: a second or two. 0 it connected, or -1 (said).
static int accept_helper(struct served *s)
{
  struct pollfd p = { s->server, POLLIN, 0 };
  double deadline = now() + 20;
  int status = 0;
  while (s->client < 0 && now() < deadline && !interrupted) {
    if (waitpid(s->child, &status, WNOHANG) == s->child) {
      fprintf(stderr, "displayctl: the helper ended before it connected (status %d)\n", status);
      s->child = -1;
      return -1;
    }
    if (poll(&p, 1, 200) > 0) s->client = accept4(s->server, NULL, NULL, SOCK_CLOEXEC);
  }
  if (s->client < 0) { fprintf(stderr, "displayctl: the helper did not connect\n"); return -1; }
  return 0;
}

// The session ends: the helper removes its layer and exits, or is ended 3 s after the socket closed. 1 when it did not
// leave by itself or left with a failure.
static int end_session(struct served *s)
{
  int failed = 0;
  if (s->client >= 0) close(s->client);
  if (s->server >= 0) close(s->server);
  if (s->child > 0) {
    int code = -1, waited = 0;
    for (int i = 0; i < 30 && !waited; i++) {
      if (waitpid(s->child, &code, WNOHANG) == s->child) waited = 1;
      else nap_ms(100);
    }
    if (!waited) {
      fprintf(stderr, "displayctl: the helper did not leave within 3 s of the socket closing; ending it\n");
      kill(s->child, SIGTERM);
      sleep(1);
      if (waitpid(s->child, &code, WNOHANG) != s->child) { kill(s->child, SIGKILL); waitpid(s->child, &code, 0); }
      failed = 1;
    } else if (!WIFEXITED(code) || WEXITSTATUS(code)) {
      fprintf(stderr, "displayctl: the helper ended with status 0x%x\n", code);
      failed = 1;
    } else printf("displayctl: the helper removed its layer and exited 0\n");
  }
  if (s->path[0]) unlink(s->path);
  if (s->session[0]) rmdir(s->session);
  return failed;
}

// ---- test: a moving pattern

static int test_main(int argc, char **argv)
{
  double seconds = 5;
  char helper[PATH_MAX] = "";
  int extra = argc;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--")) { extra = i + 1; break; }
    if (i + 1 >= argc) return usage();
    if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--size")) { if (sscanf(argv[++i], "%ux%u", &W, &H) != 2) return usage(); }
    else if (!strcmp(argv[i], "--helper")) snprintf(helper, sizeof helper, "%s", argv[++i]);
    else if (!strcmp(argv[i], "--format")) {
      const char *v = argv[++i];
      if (!strcmp(v, "bgrx")) FORMAT = XHD_FORMAT_BGRX;
      else if (!strcmp(v, "rgba16f")) FORMAT = XHD_FORMAT_RGBA16F;
      else return usage();
      BPP = xh_pixels_bpp(FORMAT);
    }
    else return usage();
  }
  if (W < 16 || H < 16 || W > 8192 || H > 8192 || seconds <= 0 || seconds > 3600) return usage();
  int allowed = helper_options("test", argc, argv, extra);
  if (allowed == 1) return usage();
  if (allowed) return 2;
  if (find_helper(helper, sizeof helper)) return 2;

  size_t bytes = (size_t)W * H * BPP;
  int memfd = xhd_memfd("displayctl-frame", bytes);
  pixels = memfd >= 0 ? mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0) : MAP_FAILED;
  if (pixels == MAP_FAILED) { fprintf(stderr, "displayctl: frame: %s\n", strerror(errno)); return 1; }
  background(0, 0, W, H);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  struct served s;
  int status = 1;
  if (serve(&s, helper, argc, argv, extra) || accept_helper(&s)) goto out;
  // The frame handshake: the square is drawn again only after the helper copied the last step (xhd.h, "XHDW"), asked
  // for before the first flush so that the whole first frame is acked too. Without it this loop wrote the frame while
  // the helper copied it, and a post could show the old square erased and the new one not drawn yet: a flicker of
  // the square (S25 Ultra at 60 Hz, 2026-11-19).
  if (xhd_send_hello(s.client, W, H, W * BPP, FORMAT, memfd) || xhd_send_want_acks(s.client) || xhd_send_flush(s.client, 0, 0, W, H)) { perror("displayctl: hello"); goto out; }
  int acks = xhd_wait_ack(s.client, 1000), acked = 0, unacked = 0;   // the first frame: the helper's Java start is done by now
  if (acks < 0) { fprintf(stderr, "displayctl: the helper closed the socket\n"); goto out; }
  printf("displayctl: helper %d connected; %ux%u pattern in %s for %.1f s\n", (int)s.child, W, H, xh_pixels_name(FORMAT), seconds);
  fflush(stdout);

  // A white square that bounces; each step flushes where it was and where it is.
  uint32_t side = W / 5, x = 2, y = 2, old_x = x, old_y = y;
  int dx = (int)(W / 90) + 1, dy = (int)(H / 150) + 1;
  unsigned long frames = 0;
  double start = now();
  int gone = 0;
  while (now() - start < seconds && !interrupted) {
    background(old_x, old_y, side, side);
    if ((int)x + dx < 2 || x + side + (uint32_t)(dx > 0 ? dx : 0) > W - 2) dx = -dx;
    if ((int)y + dy < 2 || y + side + (uint32_t)(dy > 0 ? dy : 0) > H - 2) dy = -dy;
    x = (uint32_t)((int)x + dx); y = (uint32_t)((int)y + dy);
    box(x, y, side);
    // Present one *complete* animation step. Two separate flush packets allow
    // the helper to post after only the old square's damage is copied, leaving
    // the new square's moving edge stale for an intermediate visible buffer.
    // The union covers both the erased old position and the newly drawn one.
    struct xhd_rect changed = { old_x, old_y, side, side };
    struct xhd_rect next = { x, y, side, side };
    xhd_rect_union(&changed, &next);
    int rc = xhd_send_flush(s.client, changed.x, changed.y, changed.w, changed.h);
    if (rc < 0) {
      fprintf(stderr, "displayctl: frame flush failed: %s\n", strerror(-rc));
      gone = 1;
      break;
    }
    old_x = x; old_y = y;
    frames++;
    if (acks) {
      int a = xhd_wait_ack(s.client, 100);
      if (a < 0) { gone = 1; break; }
      if (a) acked++;
      else if (++unacked >= 3 && !acked) acks = 0;      // acks stopped coming: pace by time, as before
    }
    if (!acks) {
      struct timespec nap = { 0, 1000000000 / 120 };
      nanosleep(&nap, NULL);
    }
  }
  printf("displayctl: %lu frames in %.2f s%s; %d acked by the helper%s\n", frames, now() - start, gone ? " (the helper closed the socket early)" : "",
         acked, acked ? " (each step drawn only after the helper copied the last)" : " (no frame handshake: steps paced by time)");
  status = gone;
out:
  if (end_session(&s)) status = 1;                // the session ends: the helper removes its layer and exits
  printf("displayctl: %s\n", status ? "FAILED" : "ok");
  return status ? 1 : 0;
}

// ---- pool: the screen of a VM of crosvm

// The whole of a /proc file, NUL-terminated (a maps file of more than 64 MiB is not crosvm's). NULL on a failure (said).
static char *read_all(const char *path)
{
  FILE *f = fopen(path, "re");
  if (!f) { fprintf(stderr, "displayctl pool: %s: %s\n", path, strerror(errno)); return NULL; }
  size_t size = 0, cap = 0;
  char *text = NULL;
  for (;;) {
    if (cap - size < 4096) {
      size_t more = cap ? cap * 2 : 1 << 16;
      char *grown = more <= (64u << 20) ? realloc(text, more) : NULL;
      if (!grown) break;
      text = grown;
      cap = more;
    }
    size_t n = fread(text + size, 1, cap - size - 1, f);
    if (!n) break;
    size += n;
  }
  fclose(f);
  if (!text) { fprintf(stderr, "displayctl pool: %s: out of memory\n", path); return NULL; }
  text[size] = 0;
  return text;
}

// crosvm's own mapping of the pool (xhd_pool_mapping), opened through /proc/PID/map_files: the guest's memfd, checked to
// be that mapping's file and to reach the pool's end. The fd, or -1 (said).
static int open_pool(pid_t pid, uint64_t offset, uint64_t bytes)
{
  char path[96];
  snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
  char *maps = read_all(path);
  if (!maps) return -1;
  uint64_t start = 0, end = 0, inode = 0;
  int found = xhd_pool_mapping(maps, offset, bytes, &start, &end, &inode);
  free(maps);
  if (found) {
    fprintf(stderr, "displayctl pool: process %d maps no \"/memfd:crosvm_guest\" shared at offset 0x%llx for 0x%llx bytes: not crosvm with that pool\n",
            (int)pid, (unsigned long long)offset, (unsigned long long)bytes);
    return -1;
  }
  snprintf(path, sizeof path, "/proc/%d/map_files/%llx-%llx", (int)pid, (unsigned long long)start, (unsigned long long)end);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "displayctl pool: %s: %s\n", path, strerror(errno)); return -1; }
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_ino != inode || (uint64_t)st.st_size < offset + bytes) {
    fprintf(stderr, "displayctl pool: %s is not the memfd of crosvm's mapping, or ends before the pool does\n", path);
    close(fd);
    return -1;
  }
  printf("displayctl pool: crosvm %d maps the pool at 0x%llx-0x%llx (offset 0x%llx of its %llu MiB memfd)\n", (int)pid,
         (unsigned long long)start, (unsigned long long)end, (unsigned long long)offset, (unsigned long long)(st.st_size >> 20));
  return fd;
}

// --test-guest FILE (the host tests): a plain file of the tests' own stands for crosvm's memfd. Not a memfd, a device or
// anything under /proc: a stand-in, never a way around the check of crosvm's mapping.
static int open_test_guest(const char *file, uint64_t need)
{
  char real[PATH_MAX];
  if (!realpath(file, real) || !strncmp(real, "/proc/", 6) || !strncmp(real, "/dev/", 5)) {
    fprintf(stderr, "displayctl pool: --test-guest %s: a plain file of the tests, not under /proc or /dev\n", file);
    return -1;
  }
  int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink < 1 || (uint64_t)st.st_size < need) {
    fprintf(stderr, "displayctl pool: --test-guest %s: not a plain file of %llu bytes or more\n", file, (unsigned long long)need);
    if (fd >= 0) close(fd);
    return -1;
  }
  return fd;
}

// crosvm is still there: its /proc/PID/stat names a state that is not a zombie's or a dead one's (kill(pid, 0) also
// reaches a zombie). One named file.
static int alive(pid_t pid)
{
  char path[64], text[512];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno != ENOENT && errno != ESRCH;
  ssize_t n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) return 1;
  text[n] = 0;
  const char *state = strrchr(text, ')');
  return !state || (state[1] != 0 && state[2] != 'Z' && state[2] != 'X');
}

static int page_resident(const uint8_t *page_start, size_t page)
{
  unsigned char in = 0;
  return !mincore((void *)page_start, page, &in) && (in & 1);
}

static int pool_main(int argc, char **argv)
{
  long long pid = 0;
  unsigned long long pool_off = 0, pool_bytes = 0, fb_off = 0, seq_off = 0, stride = 0;
  uint32_t width = 0, height = 0;
  double seconds = 0;                   // 0: until the VM ends
  char helper[PATH_MAX] = "";
  const char *test_guest = NULL;
  int extra = argc;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--")) { extra = i + 1; break; }
    if (i + 1 >= argc) return usage();
    const char *o = argv[i], *v = argv[++i];
    if (!strcmp(o, "--pid")) pid = strtoll(v, NULL, 10);
    else if (!strcmp(o, "--pool-offset")) pool_off = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--pool-bytes")) pool_bytes = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--fb-offset")) fb_off = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--seq-offset")) seq_off = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--stride")) stride = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--size")) { if (sscanf(v, "%ux%u", &width, &height) != 2) return usage(); }
    else if (!strcmp(o, "--seconds")) seconds = atof(v);
    else if (!strcmp(o, "--helper")) snprintf(helper, sizeof helper, "%s", v);
    else if (!strcmp(o, "--test-guest")) test_guest = v;
    else return usage();
  }
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const unsigned long long frame_bytes = stride * height;
  // The frame and the count inside the pool: the frame first and page aligned, the count's 16 bytes after it.
  if (pid <= 0 || pid > INT_MAX || !pool_bytes || pool_off % page || pool_bytes % page || pool_off > (1ull << 40) ||
      pool_bytes > (1ull << 30) || width < 16 || height < 16 || width > XHD_MAX_SIDE || height > XHD_MAX_SIDE ||
      stride < (unsigned long long)width * 4 || stride % 4 || stride > XHD_MAX_SIDE * 8ull || fb_off % page ||
      fb_off >= pool_bytes || seq_off % 16 || seq_off < fb_off + frame_bytes || seq_off + 16 > pool_bytes ||
      seconds < 0 || seconds > 86400) {
    fprintf(stderr, "displayctl pool: the frame (page aligned) and its count (after it) must lie in the pool\n");
    return usage();
  }
  int allowed = helper_options("pool", argc, argv, extra);
  if (allowed == 1) return usage();
  if (allowed) return 2;
  if (find_helper(helper, sizeof helper)) return 2;
  signal(SIGPIPE, SIG_IGN);

  // The VM's memory: the pool's frame and count, read-only, and nothing else.
  int fd = test_guest ? open_test_guest(test_guest, pool_off + pool_bytes) : open_pool((pid_t)pid, pool_off, pool_bytes);
  if (fd < 0) return 1;
  const size_t map_len = (size_t)((seq_off + 16 - fb_off + page - 1) / page * page);
  const uint8_t *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, (off_t)(pool_off + fb_off));
  close(fd);
  if (map == MAP_FAILED) { perror("displayctl pool: mmap"); return 1; }
  const size_t frame_pages = (size_t)((frame_bytes + page - 1) / page), seq_page = (size_t)((seq_off - fb_off) / page * page);
  const uint8_t *count_at = map + (seq_off - fb_off);
  unsigned char *resident = calloc(frame_pages, 1);
  int memfd = xhd_memfd("oneios-vm-frame", (size_t)frame_bytes);
  uint8_t *frame = memfd >= 0 ? mmap(NULL, (size_t)frame_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0) : MAP_FAILED;
  if (!resident || frame == MAP_FAILED) { fprintf(stderr, "displayctl pool: frame: %s\n", strerror(errno)); return 1; }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  struct served s;
  int status = 1;
  if (serve(&s, helper, argc, argv, extra) || accept_helper(&s)) goto out;
  // The first frame: what the pool holds now, whole (rows not resident yet stay black).
  struct xhd_rect changed;
  uint64_t count = 0, last = 0;
  int counted = page_resident(map + seq_page, page) && xhd_frame_seq(count_at, &count);
  if (mincore((void *)map, frame_pages * page, resident)) { perror("displayctl pool: mincore"); goto out; }
  xhd_pool_copy(frame, map, width, height, (uint32_t)stride, resident, page, !counted, &changed);
  last = count;
  if (xhd_send_hello(s.client, width, height, (uint32_t)stride, XHD_FORMAT_BGRX, memfd) || xhd_send_want_acks(s.client) ||
      xhd_send_flush(s.client, 0, 0, width, height)) { perror("displayctl pool: hello"); goto out; }
  int acks = xhd_wait_ack(s.client, 1000), acked = 0, unacked = 0;
  if (acks < 0) { fprintf(stderr, "displayctl pool: the helper closed the socket\n"); goto out; }
  printf("displayctl pool: helper %d connected; the frame %ux%u (%llu bytes a row) at +0x%llx of the pool, its count at +0x%llx: %s\n",
         (int)s.child, width, height, stride, fb_off, seq_off,
         counted ? "there, a frame is copied when it moves" : "not there yet, the frame is compared four times a second");
  fflush(stdout);

  const double start = now();
  double next_scan = start + (counted ? 2.0 : 0.25), last_frame = start - 10, next_report = start + 10, next_alive = start + 1;
  unsigned long frames = 0, scans = 0, flushes = 0, rows = 0, reported = 0;
  int gone = 0, ended = 0;
  while (!interrupted) {
    const double t = now();
    if (seconds > 0 && t - start >= seconds) break;
    if (kill((pid_t)pid, 0) && errno == ESRCH) { ended = 1; break; }
    if (t >= next_alive) {
      if (!alive((pid_t)pid)) { ended = 1; break; }
      next_alive = t + 1;
    }
    const int was = counted;
    counted = page_resident(map + seq_page, page) && xhd_frame_seq(count_at, &count);
    if (counted && !was) {
      printf("displayctl pool: the guest counts its frames (%llu so far): from now on a frame is copied when the count moves\n",
             (unsigned long long)count);
      fflush(stdout);
    }
    const int fresh = counted && count != last;
    if (!fresh && t < next_scan) {
      if (t >= next_report) {
        if (flushes != reported)
          printf("displayctl pool: %.0f s: %lu frames from the guest (count %llu), %lu flushes, %lu rows copied\n", t - start, frames,
                 (unsigned long long)count, flushes, rows);
        fflush(stdout);
        reported = flushes;
        next_report = t + 10;
      }
      nap_ms(t - last_frame < 0.5 ? 2 : 16);        // quick while frames come, slow when the screen is still
      continue;
    }
    if (fresh) { frames++; last = count; last_frame = t; }
    else scans++;
    next_scan = t + (counted ? 2.0 : 0.25);
    if (mincore((void *)map, frame_pages * page, resident)) { perror("displayctl pool: mincore"); break; }
    // Uncached writes (the console's) need the lines cleaned and invalidated; the display driver's are write-back.
    const uint32_t n = xhd_pool_copy(frame, map, width, height, (uint32_t)stride, resident, page, !counted, &changed);
    if (!n) continue;
    int rc = xhd_send_flush(s.client, changed.x, changed.y, changed.w, changed.h);
    if (rc < 0) { fprintf(stderr, "displayctl pool: flush failed: %s\n", strerror(-rc)); gone = 1; break; }
    flushes++;
    rows += n;
    // The frame is written again only after the helper copied it (xhd.h, "XHDW"), or 100 ms later.
    if (acks) {
      int a = xhd_wait_ack(s.client, 100);
      if (a < 0) { gone = 1; break; }
      if (a) acked++;
      else if (++unacked >= 3 && !acked) acks = 0;
    }
    if (!acks) nap_ms(8);
  }
  printf("displayctl pool: %s after %.1f s: %lu frames from the guest, %lu scans, %lu flushes (%d acked), %lu rows copied\n",
         ended ? "the VM ended" : gone ? "the helper left" : interrupted ? "stopped" : "time is up", now() - start, frames, scans,
         flushes, acked, rows);
  status = gone;
out:
  if (end_session(&s)) status = 1;
  printf("displayctl: %s\n", status ? "FAILED" : "ok");
  return status ? 1 : 0;
}

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "test")) return test_main(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "pool")) return pool_main(argc, argv);
  return usage();
}
