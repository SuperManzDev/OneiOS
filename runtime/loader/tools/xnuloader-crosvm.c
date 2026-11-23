// SPDX-License-Identifier: MIT
// xnuloader-crosvm: the native kernel (kernel/build-native.sh) as the payload of a VM of Android's own crosvm under the
// phone's Gunyah (docs/native.md "A VM under Gunyah"). crosvm loads an arm64 Linux Image at the start of the VM's RAM
// (0x80000000, `oneios-tool phone vm-probe` saw it there) and enters it at EL1 with the MMU and caches off, x0 = its own
// FDT. This Image is the RAM as the kernel wants it: the Linux header in its first 64 bytes; at 0x40 xnuloader-qemu's
// stub (interrupts masked, x0 = boot_args, a branch to the entry); the kernel's window (xl_boot_place: its segments, the
// device tree, boot_args) at its physical base, 32 MiB aligned with the kernel's own base as on the bench.
//   xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--swiotlb MIB]
//                    [--image DIR [--phone-image DIR] [--phone-data FILE]]
// --ram-mib is the VM's (crosvm --mem): the window ends below crosvm's FDT and its restricted DMA pool (--swiotlb, 8 MiB
// by default, the RAM's last MIB). Writes OUT (the Image), OUT.adt (the device tree the kernel is given:
// xnuloader-inspect adt FILE) and, with --image, OUT.disks; prints the layout.
// The machine is crosvm's, as its FDT for a VM described it (vm-probe run 10, `fdt.dtb`): "linux,dummy-virt", RAM at
// 0x80000000, a GICv3 distributor at 0x3fff0000 and the redistributors right below it (0x20000 a vCPU), the arch timer
// (19.2 MHz on the S25: CNTFRQ_EL0 in the VM), PSCI by HVC, four ns16550a at 0x3f8, 0x2f8, 0x3e8 and 0x2e8. The console
// is the first, as the native kernel's ns16550 driver takes it (kernel patch 0077: the node's reg absolute, below the
// arm-io window, which may not start at 0 and so begins at 0x1000 as Droid-VM's make-droidvm-adt.py has it).
// --image DIR: the image set's disks (tools/image_disks.c, as xnuloader-qemu --pci): crosvm's virtio-blk PCI functions
// 00:02.0 to 00:06.0 (os, systemos, app, the Data seed, the Data store), which `phone vm-boot` pins with pci-address=;
// each node is the function's configuration space in crosvm's CAM (0x10000 + device << 11), with the image's hash list
// and a DMA window. A protected VM's host reads and writes only crosvm's restricted DMA pool (the RAM's last --swiotlb
// MiB: crosvm's get_swiotlb_addr, and the tree of every VM the S25 made so far): each disk gets 1 MiB + 64 KiB of it (its
// queue page and its transfer buffer), and the kernel's disk driver gives the device no other address. The files are the
// phone's (--phone-image DIR, default /data/local/tmp/oneios/image, the staged set) and the Data store a file of its own
// (--phone-data FILE, default /data/local/tmp/oneios-vmboot/data.img: not the hosted session's); DIR on this machine is
// the same set, read for the identities, the hash lists and the trust caches. OUT.disks lists them for vm-boot, one line
// each: "swiotlb-mib N", "pool 0xPA" (where crosvm's tree must put it), "store FILE GIB", and "block BYTES VALUE" per
// disk (BYTES the size the phone's file must have, "-" for the store; VALUE crosvm's --block value).
// --display WxH [--scale N] [--hz N] (with --image): the boot video, B,G,R,X at W * 4 bytes a row, at the START of the
// pool, the disks' windows after it (the pool then 32 MiB unless --swiotlb says more): the guest's display driver
// copies every frame into it (it has no virtio scanout here) and maps it write-back (staged/display.rs LEGACY_BOOT_VIDEO),
// and the phone's bridge (runtime/display displayctl pool) reads it out of the pool and hands it to oneios-display.
// OUT.disks then has "fb OFFSET BYTES WIDTH HEIGHT STRIDE" (OFFSET in the pool). /chosen display-scale and the boot
// arguments as xnuloader-qemu gives them (offline-legacy-unit-scale, sheng-vsync).
// The calendar: /chosen "oneios,rtc" is crosvm's PL030 (0x2000, absolute), which kernel patch 0078 reads for the time.
// The touch, with --display: the input bus, node /arm-io/input-pci (bus 0 of crosvm's CAM: the kernel finds crosvm's
// virtio-input functions there, wherever crosvm put them), and a 64 KiB window of the pool after the disks' for their
// queues and events (three 4 KiB pages a device); OUT.disks then has "input touch": vm-boot hands crosvm the phone's
// touchscreen (--evdev).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_disks.h"
#include "imageset.h"
#include "xnuloader/adt.h"
#include "xnuloader/boot.h"
#include "xnuloader/machine.h"
#include "xnuloader/macho.h"

#define RAM_BASE      0x80000000ull     // crosvm's DRAM on aarch64
#define KERNEL_ALIGN  (32ull << 20)     // the kernel's VA and PA agree modulo 32 MiB (xnuloader-qemu)
#define FDT_RESERVE   (8ull << 20)      // below the pool: crosvm's FDT (2 MiB, 2 MiB aligned, right below it) and room
#define ARMIO_BASE    0x1000ull         // crosvm's MMIO (RTC 0x2000, PCI 0x10000, GIC) up to 1 GiB; not 0
#define ARMIO_SIZE    (0x40000000ull - ARMIO_BASE)
#define GICD          0x3fff0000ull
#define GICD_SIZE     0x10000ull
#define GICR_STRIDE   0x20000ull        // GICv3 without VLPI: RD_base and SGI_base
#define UART          0x3f8ull          // crosvm's first ns16550a (serial num=1)
#define UART_SIZE     0x8ull
#define PCI_CAM       0x10000ull        // crosvm's CAM (pci-host-cam-generic): bus << 16 | device << 11 | function << 8
#define PCI_FIRST_DEV 2u                // disk K is 00:(2 + K).0 (pinned; crosvm's pvpanic device takes a free one)
#define DMA_SLICE     0x110000ull       // a disk's DMA window: its queue page, then 1 MiB of transfer buffer at 64 KiB
#define INPUT_WINDOW  0x10000ull        // the input devices' window: three 4 KiB pages a device
#define PCI_BUS0      0x10000ull        // bus 0 of the CAM: 32 devices, 0x800 apart
#define RTC           0x2000ull         // crosvm's PL030 (rtc@2000 in its tree): RTCDR is the host's seconds since 1970
#define RTC_SIZE      0x1000ull

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
  fprintf(stderr, "usage: xnuloader-crosvm KERNEL OUT [--cpus N] [--ram-mib N] [--timebase HZ] [--args TEXT] [--args-add TEXT] [--swiotlb MIB]\n"
                  "                        [--image DIR [--phone-image DIR] [--phone-data FILE] [--display WxH [--scale 2|3] [--hz N]]]\n");
  exit(2);
}

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// A path crosvm's --block takes as it is: no comma (its key separator), no quote or blank for the phone's shell.
static int plain_path(const char *path)
{
  if (!*path || *path != '/') return 0;
  for (const char *c = path; *c; c++)
    if (*c == ',' || *c == '\'' || *c == '"' || *c == ' ' || *c == '\n' || *c == '\\' || *c == '$' || *c == '`') return 0;
  return 1;
}

int main(int argc, char **argv)
{
  if (argc < 3) usage();
  const char *kernel_path = argv[1], *out = argv[2], *args = default_args, *image_dir = NULL;
  const char *phone_image = "/data/local/tmp/oneios/image", *phone_data = "/data/local/tmp/oneios-vmboot/data.img";
  unsigned cpus = 1, ram_mib = 2048, swiotlb_mib = 0;
  unsigned display_w = 0, display_h = 0, display_scale = 3, display_hz = 60;
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
    else if (!strcmp(argv[i], "--swiotlb")) swiotlb_mib = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--image")) image_dir = argv[++i];
    else if (!strcmp(argv[i], "--phone-image")) phone_image = argv[++i];
    else if (!strcmp(argv[i], "--phone-data")) phone_data = argv[++i];
    else if (!strcmp(argv[i], "--display")) { if (sscanf(argv[++i], "%ux%u", &display_w, &display_h) != 2) usage(); }
    else if (!strcmp(argv[i], "--scale")) display_scale = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--hz")) display_hz = (unsigned)strtoul(argv[++i], NULL, 0);
    else usage();
  }
  if (cpus < 1 || cpus > XL_MAX_CPUS || ram_mib < 512 || ram_mib > 6144) { fprintf(stderr, "xnuloader-crosvm: 1-%d CPUs, 512-6144 MiB\n", XL_MAX_CPUS); return 2; }
  if (display_w && (!image_dir || display_w < 320 || display_h < 320 || display_w > 4096 || display_h > 4096 ||
                    (display_scale != 2 && display_scale != 3) || display_hz < 24 || display_hz > 120)) {
    fprintf(stderr, "xnuloader-crosvm: --display 320x320 to 4096x4096 with --image, --scale 2 or 3, --hz 24 to 120\n");
    return 2;
  }
  // The boot video's slot at the pool's start (2 MiB aligned), the disks' windows after it.
  // and the frame sequence's page right after it (16 KiB, the guest's page: OneiOSDisplay.cpp oneios_display_frame_done)
  const uint64_t fb_stride = (uint64_t)display_w * 4, fb_bytes = fb_stride * display_h, seq_off = (fb_bytes + 0x3fff) & ~0x3fffull;
  const uint64_t fb_slot = display_w ? (seq_off + 0x4000 + 0x1fffff) & ~0x1fffffull : 0;
  if (!swiotlb_mib) swiotlb_mib = display_w ? 32 : 8;
  if (swiotlb_mib < 2 || swiotlb_mib > 64 || (swiotlb_mib & 1)) { fprintf(stderr, "xnuloader-crosvm: --swiotlb 2 to 64 MiB, even\n"); return 2; }
  static char with_display[4096];
  if (display_w) {
    // what xnuhost passes beside the tree's display-scale: the backend's unit scale and its vsync tick (display/README.md)
    size_t at = (size_t)snprintf(with_display, sizeof with_display, "%s", args);
    if (at < sizeof with_display && !strstr(args, "offline-legacy-unit-scale="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " offline-legacy-unit-scale=%u", display_scale);
    if (at < sizeof with_display && !strstr(args, "sheng-vsync="))
      at += (size_t)snprintf(with_display + at, sizeof with_display - at, " sheng-vsync=%u", display_hz);
    if (at >= sizeof with_display) { fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n"); return 2; }
    args = with_display;
  }
  if (image_dir && (!plain_path(phone_image) || !plain_path(phone_data))) {
    fprintf(stderr, "xnuloader-crosvm: --phone-image and --phone-data: absolute paths without commas, quotes or blanks\n");
    return 2;
  }

  // The image set's disks: this machine's copy for what the nodes say, the phone's files for crosvm.
  static xh_imageset set;
  static image_disk disks[IMAGE_DISKS_MAX];
  static char seed_hex[65];
  unsigned ndisk = image_dir ? image_disks(image_dir, phone_data, false, &set, disks, seed_hex) : 0;
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

  const uint64_t phys = RAM_BASE + KERNEL_ALIGN + (k.base & (KERNEL_ALIGN - 1));
  const uint64_t ram_end = RAM_BASE + ((uint64_t)ram_mib << 20);
  // crosvm's restricted DMA pool: the RAM's last MIB (aarch64/src/lib.rs get_swiotlb_addr; Gunyah wants it static).
  const uint64_t pool = ram_end - ((uint64_t)swiotlb_mib << 20), top_reserve = ((uint64_t)swiotlb_mib << 20) + FDT_RESERVE;
  if (phys + (256ull << 20) + top_reserve > ram_end) { fprintf(stderr, "xnuloader-crosvm: %u MiB leave the kernel less than 256 MiB\n", ram_mib); return 2; }
  // the input window after the disks', 16 KiB aligned (the guest's page)
  const uint64_t input_off = display_w ? (fb_slot + ndisk * DMA_SLICE + 0x3fff) & ~0x3fffull : 0;
  const uint64_t pool_need = display_w ? input_off + INPUT_WINDOW : fb_slot + ndisk * DMA_SLICE;
  if (pool_need > ((uint64_t)swiotlb_mib << 20)) {
    fprintf(stderr, "xnuloader-crosvm: the boot video, %u disks and the input need %llu KiB of the pool\n", ndisk, (unsigned long long)(pool_need >> 10));
    return 2;
  }
  const uint64_t fb_pa = pool, disks_pa = pool + fb_slot, input_pa = pool + input_off;
  if (display_w) {
    // the page the guest counts its frames in (a new argument: the user's --args cannot have named the pool)
    size_t at = strlen(with_display);
    if (snprintf(with_display + at, sizeof with_display - at, " oneios-frame-seq=0x%llx", (unsigned long long)(fb_pa + seq_off)) >= (int)(sizeof with_display - at)) {
      fprintf(stderr, "xnuloader-crosvm: boot arguments too long\n");
      return 2;
    }
  }
  const uint64_t window = (ram_end - top_reserve - phys) & ~0x1fffffull;

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
  if (display_w) {
    m.display_width = display_w;
    m.display_height = display_h;
    m.display_scale = display_scale;
    m.display_hz = display_hz;
  }
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r || fread(m.random_seed, 1, sizeof m.random_seed, r) != sizeof m.random_seed) { fprintf(stderr, "/dev/urandom: read failed\n"); return 1; }
  fclose(r);
  // The disks: PCI function 00:(2 + K).0 for disk K, its configuration space the node's reg.
  for (unsigned i = 0; i < ndisk; i++)
    m.dev[m.ndev++] = (xl_device){ disks[i].writable ? XL_DEV_DATA_STORE : XL_DEV_BLOCK, PCI_CAM + ((uint64_t)(PCI_FIRST_DEV + i) << 11), 0x100, true };
  if (display_w) m.dev[m.ndev++] = (xl_device){ XL_DEV_INPUT, PCI_CAM, PCI_BUS0, true };   // the input bus
  const xh_imageset_file *os = image_dir ? xh_imageset_role(&set, "os") : NULL;
  if (os && *os->volume_uuid) m.root_volume_uuid = os->volume_uuid;
  m.trust_cache = trust.data != NULL;
  xl_adt_node *tree = xl_machine_tree(&m, &err);
  if (!tree) { fprintf(stderr, "board: %s\n", err); return 1; }
  for (unsigned i = 0, blocks = 0; i < ndisk; i++) {
    char name[40];
    if (disks[i].writable) snprintf(name, sizeof name, "arm-io/data-store");
    else snprintf(name, sizeof name, "arm-io/virtio-pci-%u", blocks++);
    xl_adt_node *node = xl_adt_find(tree, name);
    if (!node) { fprintf(stderr, "board: no /%s\n", name); return 1; }
    xh_imageset_describe(node, &disks[i].identity);
    const uint64_t dma[2] = { disks_pa + i * DMA_SLICE, DMA_SLICE };
    if ((disks[i].list && xl_adt_set(node, "oneios,hash-list", disks[i].list, disks[i].list_size)) ||
        xl_adt_set(node, "oneios,dma-window", dma, sizeof dma)) {
      fprintf(stderr, "board: /%s: no room for its hash list or DMA window\n", name);
      return 1;
    }
  }
  if (display_w) {
    xl_adt_node *bus = xl_adt_find(tree, "arm-io/input-pci");
    const uint64_t window[2] = { input_pa, INPUT_WINDOW };
    if (!bus || xl_adt_set(bus, "oneios,dma-window", window, sizeof window)) { fprintf(stderr, "board: no /arm-io/input-pci for the touch\n"); return 1; }
  }
  // The calendar: crosvm's PL030, absolute (below the arm-io window), for kernel patch 0078's getGMTTimeOfDay.
  {
    xl_adt_node *chosen = xl_adt_find(tree, "chosen");
    const uint64_t rtc[2] = { RTC, RTC_SIZE };
    if (!chosen || xl_adt_set(chosen, "oneios,rtc", rtc, sizeof rtc)) { fprintf(stderr, "board: no /chosen for the clock\n"); return 1; }
  }
  // The console: the node /defaults serial-device names by its AAPL,phandle (pe_serial.c get_serial_device_phandle).
  xl_adt_node *uart = xl_adt_find(tree, "arm-io/uart0"), *defaults = xl_adt_find(tree, "defaults");
  if (!uart || !defaults || xl_adt_set_u32(uart, "AAPL,phandle", 0x10001) || xl_adt_set_u32(defaults, "serial-device", 0x10001)) {
    fprintf(stderr, "board: no /arm-io/uart0 or /defaults for the console\n");
    return 1;
  }

  uint8_t *ram = calloc(1, window);
  if (!ram) { fprintf(stderr, "xnuloader-crosvm: out of memory\n"); return 1; }
  xl_boot_request req = { .image = { image, size }, .kernel = &k, .ram = ram, .window = window, .phys_base = phys,
                          .virt_base = k.base, .tree = tree, .command_line = args, .trustcache = trust,
                          .video = { display_w ? fb_pa : 0, display_w ? 1 : 0, fb_stride, display_w, display_h, display_w ? 32 : 0 } };
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
  // the window was cut for that much RAM, that many vCPUs and that pool
  char label[128];
  int label_len = snprintf(label, sizeof label, "oneios-crosvm ram-mib=%u cpus=%u swiotlb-mib=%u", ram_mib, cpus, swiotlb_mib);
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
  if (ndisk) {
    // OUT.disks for vm-boot: the pool crosvm's tree must name, the store to make, and crosvm's --block values.
    char list[4096];
    snprintf(list, sizeof list, "%s.disks", out);
    FILE *d = fopen(list, "w");
    if (!d) { fprintf(stderr, "%s: %s\n", list, strerror(errno)); return 1; }
    fprintf(d, "swiotlb-mib %u\npool 0x%llx\nstore %s %u\n", swiotlb_mib, (unsigned long long)pool, phone_data, set.store_gib ? set.store_gib : 16);
    if (display_w)
      fprintf(d, "fb %llu %llu %u %u %llu %llu\n", (unsigned long long)(fb_pa - pool), (unsigned long long)fb_bytes, display_w, display_h,
              (unsigned long long)fb_stride, (unsigned long long)(fb_pa - pool + seq_off));
    if (display_w) fprintf(d, "input touch\n");
    for (unsigned i = 0; i < ndisk; i++) {
      char file[1200];
      if (disks[i].writable) snprintf(file, sizeof file, "%s", phone_data);
      else snprintf(file, sizeof file, "%s/%s", phone_image, disks[i].rel);
      if (!plain_path(file)) { fprintf(stderr, "%s: a path crosvm or the phone's shell would split\n", file); return 1; }
      if (disks[i].writable) fprintf(d, "block - path=%s,ro=false,id=%s,pci-address=00:%02x.0\n", file, disks[i].identity.role, PCI_FIRST_DEV + i);
      else fprintf(d, "block %llu path=%s,ro=true,id=%s,pci-address=00:%02x.0\n", (unsigned long long)disks[i].size, file,
                   disks[i].identity.role, PCI_FIRST_DEV + i);
    }
    if (fclose(d)) { fprintf(stderr, "%s: write failed\n", list); return 1; }
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
  printf("  restricted DMA pool (crosvm --swiotlb %u) at PA 0x%llx\n", swiotlb_mib, (unsigned long long)pool);
  if (display_w)
    printf("  display: %ux%u @%ux, %u Hz; the boot video at PA 0x%llx (the pool's start, %llu bytes, %llu a row); the disks' windows from 0x%llx\n",
           display_w, display_h, display_scale, display_hz, (unsigned long long)fb_pa, (unsigned long long)fb_bytes,
           (unsigned long long)fb_stride, (unsigned long long)disks_pa);
  if (display_w)
    printf("  touch: crosvm's virtio-input functions on PCI bus 0 (the kernel finds them), their pages in the pool at 0x%llx (%llu KiB)\n",
           (unsigned long long)input_pa, (unsigned long long)(INPUT_WINDOW >> 10));
  if (trust.data) printf("  trust caches: %u modules, %zu bytes; root: the OS volume %s\n", (unsigned)trust.data[0], trust.size, m.root_volume_uuid);
  for (unsigned i = 0; i < ndisk; i++)
    printf("  disk %u (PCI 00:%02x.0, config 0x%llx, DMA window 0x%llx+0x%llx): %s, %s%s%s, %s%s\n", i, PCI_FIRST_DEV + i,
           (unsigned long long)(PCI_CAM + ((uint64_t)(PCI_FIRST_DEV + i) << 11)), (unsigned long long)(disks_pa + i * DMA_SLICE),
           (unsigned long long)DMA_SLICE, disks[i].identity.role, disks[i].writable ? phone_data : phone_image,
           disks[i].writable ? "" : "/", disks[i].writable ? "" : disks[i].rel, disks[i].writable ? "writable" : "read-only",
           disks[i].list ? ", hash list in its node" : "");
  return 0;
write_failed:
  fprintf(stderr, "%s: write failed\n", out);
  return 1;
}
