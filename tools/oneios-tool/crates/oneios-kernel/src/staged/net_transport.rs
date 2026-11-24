// From build-oneios-net.py (stage_transport), written out by a helper of the port and kept by hand since.
/// DroidVMVirtioNet.c's line that changes.
pub const NET_TRANSPORT: &[(&str, &str)] = &[
    ("            if (broadcast || ours) {\n",
     "            if (broadcast || ours || (dst[0] & 1)) {   // OneiOS (build-oneios-net.py): multicast too\n"),
];

/// The native kernel's virtio-net on crosvm's PCI (xnuloader-crosvm --wifi): Droid-VM's crosvm has its --net card on
/// virtio-pci and picks its device, so /arm-io/net ("oneios,virtio-net-pci") is all of bus 0 of its ECAM and the transport
/// takes the first 1af4:1041 function there. DroidVMVirtioNet.c's lines that change for it, after NET_TRANSPORT and compiled
/// only with XNU_ONEIOS_NATIVE: the MMIO transport's register offsets go to the function's regions (VirtIO 1.2 4.1.4), as
/// the sound card's do (audio.rs SND_TRANSPORT_PCI), with the card's configuration (the MAC at 0x100) and the ISR
/// (InterruptStatus at 0x60: reading it acknowledges). The queues stay in the transport's DMA region of the guest's RAM,
/// which the shared-RAM VM's host reads; polled (no droidvm,intid), as before. The hosted kernel compiles the file as before.
pub const NET_TRANSPORT_PCI: &[(&str, &str)] = &[
    (r#"// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t net_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return value;
}
static uint8_t net_read8(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return (uint8_t)value;
}
static void net_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(droidvm_net.registers + offset) : "memory");
}
"#,
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/net_transport.rs NET_TRANSPORT_PCI): the card as a virtio-pci function of crosvm's ECAM host
// (/arm-io/net "oneios,virtio-net-pci" is bus 0, xnuloader-crosvm --wifi), the MMIO transport's offsets (VirtIO 1.2 4.2.2)
// on its regions (4.1.4), as the sound card's (staged/audio.rs SND_TRANSPORT_PCI). Single loads and stores with a plain base
// register, as the MMIO accessors.
extern int printf(const char *, ...);
static bool net_pci;
static uintptr_t net_pci_common, net_pci_notify, net_pci_isr, net_pci_device;
static uint32_t net_pci_multiplier;
static uint16_t net_pci_notify_off[2];
static uint32_t net_pci_rd32(uintptr_t p) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static uint16_t net_pci_rd16(uintptr_t p) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint16_t)v; }
static uint8_t net_pci_rd8(uintptr_t p) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint8_t)v; }
static void net_pci_wr32(uintptr_t p, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(p) : "memory"); }
static void net_pci_wr16(uintptr_t p, uint16_t v) { __asm__ volatile("strh %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static void net_pci_wr8(uintptr_t p, uint8_t v) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static uint32_t net_pci_read(unsigned offset) {
    if (offset >= 0x100) return net_pci_rd8(net_pci_device + (offset - 0x100));   // the card's configuration, a byte
    switch (offset) {
    case 0x00: return 0x74726976;                                // MagicValue, Version, DeviceID: the function is 1af4:1041
    case 0x04: return 2;
    case 0x08: return 1;
    case 0x10: return net_pci_rd32(net_pci_common + 0x04);       // DeviceFeatures, of the word DeviceFeaturesSel chose
    case 0x34: return net_pci_rd16(net_pci_common + 0x18);       // QueueNumMax: the selected queue's size until it is set
    case 0x44: return net_pci_rd16(net_pci_common + 0x1c);       // QueueReady: queue_enable
    case 0x60: return net_pci_rd8(net_pci_isr) & 3;              // InterruptStatus: reading the ISR acknowledges it
    case 0x70: return net_pci_rd8(net_pci_common + 0x14);        // Status
    default: return 0;
    }
}
static void net_pci_write(unsigned offset, uint32_t value) {
    switch (offset) {
    case 0x14: net_pci_wr32(net_pci_common + 0x00, value); break;   // DeviceFeaturesSel
    case 0x20: net_pci_wr32(net_pci_common + 0x0c, value); break;   // DriverFeatures
    case 0x24: net_pci_wr32(net_pci_common + 0x08, value); break;   // DriverFeaturesSel
    case 0x30:                                                      // QueueSel, and that queue's notification offset
        net_pci_wr16(net_pci_common + 0x16, (uint16_t)value);
        if (value < 2) net_pci_notify_off[value] = net_pci_rd16(net_pci_common + 0x1e);
        break;
    case 0x38: net_pci_wr16(net_pci_common + 0x18, (uint16_t)value); break;
    case 0x44: net_pci_wr16(net_pci_common + 0x1c, (uint16_t)value); break;
    case 0x50:                                                      // QueueNotify
        if (value < 2) net_pci_wr16(net_pci_notify + (uintptr_t)net_pci_notify_off[value] * net_pci_multiplier, (uint16_t)value);
        break;
    case 0x70: net_pci_wr8(net_pci_common + 0x14, (uint8_t)value); break;
    case 0x80: case 0x84: net_pci_wr32(net_pci_common + 0x20 + (offset - 0x80), value); break;   // queue_desc
    case 0x90: case 0x94: net_pci_wr32(net_pci_common + 0x28 + (offset - 0x90), value); break;   // queue_driver
    case 0xa0: case 0xa4: net_pci_wr32(net_pci_common + 0x30 + (offset - 0xa0), value); break;   // queue_device
    default: break;                                                 // InterruptACK (0x64): the ISR's read did it
    }
}
// The function's regions from its vendor capabilities: common configuration (1), notifications (2), ISR (3) and the card's
// configuration (4), each in the BAR crosvm assigned, mapped as device memory. False when one is missing.
static bool net_pci_regions(uintptr_t config) {
    if (!(net_pci_rd16(config + 0x06) & 0x10)) return false;                    // no capabilities list
    struct { uint8_t bar; uint32_t offset, length; bool found; } r[5];
    memset(r, 0, sizeof(r));
    uint32_t multiplier = 0;
    uint8_t at = net_pci_rd8(config + 0x34) & 0xfc;
    for (unsigned walked = 0; at >= 0x40 && at <= 0x100 - 20 && walked < 48; ++walked) {
        const uint8_t id = net_pci_rd8(config + at), next = net_pci_rd8(config + at + 1) & 0xfc, type = net_pci_rd8(config + at + 3);
        if (id == 0x09 && type >= 1 && type <= 4 && !r[type].found) {
            r[type].bar = net_pci_rd8(config + at + 4);
            r[type].offset = net_pci_rd32(config + at + 8);
            r[type].length = net_pci_rd32(config + at + 12);
            r[type].found = true;
            if (type == 2) multiplier = net_pci_rd32(config + at + 16);
        }
        at = next;
    }
    if (!r[1].found || !r[2].found || !r[3].found || !r[4].found || r[1].length < 0x38 || r[2].length < 2 || r[3].length < 1 ||
        r[4].length < 8)
        return false;
    net_pci_wr16(config + 0x04, (uint16_t)(net_pci_rd16(config + 0x04) | 6));   // memory space and bus master
    uintptr_t mapped[5] = {0};
    for (unsigned t = 1; t <= 4; ++t) {
        const unsigned bar = r[t].bar;
        if (bar > 5 || r[t].offset > UINT32_MAX - r[t].length) return false;
        const uint32_t low = net_pci_rd32(config + 0x10 + bar * 4);
        if (low & 1) return false;                                              // an I/O BAR
        uint64_t base = low & ~(uint64_t)0xf;
        if ((low & 6) == 4) base |= (uint64_t)net_pci_rd32(config + 0x10 + bar * 4 + 4) << 32;
        if (!base) return false;
        const uint64_t from = (base + r[t].offset) & ~(uint64_t)0x3fff;
        const uint64_t to = (base + r[t].offset + r[t].length + 0x3fff) & ~(uint64_t)0x3fff;
        const uintptr_t m = ml_io_map((vm_offset_t)from, (vm_size_t)(to - from));
        if (!m) return false;
        mapped[t] = m + (uintptr_t)(base + r[t].offset - from);
    }
    net_pci_common = mapped[1]; net_pci_notify = mapped[2]; net_pci_isr = mapped[3]; net_pci_device = mapped[4];
    net_pci_multiplier = multiplier;
    return true;
}
// The bus's first 1af4:1041 function (function 0 of each device: 32 KiB a device on crosvm's ECAM) with its regions mapped:
// its configuration space, or 0.
static uintptr_t net_pci_find(vm_offset_t bus, uint64_t size) {
    const uintptr_t space = ml_io_map(bus, (vm_size_t)size);
    if (!space) { printf("droidvm-net: the PCI bus at %#llx not mapped\n", (unsigned long long)bus); return 0; }
    for (unsigned dev = 0; dev < 32 && ((uint64_t)dev << 15) + 0x1000 <= size; ++dev) {
        const uintptr_t config = space + ((uintptr_t)dev << 15);
        if (net_pci_rd32(config) != (0x1af4u | 0x1041u << 16)) continue;
        if (!net_pci_regions(config)) { printf("droidvm-net: the network card at 00:%02x.0 has not its regions\n", dev); return 0; }
        net_pci = true;
        printf("droidvm-net: virtio-pci network card at 00:%02x.0 (1af4:1041), notifications x%u, polled\n", dev, net_pci_multiplier);
        return config;
    }
    printf("droidvm-net: no 1af4:1041 function on bus 0 (crosvm without --net?)\n");
    return 0;
}
static uint32_t net_read32(unsigned offset) {
    if (net_pci) return net_pci_read(offset);
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return value;
}
static uint8_t net_read8(unsigned offset) {
    if (net_pci) return (uint8_t)net_pci_read(offset);
    uint32_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return (uint8_t)value;
}
static void net_write32(unsigned offset, uint32_t value) {
    if (net_pci) { net_pci_write(offset, value); return; }
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(droidvm_net.registers + offset) : "memory");
}
#else
// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t net_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return value;
}
static uint8_t net_read8(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return (uint8_t)value;
}
static void net_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(droidvm_net.registers + offset) : "memory");
}
#endif
"#),
    ("    droidvm_net.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    // OneiOS native: on crosvm's ECAM bus ("oneios,virtio-net-pci", xnuloader-crosvm --wifi) its 1af4:1041 function,
    // wherever crosvm put it; an MMIO window as before
    if (SecureDTGetProperty(node, "compatible", &value, &size) == kSuccess && size >= 22 &&
        !memcmp(value, "oneios,virtio-net-pci", 22))
        droidvm_net.registers = net_pci_find((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), reg[1]);
    else
#endif
    droidvm_net.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
"#),
];
