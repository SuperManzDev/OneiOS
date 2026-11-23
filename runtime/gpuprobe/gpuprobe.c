// SPDX-License-Identifier: MIT
// gpuprobe: can Android's GPU write into the restricted DMA pool of a protected VM of crosvm? (docs/native.md, the GPU)
//   gpuprobe --pid CROSVM --pool-offset N --pool-bytes N --at N [--bytes N]
// Such a VM's memory is lent to the guest (the host, and every device of the host's, faults on it) but for crosvm's
// restricted DMA pool, which crosvm shares: a GPU path for the VM would keep the GPU's buffers there. This maps
// [pool + at, + bytes) of crosvm's own mapping of the pool, found as displayctl pool finds it (/proc/PID/maps, then
// /proc/PID/map_files), a part the VM does not use (xnuloader-crosvm puts the frame, the disks' windows and the
// input's first: a 32 MiB pool's last 2 MiB stay free); gives it to kgsl by its user address
// (IOCTL_KGSL_GPUOBJ_IMPORT, else the legacy IOCTL_KGSL_MAP_USER_MEM with its length; I/O-coherent and write-back, as
// drm2kgsl imports a guest's memory); and has the GPU write one word into it (CP_MEM_WRITE in an indirect buffer of
// its own, IOCTL_KGSL_GPU_COMMAND), which it then reads through its own mapping. It writes nothing else of the VM's.
#define _GNU_SOURCE
#include "xhd.h"
#include "msm_kgsl.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SENTINEL 0x5eed5eedu
#define WRITTEN  0xc0ffee42u
#define CP_MEM_WRITE       0x3d
#define CP_WAIT_MEM_WRITES 0x12
#define CP_WAIT_FOR_ME     0x13

static int usage(void)
{
  fprintf(stderr, "usage: gpuprobe --pid CROSVM --pool-offset N --pool-bytes N --at N [--bytes N]\n");
  return 2;
}

static int kgsl(int fd, unsigned long request, void *arg)
{
  int r;
  do r = ioctl(fd, request, arg);
  while (r == -1 && errno == EINTR);
  return r;
}

// A type-7 PM4 packet's header (Adreno 5xx and later): the count and the opcode, each with its odd parity bit.
static uint32_t parity(uint32_t v)
{
  return (0x9669u >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}
static uint32_t pkt7(uint32_t opcode, uint32_t count)
{
  return 0x70000000u | count | (parity(count) << 15) | ((opcode & 0x7f) << 16) | (parity(opcode) << 23);
}

static char *read_all(const char *path)
{
  FILE *f = fopen(path, "re");
  if (!f) return NULL;
  size_t size = 0, cap = 1 << 16;
  char *text = malloc(cap);
  for (size_t n; text && (n = fread(text + size, 1, cap - size - 1, f)) > 0;) {
    size += n;
    if (cap - size < 4096) {
      char *grown = cap < (64u << 20) ? realloc(text, cap * 2) : NULL;
      if (!grown) break;
      text = grown;
      cap *= 2;
    }
  }
  fclose(f);
  if (text) text[size] = 0;
  return text;
}

int main(int argc, char **argv)
{
  long long pid = 0;
  unsigned long long pool_off = 0, pool_bytes = 0, at = 0, bytes = 2u << 20;
  for (int i = 1; i < argc; i++) {
    if (i + 1 >= argc) return usage();
    const char *o = argv[i], *v = argv[++i];
    if (!strcmp(o, "--pid")) pid = strtoll(v, NULL, 10);
    else if (!strcmp(o, "--pool-offset")) pool_off = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--pool-bytes")) pool_bytes = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--at")) at = strtoull(v, NULL, 0);
    else if (!strcmp(o, "--bytes")) bytes = strtoull(v, NULL, 0);
    else return usage();
  }
  if (pid <= 0 || !pool_bytes || (pool_off & 0xfff) || (at & 0xfff) || !bytes || (bytes & 0xfff) || at + bytes > pool_bytes) return usage();

  // crosvm's own mapping of the pool, and its memfd (as displayctl pool opens it)
  char path[96];
  snprintf(path, sizeof path, "/proc/%lld/maps", pid);
  char *maps = read_all(path);
  uint64_t start = 0, end = 0, inode = 0;
  if (!maps || xhd_pool_mapping(maps, pool_off, pool_bytes, &start, &end, &inode)) {
    fprintf(stderr, "gpuprobe: process %lld maps no crosvm pool at offset 0x%llx for 0x%llx bytes\n", pid, pool_off, pool_bytes);
    return 1;
  }
  free(maps);
  snprintf(path, sizeof path, "/proc/%lld/map_files/%" PRIx64 "-%" PRIx64, pid, start, end);
  int memfd = open(path, O_RDWR | O_CLOEXEC);
  struct stat st;
  if (memfd < 0 || fstat(memfd, &st) || (uint64_t)st.st_ino != inode || (uint64_t)st.st_size < pool_off + pool_bytes) {
    fprintf(stderr, "gpuprobe: %s: %s\n", path, memfd < 0 ? strerror(errno) : "not the pool's memfd");
    return 1;
  }
  uint8_t *cpu = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, (off_t)(pool_off + at));
  close(memfd);
  if (cpu == MAP_FAILED) { perror("gpuprobe: mmap"); return 1; }
  volatile uint32_t *word = (volatile uint32_t *)(cpu + 256);
  *word = SENTINEL;
  __sync_synchronize();
  printf("gpuprobe: crosvm %lld's pool at 0x%" PRIx64 "-0x%" PRIx64 "; its +0x%llx (%llu KiB) mapped for the GPU\n", pid, start, end, at, bytes >> 10);

  int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("gpuprobe: /dev/kgsl-3d0"); return 1; }
  struct kgsl_drawctxt_create ctx = { .flags = KGSL_CONTEXT_SAVE_GMEM | KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_PREAMBLE };
  if (kgsl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &ctx)) { perror("gpuprobe: DRAWCTXT_CREATE"); return 1; }

  // The pool's part as a GPU object: by its user address, else with the legacy call that takes a length.
  const uint64_t memflags = KGSL_MEMFLAGS_IOCOHERENT | ((uint64_t)KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT);
  uint64_t target = 0;
  unsigned int pool_id = 0;
  unsigned long legacy_gpuaddr = 0;
  struct kgsl_gpuobj_import_useraddr address = { .virtaddr = (uintptr_t)cpu };
  struct kgsl_gpuobj_import import = { .priv = (uintptr_t)&address, .priv_len = sizeof address, .flags = memflags, .type = KGSL_USER_MEM_TYPE_ADDR };
  if (!kgsl(fd, IOCTL_KGSL_GPUOBJ_IMPORT, &import)) {
    struct kgsl_gpuobj_info info = { .id = import.id };
    if (kgsl(fd, IOCTL_KGSL_GPUOBJ_INFO, &info)) { perror("gpuprobe: GPUOBJ_INFO"); return 1; }
    pool_id = import.id;
    target = info.gpuaddr;
    printf("gpuprobe: GPUOBJ_IMPORT by user address: id %u, GPU address 0x%" PRIx64 ", %" PRIu64 " bytes\n", pool_id, target, (uint64_t)info.size);
  } else {
    printf("gpuprobe: GPUOBJ_IMPORT by user address: %s; the legacy MAP_USER_MEM\n", strerror(errno));
    struct kgsl_map_user_mem map = { .fd = -1, .len = bytes, .hostptr = (uintptr_t)cpu, .memtype = KGSL_USER_MEM_TYPE_ADDR, .flags = (unsigned int)memflags };
    if (kgsl(fd, IOCTL_KGSL_MAP_USER_MEM, &map)) { perror("gpuprobe: MAP_USER_MEM"); return 1; }
    legacy_gpuaddr = map.gpuaddr;
    target = map.gpuaddr;
    printf("gpuprobe: MAP_USER_MEM: GPU address 0x%lx\n", legacy_gpuaddr);
  }
  target += 256;

  // An indirect buffer of the GPU's own: write the word, wait for the write to land.
  struct kgsl_gpuobj_alloc alloc = { .size = 4096 };
  if (kgsl(fd, IOCTL_KGSL_GPUOBJ_ALLOC, &alloc)) { perror("gpuprobe: GPUOBJ_ALLOC"); return 1; }
  struct kgsl_gpuobj_info ib_info = { .id = alloc.id };
  if (kgsl(fd, IOCTL_KGSL_GPUOBJ_INFO, &ib_info)) { perror("gpuprobe: GPUOBJ_INFO (IB)"); return 1; }
  uint32_t *ib = mmap(NULL, alloc.mmapsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)alloc.id << 12);
  if (ib == MAP_FAILED) { perror("gpuprobe: mmap (IB)"); return 1; }
  unsigned n = 0;
  ib[n++] = pkt7(CP_MEM_WRITE, 3);
  ib[n++] = (uint32_t)target;
  ib[n++] = (uint32_t)(target >> 32);
  ib[n++] = WRITTEN;
  ib[n++] = pkt7(CP_WAIT_MEM_WRITES, 0);
  ib[n++] = pkt7(CP_WAIT_FOR_ME, 0);
  __asm__ volatile("dsb st" ::: "memory");
  struct kgsl_command_object object = { .gpuaddr = ib_info.gpuaddr, .size = n * 4, .flags = KGSL_CMDLIST_IB, .id = alloc.id };
  struct kgsl_gpu_command command = { .cmdlist = (uintptr_t)&object, .cmdsize = sizeof object, .numcmds = 1, .context_id = ctx.drawctxt_id };
  if (kgsl(fd, IOCTL_KGSL_GPU_COMMAND, &command)) { perror("gpuprobe: GPU_COMMAND"); return 1; }
  struct kgsl_device_waittimestamp_ctxtid wait = { .context_id = ctx.drawctxt_id, .timestamp = command.timestamp, .timeout = 2000 };
  int waited = kgsl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);
  const uint32_t got = *word;
  printf("gpuprobe: the GPU's command (timestamp %u) %s; the word at +0x%llx is 0x%08x: %s\n", command.timestamp,
         waited ? "did not complete in 2 s" : "completed", at + 256, got,
         got == WRITTEN ? "the GPU wrote into the VM's pool" : got == SENTINEL ? "unchanged: the GPU's write did not land" : "something else");

  munmap(ib, alloc.mmapsize);
  struct kgsl_gpumem_free_id free_ib = { .id = alloc.id };
  kgsl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &free_ib);
  if (pool_id) {
    struct kgsl_gpumem_free_id free_pool = { .id = pool_id };
    kgsl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &free_pool);
  } else if (legacy_gpuaddr) {
    struct kgsl_sharedmem_free free_pool = { .gpuaddr = legacy_gpuaddr };
    kgsl(fd, IOCTL_KGSL_SHAREDMEM_FREE, &free_pool);
  }
  struct kgsl_drawctxt_destroy destroy = { .drawctxt_id = ctx.drawctxt_id };
  kgsl(fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &destroy);
  close(fd);
  munmap(cpu, bytes);
  return got == WRITTEN ? 0 : 1;
}
