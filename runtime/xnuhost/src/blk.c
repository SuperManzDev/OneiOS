// SPDX-License-Identifier: MIT
// Disks backed by the image set's files. The role of UniXNU's RamBlock and VirtioMmioBlock back ends
// (https://github.com/UniXNU/RamBlock 60a54ce, https://github.com/UniXNU/VirtioMmioBlock 4e1013e):
// there the guest keeps a seed in RAM and changed pages in a store window; here Linux's page cache
// and a sparse file do both, so the store is simply a raw image the size of the Data disk.
//   read-only: os.img, systemos.img, app.img (raw APFS containers), opened O_RDONLY.
//   writable:  <data>/data.img, created sparse on the first start. A seed that is itself a disk image
//              (not a tar) is copied in, holes kept; a tar seed is the userland's to unpack.
#define _GNU_SOURCE
#include "xnuhost.h"
#include "imageset.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct xh_blk {
  int fd; uint64_t bytes; bool writable; xh_verify *verify;
  char name[16];                // the disk's role (main.c), for its statistics line
  uint64_t last_end;            // where the last read ended: a read that starts near it is sequential (the read-ahead)
  // Since the last statistics line; one request at a time (virtio_blk.c answers under the device's lock).
  struct { uint64_t since, reads, read_bytes, writes, write_bytes, flushes, busy_ns, max_ns; } io;
};

// Chunks checked ahead after a sequential read of a verified image (disk-ahead=, xh_blk_set_ahead; 0 = none).
static unsigned ahead_chunks = 8;
void xh_blk_set_ahead(unsigned chunks) { ahead_chunks = chunks > 64 ? 64 : chunks; }
void xh_blk_set_name(xh_blk *b, const char *name) { snprintf(b->name, sizeof b->name, "%s", name && *name ? name : "?"); }

static uint64_t blk_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// One request's cost (virtio_blk.c), and every 10 s while requests come one line:
//   disk systemos: 10 s: reads 4821 (4712 MiB), writes 0 (0 MiB), flushes 0; in requests 8123 ms (mean 1685 us, max 12345
//   us); chunks checked: 300 on the reads (450 ms), 4100 ahead (5900 ms on the workers), 120 waits (80 ms)
// "in requests" is the time the guest's vCPUs spent in this disk's requests (its driver sends one at a time and waits).
void xh_blk_account(xh_blk *b, unsigned type, uint64_t bytes, uint64_t ns)
{
  uint64_t now = blk_now_ns();
  if (!b->io.since) b->io.since = now;
  if (type == 0) { b->io.reads++; b->io.read_bytes += bytes; }
  else if (type == 1) { b->io.writes++; b->io.write_bytes += bytes; }
  else if (type == 4) b->io.flushes++;
  b->io.busy_ns += ns;
  if (ns > b->io.max_ns) b->io.max_ns = ns;
  if (now - b->io.since < 10000000000ull) return;
  uint64_t n = b->io.reads + b->io.writes + b->io.flushes;
  char checks[256] = "";
  if (b->verify) {
    xh_verify_stats s;
    xh_verify_stats_take(b->verify, &s);
    snprintf(checks, sizeof checks, "; chunks checked: %llu on the reads (%llu ms), %llu ahead (%llu ms on the workers), %llu waits (%llu ms)%s",
             (unsigned long long)s.sync_chunks, (unsigned long long)(s.sync_ns / 1000000), (unsigned long long)s.ahead_chunks,
             (unsigned long long)(s.ahead_ns / 1000000), (unsigned long long)s.waits, (unsigned long long)(s.wait_ns / 1000000),
             s.failed || s.ahead_failed ? ", some chunks FAILED their check" : "");
  }
  LOGI("disk %s: %llu s: reads %llu (%llu MiB), writes %llu (%llu MiB), flushes %llu; in requests %llu ms (mean %llu us, max %llu us)%s",
       b->name[0] ? b->name : "?", (unsigned long long)((now - b->io.since) / 1000000000u), (unsigned long long)b->io.reads,
       (unsigned long long)(b->io.read_bytes >> 20), (unsigned long long)b->io.writes, (unsigned long long)(b->io.write_bytes >> 20),
       (unsigned long long)b->io.flushes, (unsigned long long)(b->io.busy_ns / 1000000), (unsigned long long)(n ? b->io.busy_ns / n / 1000 : 0),
       (unsigned long long)(b->io.max_ns / 1000), checks);
  memset(&b->io, 0, sizeof b->io);
  b->io.since = now;
}

#define DATA_DEFAULT_BYTES (32ull << 30)      // sparse: costs what the guest writes

static bool is_tar(int fd)
{
  char magic[6] = { 0 };
  return pread(fd, magic, 5, 257) == 5 && !memcmp(magic, "ustar", 5);
}

// Copies a seed image into a new store, skipping blocks of zeros so the store stays sparse.
static int copy_seed(int from, int to, uint64_t bytes)
{
  enum { CHUNK = 1 << 20 };
  static const char zeros[4096];
  char *buffer = malloc(CHUNK);
  if (!buffer) return -1;
  int rc = 0;
  for (uint64_t at = 0; at < bytes && !rc;) {
    ssize_t n = pread(from, buffer, CHUNK, (off_t)at);
    if (n <= 0) { rc = -1; break; }
    for (ssize_t b = 0; b < n; b += 4096) {
      size_t len = (size_t)(n - b < 4096 ? n - b : 4096);
      if (!memcmp(buffer + b, zeros, len)) continue;
      if (pwrite(to, buffer + b, len, (off_t)(at + (uint64_t)b)) != (ssize_t)len) { rc = -1; break; }
    }
    at += (uint64_t)n;
  }
  free(buffer);
  return rc;
}

xh_blk *xh_blk_open(const xh_disk_config *config, const char **error)
{
  struct stat st;
  xh_blk *b = calloc(1, sizeof *b);
  if (!b) { *error = "out of memory"; return NULL; }
  b->fd = -1;
  b->writable = config->writable;
  if (!config->writable) {
    b->fd = open(config->path, O_RDONLY | O_CLOEXEC);
    if (b->fd < 0 || fstat(b->fd, &st)) { *error = "cannot open the image"; goto bad; }
  } else {
    b->fd = open(config->path, O_RDWR | O_CLOEXEC);
    if (b->fd < 0 && errno == ENOENT) {
      // First start: a new store, written under a temporary name so a crash leaves no half store.
      char temporary[600];
      snprintf(temporary, sizeof temporary, "%s.new", config->path);
      int fd = open(temporary, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0) { *error = "cannot create the Data store"; goto bad; }
      uint64_t bytes = DATA_DEFAULT_BYTES;
      int seed = *config->seed ? open(config->seed, O_RDONLY | O_CLOEXEC) : -1;
      bool image = seed >= 0 && !is_tar(seed) && fstat(seed, &st) == 0 && st.st_size >= 4096;
      if (image && (uint64_t)st.st_size > bytes) bytes = (uint64_t)st.st_size;
      int rc = ftruncate(fd, (off_t)bytes);
      if (!rc && image) rc = copy_seed(seed, fd, (uint64_t)st.st_size);
      if (!rc) rc = fsync(fd);
      if (seed >= 0) close(seed);
      if (rc || rename(temporary, config->path)) { close(fd); unlink(temporary); *error = "cannot fill the new Data store"; goto bad; }
      b->fd = fd;
    }
    if (b->fd < 0 || fstat(b->fd, &st)) { *error = "cannot open the Data store"; goto bad; }
  }
  b->bytes = (uint64_t)st.st_size & ~511ull;
  if (b->bytes == 0) { *error = "the image is empty"; goto bad; }
  return b;
bad:
  if (b->fd >= 0) close(b->fd);
  free(b);
  return NULL;
}

uint64_t xh_blk_bytes(const xh_blk *b) { return b->bytes; }
bool xh_blk_writable(const xh_blk *b) { return b->writable; }

// A read-only image whose chunks are checked against the image set's hash list before the guest
// gets them (imageset.c). A chunk that differs is a read error.
void xh_blk_set_verify(xh_blk *b, xh_verify *verify) { if (!b->writable) b->verify = verify; }

int xh_blk_read(xh_blk *b, uint64_t offset, void *buffer, uint64_t length)
{
  if (offset > b->bytes || length > b->bytes - offset) return -1;
  if (b->verify && xh_verify_range(b->verify, b->fd, offset, length)) return -1;
  for (uint64_t done = 0; done < length;) {
    ssize_t n = pread(b->fd, (char *)buffer + done, length - done, (off_t)(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    done += (uint64_t)n;
  }
  // A read that starts within 2 MiB of where the last one ended is sequential (the guest's vCPUs take the shared
  // cache's megabytes in rising order, a little out of turn): the next chunks are checked ahead (imageset.c,
  // xh_verify_ahead), so the guest's read of them is a page-cache copy. A random read queues nothing.
  if (b->verify && ahead_chunks && length) {
    uint64_t end = offset + length, window = 2ull << 20;
    if (b->last_end && offset + window >= b->last_end && offset <= b->last_end + window) {
      uint64_t chunk = xh_verify_chunk(b->verify);
      (void)xh_verify_ahead(b->verify, b->fd, (end - 1) / chunk + 1, ahead_chunks);
    }
    b->last_end = end;
  }
  return 0;
}

int xh_blk_write(xh_blk *b, uint64_t offset, const void *buffer, uint64_t length)
{
  if (!b->writable || offset > b->bytes || length > b->bytes - offset) return -1;
  for (uint64_t done = 0; done < length;) {
    ssize_t n = pwrite(b->fd, (const char *)buffer + done, length - done, (off_t)(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    done += (uint64_t)n;
  }
  return 0;
}

int xh_blk_flush(xh_blk *b) { return b->writable ? fdatasync(b->fd) : 0; }

void xh_blk_close(xh_blk *b)
{
  if (!b) return;
  if (b->writable) fsync(b->fd);
  close(b->fd);
  free(b);
}
