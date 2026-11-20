// SPDX-License-Identifier: MIT
// xnuloader-qemu: a kernel placed for qemu-system-aarch64's virt board the way a firmware loader (DarwinBoot) places it
// on a phone: EL1, MMU and caches off, at the physical address of its entry, x0 = the physical address of boot_args.
// The bench for the native kernel (kernel/build-native.sh; the plain one of kernel/build-linux.sh too) before any phone
// (docs/native.md).
//   xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--qemu-cpu M]
//                  [--image DIR --data FILE]
// Writes into OUTDIR:
//   window.bin  the kernel's window from its physical base: its segments, the device tree, boot_args and the trust
//               caches (xl_boot_place, the code xnuhost uses, with virt_base = the kernel's own base as a firmware gives it)
//   stub.bin    2 MiB into RAM: interrupts masked, x0 = boot_args, a branch to the entry
//   board.adt   the device tree the kernel was given (xnuloader-inspect adt FILE prints it)
//   qemu.args   the machine, the two loader devices and the disks, one argument per line (scripts/qemu/boot-vm.sh)
// and prints the layout. The board: virt with GICv3 (distributor 0x08000000, redistributors 0x080a0000, 0x20000 a
// CPU), the PL011 at 0x09000000, PSCI by HVC (the machine has no EL2 or EL3), Cortex-A710 CPUs (--qemu-cpu: why).
// --image DIR: the image set (docs/image-format.md), read as xnuhost reads it (runtime/xnuhost/src/imageset.c: the
// manifest, each file's SHA-256, the hash lists, the trust caches). Its disks go on virt's virtio-mmio transports (modern
// virtio, transport K at 0x0a000000 + 0x200 * K): os, systemos and app read-only, the Data seed read-only, and the Data
// store FILE (--data, a raw file the kernel formats on its first start) writable, each node with the identity
// xnuhost gives it. A read-only image's node also carries its hash list ("oneios,hash-list"): the native kernel's
// OneiOSHostedDisk checks every chunk it reads against it, as xnuhost does for a hosted kernel. /chosen root-matching
// names the OS volume; the trust caches go where the hosted kernel's do.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "imageset.h"
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

#define RAM_BASE      0x40000000ull     // virt's DRAM
#define STUB_PA       (RAM_BASE + 0x200000) // past the FDT QEMU writes at the start of RAM (1 MiB) for a bare image
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB: 16K's L2 blocks, and 4K's 2 MiB ones
#define VIRTIO_BASE   0x0a000000ull     // virt's 32 virtio-mmio transports; QEMU's bus virtio-mmio-bus.K is transport K
#define VIRTIO_SIZE   0x200ull
#define VIRTIO_INTID  48u               // transport K's line: SPI 16 + K (the driver polls; the node says it all the same)
#define MAX_DISKS     5

// The machine's own arguments from xnuhost's default (runtime/xnuhost/src/config.c), less the hosted kernel's devices.
static const char default_args[] =
  "debug=0x14e serial=3 -v kt_fixed_zones=120 -no_slto_panic ml-timeout-lock-panic-scale=40 tlto_us=0 "
  "sheng-smp=1 armcpu-self-ipi=1 armcpu-spi-ipi=1 use_contiguous_hint=0 interrupt-masked-debug-mode=0 "
  "sched_preemption_disable_debug_mode=0 wdt=-1";

static uint8_t *slurp(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  uint8_t *data = malloc(n > 0 ? (size_t)n : 1);
  if (n < 0 || !data || fread(data, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
  fclose(f);
  *size = (size_t)n;
  return data;
}

static void spit(const char *dir, const char *name, const void *data, size_t size)
{
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(data, 1, size, f) != size || fclose(f)) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(1); }
}

static void usage(void)
{
  fprintf(stderr, "usage: xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--qemu-cpu M] [--image DIR --data FILE]\n");
  exit(2);
}

// A disk of the image set on transport K: the file QEMU serves, read-only or not, and what its node says.
typedef struct {
  char             path[1024];
  bool             writable;
  xh_disk_identity identity;
  const uint8_t   *list;             // the hash list of a read-only image, or NULL
  size_t           list_size;
} disk;

static void image_disk(disk *d, const xh_imageset *set, const xh_imageset_file *f)
{
  snprintf(d->path, sizeof d->path, "%s/%s", set->dir, f->path);
  struct stat st;
  if (stat(d->path, &st) || (uint64_t)st.st_size != f->size || (f->size & 511)) {
    fprintf(stderr, "%s: not the %llu bytes (whole sectors) manifest.json lists\n", d->path, (unsigned long long)f->size);
    exit(1);
  }
}

// The image set's five disks, in the order of their transports, as xnuhost describes its own (main.c image_set_disk).
static unsigned image_disks(const char *dir, const char *data, xh_imageset *set, disk *disks, char seed_hex[65])
{
  const char *error = NULL;
  int rc = xh_imageset_load(dir, set, &error);
  if (rc) { fprintf(stderr, "%s: %s\n", dir, rc > 0 ? "no manifest.json" : error); exit(1); }
  unsigned n = 0;
  static const char *const images[] = { "os", "systemos", "app" };
  for (unsigned i = 0; i < 3; i++) {
    const xh_imageset_file *f = xh_imageset_role(set, images[i]);
    if (!f) { fprintf(stderr, "%s: no %s image in manifest.json\n", dir, images[i]); exit(1); }
    disk *d = &disks[n++];
    image_disk(d, set, f);
    xh_verify *v = xh_verify_open(set, f, &error);   // the hash list, checked against the manifest
    if (!v) { fprintf(stderr, "%s: %s\n", d->path, error); exit(1); }
    d->list = xh_verify_list(v, &d->list_size);
    d->identity = (xh_disk_identity){ .role = f->role, .verified = xh_verify_digest(v), .volume_uuid = f->volume_uuid };
  }
  const xh_imageset_file *seed = xh_imageset_role(set, "data-seed");
  seed_hex[0] = 0;
  for (unsigned i = 0; seed && i < 32; i++) snprintf(seed_hex + i * 2, 3, "%02x", seed->sha256[i]);
  if (seed) {
    disk *d = &disks[n++];
    image_disk(d, set, seed);
    d->identity = (xh_disk_identity){ .role = "seed", .bytes = seed->size, .sha256_hex = seed_hex };
  }
  disk *d = &disks[n++];
  struct stat st;
  if (stat(data, &st) || st.st_size < (1ll << 30) || (st.st_size & 511)) {
    fprintf(stderr, "%s: the Data store must be a raw file of whole sectors, 1 GiB or more\n", data);
    exit(1);
  }
  snprintf(d->path, sizeof d->path, "%s", data);
  d->writable = true;
  d->identity = (xh_disk_identity){ .role = "data", .volume_uuid = set->data_uuid, .volume_name = set->data_name, .sha256_hex = seed_hex };
  return n;
}

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args, *image_dir = NULL, *data_path = NULL;
  unsigned cpus = 1, ram_mib = 1024;
  // The CPU: a Cortex-A710 (Armv9.0, PAuth, BTI, 64-byte DC ZVA, no FEAT_LVA), the kind of core a phone has, not QEMU's
  // max: max has FEAT_LVA, and the DEVELOPMENT kernel asserts that no CPU has it (vm_sanitize.c:643, launchd's first
  // jobs, 2026-11-20). No SVE (Apple's cores have none; neither has the S25's Oryon). Its counter at 1 GHz, as the tree's
  // timebase says (--timebase and cntfrq= go together).
  const char *qemu_cpu = "cortex-a710,pauth-impdef=on,sve=off,cntfrq=1000000000";
  uint32_t timebase = 1000000000;
  for (int i = 3; i < argc; i++) {
    if (i + 1 >= argc) usage();
    if (!strcmp(argv[i], "--cpus")) cpus = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--qemu-cpu")) qemu_cpu = argv[++i];
    else if (!strcmp(argv[i], "--ram-mib")) ram_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--timebase")) timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--args")) args = argv[++i];
    else if (!strcmp(argv[i], "--args-add")) {
      // more arguments after the defaults (or after --args, whichever came first)
      static char joined[4096];
      if (snprintf(joined, sizeof joined, "%s %s", args, argv[++i]) >= (int)sizeof joined) { fprintf(stderr, "xnuloader-qemu: boot arguments too long\n"); return 2; }
      args = joined;
    }
    else if (!strcmp(argv[i], "--image")) image_dir = argv[++i];
    else if (!strcmp(argv[i], "--data")) data_path = argv[++i];
    else usage();
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 256 || ram_mib > 3072) { fprintf(stderr, "xnuloader-qemu: 1-%d CPUs, 256-3072 MiB\n", XL_MAX_CPUS); return 2; }
  if (!image_dir != !data_path) { fprintf(stderr, "xnuloader-qemu: --image and --data go together\n"); return 2; }
  static xh_imageset set;
  static disk disks[MAX_DISKS];
  static char seed_hex[65];
  unsigned ndisk = image_dir ? image_disks(image_dir, data_path, &set, disks, seed_hex) : 0;
  xl_blob trust = { 0 };
  if (image_dir) {
    const char *error = NULL;
    trust = xh_imageset_trustcaches(&set, &error);
    if (!trust.data) { fprintf(stderr, "%s: trust caches: %s\n", image_dir, error); return 1; }
  }

  const char *err = NULL;
  size_t size;
  uint8_t *image = slurp(kernel_path, &size);
  static xl_kernel k;
  if (xl_macho_parse(image, size, &k, &err)) { fprintf(stderr, "%s: %s\n", kernel_path, err); return 1; }

  uint64_t window = (uint64_t)ram_mib << 20;
  uint64_t phys = RAM_BASE + KERNEL_ALIGN + (k.base & (KERNEL_ALIGN - 1));
  xl_machine m;
  memset(&m, 0, sizeof m);
  m.ncpu = cpus;
  for (unsigned i = 0; i < cpus; i++) { m.cpu_reg[i] = i; m.cpu_cluster[i] = 'P'; }   // virt: Aff0 = the CPU's index
  m.timebase_hz = timebase;
  m.dram_base = phys;
  m.dram_size = window;
  m.armio_base = XL_BOARD_ARMIO_BASE;
  m.armio_size = XL_BOARD_ARMIO_SIZE;
  m.gic_dist = 0x08000000ull;
  m.gic_dist_size = 0x10000ull;
  m.gic_redist = 0x080a0000ull;
  m.gic_redist_size = 0x20000ull * cpus;
  m.gic_redist_stride = 0x20000ull;
  m.uart_base = 0x09000000ull;
  m.uart_size = 0x1000ull;
  m.psci_method = "hvc";
  m.model = "iPhone18,2";
  m.have_chip_id = true;
  m.chip_id = 0x8150;
  m.unique_chip_id = 0x0ce10511e5000001ull;     // a bench's own: not a phone's
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r || fread(m.random_seed, 1, sizeof m.random_seed, r) != sizeof m.random_seed) { fprintf(stderr, "/dev/urandom: read failed\n"); return 1; }
  fclose(r);
  // The disks: transport K for disk K, read-only ones as virtio,mmio nodes (virtio-mmio-0..), the store as data-store.
  for (unsigned i = 0; i < ndisk; i++)
    m.dev[m.ndev++] = (xl_device){ disks[i].writable ? XL_DEV_DATA_STORE : XL_DEV_BLOCK, VIRTIO_BASE + i * VIRTIO_SIZE, VIRTIO_SIZE };
  const xh_imageset_file *os = image_dir ? xh_imageset_role(&set, "os") : NULL;
  if (os && *os->volume_uuid) m.root_volume_uuid = os->volume_uuid;
  m.trust_cache = trust.data != NULL;
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }
  for (unsigned i = 0, blocks = 0; i < ndisk; i++) {
    char name[40];
    if (disks[i].writable) snprintf(name, sizeof name, "arm-io/data-store");
    else snprintf(name, sizeof name, "arm-io/virtio-mmio-%u", blocks++);
    xl_adt_node *node = xl_adt_find(tree, name);
    if (!node) { fprintf(stderr, "board: no /%s\n", name); return 1; }
    xh_imageset_describe(node, &disks[i].identity);
    if (disks[i].list && xl_adt_set(node, "oneios,hash-list", disks[i].list, disks[i].list_size)) {
      fprintf(stderr, "board: /%s: no room for its hash list\n", name);
      return 1;
    }
    xl_adt_set_u32(node, "droidvm,intid", VIRTIO_INTID + i);
  }
  // The console: XNU's PL011 driver (the board's oneios-board-pl011-console.patch) takes the node that /defaults
  // serial-device names by its AAPL,phandle (pexpert/arm/pe_serial.c get_serial_device_phandle).
  xl_adt_node *uart = xl_adt_find(tree, "arm-io/uart0"), *defaults = xl_adt_find(tree, "defaults");
  if (!uart || !defaults || xl_adt_set_u32(uart, "AAPL,phandle", 0x10001) || xl_adt_set_u32(defaults, "serial-device", 0x10001)) {
    fprintf(stderr, "board: no /arm-io/uart0 or /defaults for the console\n");
    return 1;
  }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-qemu: %u MiB: out of memory\n", ram_mib); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args, .trustcache = trust };
  xl_boot_layout lay;
  if (xl_boot_place(&req, &lay, &err)) { fprintf(stderr, "place: %s\n", err); return 1; }

  // The flat tree as the kernel got it, for xnuloader-inspect.
  uint64_t tree_off = lay.tree_pa - phys;
  spit(out, "board.adt", ram + tree_off, lay.tree_size);
  uint64_t used = (lay.used + 0x3fff) & ~0x3fffull;
  spit(out, "window.bin", ram, used);

  // msr daifset, #0xf; ldr x0, [pc, #12]; ldr x1, [pc, #16]; br x1; then boot_args' and the entry's physical addresses
  // (8-byte aligned: with the MMU off every load is to Device memory, which takes no unaligned access).
  uint32_t stub[8] = { 0xd5034fdfu, 0x58000060u, 0x58000081u, 0xd61f0020u };
  memcpy(&stub[4], &lay.args_pa, 8);
  memcpy(&stub[6], &lay.entry_pa, 8);
  spit(out, "stub.bin", stub, sizeof stub);

  uint64_t ram_end = phys + window;
  uint64_t qemu_mib = ((ram_end - RAM_BASE) + (256ull << 20) - 1) >> 20 & ~255ull;
  char line[16384];
  int n = snprintf(line, sizeof line,
                   "-M\nvirt,gic-version=3,virtualization=off,secure=off,highmem=off\n-cpu\n%s\n-smp\n%u\n-m\n%lluM\n"
                   "-device\nloader,file=%s/window.bin,addr=0x%llx,force-raw=on\n-device\nloader,file=%s/stub.bin,addr=0x%llx,cpu-num=0,force-raw=on\n",
                   qemu_cpu, cpus, (unsigned long long)qemu_mib, out, (unsigned long long)phys, out, (unsigned long long)STUB_PA);
  // The disks: modern virtio (OneiOSHostedDisk takes version 2 only), disk K on transport K. A path with a comma would
  // need QEMU's doubling: refused instead.
  if (ndisk && n >= 0 && (size_t)n < sizeof line) n += snprintf(line + n, sizeof line - n, "-global\nvirtio-mmio.force-legacy=false\n");
  for (unsigned i = 0; i < ndisk && n >= 0 && (size_t)n < sizeof line; i++) {
    if (strchr(disks[i].path, ',') || strchr(disks[i].path, '\n')) { fprintf(stderr, "%s: a disk path with a comma\n", disks[i].path); return 1; }
    n += snprintf(line + n, sizeof line - n, "-drive\nif=none,id=disk%u,format=raw,file=%s,readonly=%s\n"
                  "-device\nvirtio-blk-device,drive=disk%u,bus=virtio-mmio-bus.%u\n",
                  i, disks[i].path, disks[i].writable ? "off" : "on", i, i);
  }
  if (n < 0 || (size_t)n >= sizeof line) { fprintf(stderr, "xnuloader-qemu: OUTDIR or the disks' paths too long\n"); return 1; }
  spit(out, "qemu.args", line, (size_t)n);

  printf("xnuloader-qemu: %s %s, base 0x%llx, entry 0x%llx\n", k.fileset ? "MH_FILESET" : "MH_EXECUTE", k.cpusubtype == 2 ? "arm64e" : "arm64",
         (unsigned long long)k.base, (unsigned long long)k.entry);
  printf("  window at PA 0x%llx (%u MiB), %llu MiB used; entry PA 0x%llx; boot_args PA 0x%llx; device tree PA 0x%llx (%llu bytes)\n",
         (unsigned long long)phys, ram_mib, (unsigned long long)(lay.used >> 20), (unsigned long long)lay.entry_pa,
         (unsigned long long)lay.args_pa, (unsigned long long)lay.tree_pa, (unsigned long long)lay.tree_size);
  printf("  stub at PA 0x%llx; QEMU RAM %llu MiB; %u CPU%s (%s), timebase %u Hz; boot args: %s\n", (unsigned long long)STUB_PA,
         (unsigned long long)qemu_mib, cpus, cpus == 1 ? "" : "s", qemu_cpu, timebase, args);
  if (trust.data) printf("  trust caches: %u modules, %zu bytes; root: the OS volume %s\n", (unsigned)trust.data[0], trust.size, m.root_volume_uuid);
  for (unsigned i = 0; i < ndisk; i++)
    printf("  disk %u (transport 0x%llx): %s, %s, %s%s\n", i, (unsigned long long)(VIRTIO_BASE + i * VIRTIO_SIZE), disks[i].identity.role,
           disks[i].path, disks[i].writable ? "writable" : "read-only", disks[i].list ? ", hash list in its node" : "");
  return 0;
}
