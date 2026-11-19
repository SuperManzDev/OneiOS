// SPDX-License-Identifier: MIT
// xnuloader-qemu: a kernel placed for qemu-system-aarch64's virt board the way a firmware loader (DarwinBoot) places it
// on a phone: EL1, MMU and caches off, at the physical address of its entry, x0 = the physical address of boot_args.
// The bench for the standalone kernel (kernel/build-linux.sh) before any phone (docs/native.md).
//   xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT]
// Writes into OUTDIR:
//   window.bin  the kernel's window from its physical base: its segments, the device tree and boot_args
//               (xl_boot_place, the code xnuhost uses, with virt_base = the kernel's own base as a firmware gives it)
//   stub.bin    2 MiB into RAM: interrupts masked, x0 = boot_args, a branch to the entry
//   board.adt   the device tree the kernel was given (xnuloader-inspect adt FILE prints it)
//   qemu.args   the machine and the two loader devices, one argument per line (scripts/qemu/boot-vm.sh)
// and prints the layout. The board: virt with GICv3 (distributor 0x08000000, redistributors 0x080a0000, 0x20000 a
// CPU), the PL011 at 0x09000000, PSCI by HVC (the machine has no EL2 or EL3), no devices yet.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

#define RAM_BASE      0x40000000ull     // virt's DRAM
#define STUB_PA       (RAM_BASE + 0x200000) // past the FDT QEMU writes at the start of RAM (1 MiB) for a bare image
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB: 16K's L2 blocks, and 4K's 2 MiB ones

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
  fprintf(stderr, "usage: xnuloader-qemu KERNEL OUTDIR [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT]\n");
  exit(2);
}

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args;
  unsigned cpus = 1, ram_mib = 1024;
  uint32_t timebase = 1000000000;        // CNTFRQ_EL0 of virt with -cpu max since QEMU 9.0 (62.5 MHz before, and for older models)
  for (int i = 3; i < argc; i++) {
    if (i + 1 >= argc) usage();
    if (!strcmp(argv[i], "--cpus")) cpus = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--ram-mib")) ram_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--timebase")) timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--args")) args = argv[++i];
    else usage();
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 256 || ram_mib > 3072) { fprintf(stderr, "xnuloader-qemu: 1-%d CPUs, 256-3072 MiB\n", XL_MAX_CPUS); return 2; }

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
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-qemu: %u MiB: out of memory\n", ram_mib); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args };
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
  char line[8192];
  int n = snprintf(line, sizeof line,
                   "-M\nvirt,gic-version=3,virtualization=off,secure=off,highmem=off\n-cpu\nmax,pauth-impdef=on\n-smp\n%u\n-m\n%lluM\n"
                   "-device\nloader,file=%s/window.bin,addr=0x%llx,force-raw=on\n-device\nloader,file=%s/stub.bin,addr=0x%llx,cpu-num=0,force-raw=on\n",
                   cpus, (unsigned long long)qemu_mib, out, (unsigned long long)phys, out, (unsigned long long)STUB_PA);
  if (n < 0 || (size_t)n >= sizeof line) { fprintf(stderr, "xnuloader-qemu: OUTDIR too long\n"); return 1; }
  spit(out, "qemu.args", line, (size_t)n);

  printf("xnuloader-qemu: %s %s, base 0x%llx, entry 0x%llx\n", k.fileset ? "MH_FILESET" : "MH_EXECUTE", k.cpusubtype == 2 ? "arm64e" : "arm64",
         (unsigned long long)k.base, (unsigned long long)k.entry);
  printf("  window at PA 0x%llx (%u MiB), %llu MiB used; entry PA 0x%llx; boot_args PA 0x%llx; device tree PA 0x%llx (%llu bytes)\n",
         (unsigned long long)phys, ram_mib, (unsigned long long)(lay.used >> 20), (unsigned long long)lay.entry_pa,
         (unsigned long long)lay.args_pa, (unsigned long long)lay.tree_pa, (unsigned long long)lay.tree_size);
  printf("  stub at PA 0x%llx; QEMU RAM %llu MiB; %u CPU%s, timebase %u Hz; boot args: %s\n", (unsigned long long)STUB_PA,
         (unsigned long long)qemu_mib, cpus, cpus == 1 ? "" : "s", timebase, args);
  return 0;
}
