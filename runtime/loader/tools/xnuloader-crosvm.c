// SPDX-License-Identifier: MIT
// xnuloader-crosvm: the native kernel (kernel/build-native.sh) as the payload of a VM of Android's own crosvm under the
// phone's Gunyah (docs/native.md "A VM under Gunyah"). crosvm loads an arm64 Linux Image at the start of the VM's RAM
// (0x80000000, `oneios-tool phone vm-probe` saw it there) and enters it at EL1 with the MMU and caches off, x0 = its own
// FDT. This Image is the RAM as the kernel wants it: the Linux header in its first 64 bytes; at 0x40 xnuloader-qemu's
// stub (interrupts masked, x0 = boot_args, a branch to the entry); the kernel's window (xl_boot_place: its segments, the
// device tree, boot_args) at its physical base, 32 MiB aligned with the kernel's own base as on the bench.
//   xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT]
// --ram-mib is the VM's (crosvm --mem): the window ends TOP_RESERVE before the RAM's end, where crosvm puts its FDT and
// its restricted DMA pool (--swiotlb, 8 MiB in vm-probe). Writes OUT (the Image) and OUT.adt (the device tree the kernel
// is given: xnuloader-inspect adt FILE), and prints the layout.
// The machine is crosvm's, as its FDT for a VM described it (vm-probe run 10, `fdt.dtb`): "linux,dummy-virt", RAM at
// 0x80000000, a GICv3 distributor at 0x3fff0000 and the redistributors right below it (0x20000 a vCPU), the arch timer
// (19.2 MHz on the S25: CNTFRQ_EL0 in the VM), PSCI by HVC, four ns16550a at 0x3f8, 0x2f8, 0x3e8 and 0x2e8. The console
// is the first, as the native kernel's ns16550 driver takes it (kernel patch 0077: the node's reg absolute, below the
// arm-io window, which may not start at 0 and so begins at 0x1000 as Droid-VM's make-droidvm-adt.py has it). No device
// yet: crosvm's are on PCI.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

#define RAM_BASE      0x80000000ull     // crosvm's DRAM on aarch64
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB (xnuloader-qemu)
#define TOP_RESERVE   (16ull << 20)     // crosvm's FDT (2 MiB below the pool) and its restricted DMA pool (8 MiB)
#define ARMIO_BASE    0x1000ull         // crosvm's MMIO (RTC 0x2000, PCI 0x10000, GIC) up to 1 GiB; not 0
#define ARMIO_SIZE    (0x40000000ull - ARMIO_BASE)
#define GICD          0x3fff0000ull
#define GICD_SIZE     0x10000ull
#define GICR_STRIDE   0x20000ull        // GICv3 without VLPI: RD_base and SGI_base
#define UART          0x3f8ull          // crosvm's first ns16550a (serial num=1)
#define UART_SIZE     0x8ull

// xnuloader-qemu's arguments: xnuhost's default (runtime/xnuhost/src/config.c), less the hosted kernel's devices.
static const char default_args[] =
  "debug=0x14e serial=3 -v kt_fixed_zones=120 -no_slto_panic ml-timeout-lock-panic-scale=40 tlto_us=0 "
  "sheng-smp=1 armcpu-self-ipi=1 armcpu-spi-ipi=1 use_contiguous_hint=0 interrupt-masked-debug-mode=0 "
  "sched_preemption_disable_debug_mode=0 wdt=-1";

static uint8_t *slurp(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

static void usage(void)
{
  fprintf(stderr, "usage: xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT]\n");
  exit(2);
}

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args;
  unsigned cpus = 1, ram_mib = 2048;
  uint32_t timebase = 19200000;         // the S25's (vm-probe: CNTFRQ_EL0 in the VM)
  for (int i = 3; i < argc; i++) {
    if (i + 1 >= argc) usage();
    if (!strcmp(argv[i], "--cpus")) cpus = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--ram-mib")) ram_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--timebase")) timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--args")) args = argv[++i];
    else if (!strcmp(argv[i], "--args-add")) {
      static char joined[4096];
      if (snprintf(joined, sizeof joined, "%s %s", args, argv[++i]) >= (int)sizeof joined) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
      args = joined;
    }
    else usage();
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 512 || ram_mib > 6144) { fprintf(stderr, "xnuloader-crosvm: 1-%d CPUs, 512-6144 MiB\n", XL_MAX_CPUS); return 2; }

  const char *err = NULL;
  size_t size;
  uint8_t *image = slurp(kernel_path, &size);
  static xl_kernel k;
  if (xl_macho_parse(image, size, &k, &err)) { fprintf(stderr, "%s: %s\n", kernel_path, err); return 1; }

  const uint64_t phys = RAM_BASE + KERNEL_ALIGN + (k.base & (KERNEL_ALIGN - 1));
  const uint64_t ram_end = RAM_BASE + ((uint64_t)ram_mib << 20);
  if (phys + (256ull << 20) + TOP_RESERVE > ram_end) { fprintf(stderr, "xnuloader-crosvm: %u MiB leave the kernel less than 256 MiB\n", ram_mib); return 2; }
  const uint64_t window = (ram_end - TOP_RESERVE - phys) & ~0x1fffffull;

  xl_machine m;
  memset(&m, 0, sizeof m);
  m.ncpu = cpus;
  for (unsigned i = 0; i < cpus; i++) { m.cpu_reg[i] = i; m.cpu_cluster[i] = 'P'; }   // crosvm: Aff0 = the vCPU's index
  m.timebase_hz = timebase;
  m.dram_base = phys;
  m.dram_size = window;
  m.armio_base = ARMIO_BASE;
  m.armio_size = ARMIO_SIZE;
  m.gic_dist = GICD;
  m.gic_dist_size = GICD_SIZE;
  m.gic_redist = GICD - GICR_STRIDE * cpus;
  m.gic_redist_size = GICR_STRIDE * cpus;
  m.gic_redist_stride = GICR_STRIDE;
  m.uart_base = UART;
  m.uart_size = UART_SIZE;
  m.uart_ns16550 = true;
  m.psci_method = "hvc";
  m.model = "iPhone18,2";
  m.have_chip_id = true;
  m.chip_id = 0x8150;
  m.unique_chip_id = 0x0ce10511e5000002ull;     // a VM's own: not a phone's, not the bench's
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r || fread(m.random_seed, 1, sizeof m.random_seed, r) != sizeof m.random_seed) { fprintf(stderr, "/dev/urandom: read failed\n"); return 1; }
  fclose(r);
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }
  // The console: the node /defaults serial-device names by its AAPL,phandle (pe_serial.c get_serial_device_phandle).
  xl_adt_node *uart = xl_adt_find(tree, "arm-io/uart0"), *defaults = xl_adt_find(tree, "defaults");
  if (!uart || !defaults || xl_adt_set_u32(uart, "AAPL,phandle", 0x10001) || xl_adt_set_u32(defaults, "serial-device", 0x10001)) {
    fprintf(stderr, "board: no /arm-io/uart0 or /defaults for the console\n");
    return 1;
  }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-crosvm: out of memory\n"); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args };
  xl_boot_layout lay;
  if (xl_boot_place(&req, &lay, &err)) { fprintf(stderr, "place: %s\n", err); return 1; }
  const uint64_t used = (lay.used + 0x3fff) & ~0x3fffull;

  // The Image: [the header and the stub][zeros][the window at phys - RAM_BASE]
  const uint64_t at = phys - RAM_BASE, total = at + used;
  uint8_t head[0x60];
  memset(head, 0, sizeof head);
  put32(head + 0x00, 0x14000010u);              // code0: b 0x40
  put64(head + 0x08, 0);                        // text_offset
  put64(head + 0x10, total);                    // image_size
  put64(head + 0x18, 0x2);                      // flags: little-endian, 4 KiB pages, 2 MiB-aligned base at DRAM's start
  memcpy(head + 0x38, "ARM\x64", 4);            // magic
  // 0x40: msr daifset, #0xf; ldr x0, [pc, #12]; ldr x1, [pc, #16]; br x1; then boot_args' and the entry's physical
  // addresses at 0x50 and 0x58, 8-byte aligned (MMU off: every load is to Device memory, which takes no unaligned one)
  put32(head + 0x40, 0xd5034fdfu);
  put32(head + 0x44, 0x58000060u);
  put32(head + 0x48, 0x58000081u);
  put32(head + 0x4c, 0xd61f0020u);
  put64(head + 0x50, lay.args_pa);
  put64(head + 0x58, lay.entry_pa);

  // At 0x100 a label of the VM this Image is made for, which `oneios-tool phone vm-boot` reads and starts crosvm with:
  // the window was cut for that much RAM and that many vCPUs
  char label[128];
  int label_len = snprintf(label, sizeof label, "oneios-crosvm ram-mib=%u cpus=%u", ram_mib, cpus);
  if (label_len < 0 || (size_t)label_len >= sizeof label) return 1;

  FILE *f = fopen(out, "wb");
  if (!f) { fprintf(stderr, "%s: %s\n", out, strerror(errno)); return 1; }
  static uint8_t zeros[1 << 16];
  if (fwrite(head, 1, sizeof head, f) != sizeof head) goto write_failed;
  if (fwrite(zeros, 1, 0x100 - sizeof head, f) != 0x100 - sizeof head || fwrite(label, 1, (size_t)label_len + 1, f) != (size_t)label_len + 1)
    goto write_failed;
  for (uint64_t left = at - 0x100 - (uint64_t)label_len - 1; left; ) {
    size_t n = left > sizeof zeros ? sizeof zeros : (size_t)left;
    if (fwrite(zeros, 1, n, f) != n) goto write_failed;
    left -= n;
  }
  if (fwrite(ram, 1, used, f) != used || fclose(f)) goto write_failed;
  {
    char adt[4096];
    snprintf(adt, sizeof adt, "%s.adt", out);
    FILE *a = fopen(adt, "wb");
    if (!a || fwrite(ram + (lay.tree_pa - phys), 1, lay.tree_size, a) != lay.tree_size || fclose(a)) { fprintf(stderr, "%s: write failed\n", adt); return 1; }
  }

  printf("xnuloader-crosvm: %s %s, base 0x%llx, entry 0x%llx\n", k.fileset ? "MH_FILESET" : "MH_EXECUTE", k.cpusubtype == 2 ? "arm64e" : "arm64",
         (unsigned long long)k.base, (unsigned long long)k.entry);
  printf("  Image %s: %llu MiB, loaded at PA 0x%llx; stub at 0x%llx: boot_args PA 0x%llx, entry PA 0x%llx\n", out,
         (unsigned long long)(total >> 20), (unsigned long long)RAM_BASE, (unsigned long long)(RAM_BASE + 0x40),
         (unsigned long long)lay.args_pa, (unsigned long long)lay.entry_pa);
  printf("  window at PA 0x%llx (%llu MiB of the VM's %u), %llu MiB used; device tree PA 0x%llx (%llu bytes)\n",
         (unsigned long long)phys, (unsigned long long)(window >> 20), ram_mib, (unsigned long long)(lay.used >> 20),
         (unsigned long long)lay.tree_pa, (unsigned long long)lay.tree_size);
  printf("  %u vCPU%s, timebase %u Hz, GICD 0x%llx, GICR 0x%llx, console ns16550a at 0x%llx; boot args: %s\n", cpus,
         cpus == 1 ? "" : "s", timebase, (unsigned long long)GICD, (unsigned long long)m.gic_redist, (unsigned long long)UART, args);
  return 0;
write_failed:
  fprintf(stderr, "%s: write failed\n", out);
  return 1;
}
