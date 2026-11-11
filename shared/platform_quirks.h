/* SPDX-License-Identifier: MIT */
/*
 * platform_quirks.h -- platform-neutral detection for the efi64boot /
 * efi32boot hosted-firmware loaders.
 *
 * Canonical copy: "Default Project/shared/platform_quirks.h".  Both loaders
 * include it via -I../shared.  Freestanding: no libc, no UEFI headers, only
 * <stdint.h> and GCC/Clang inline asm.  Compiles as both IA-32 and x86-64.
 *
 * DESIGN: ONE BUILD FOR EVERY MACHINE
 * -----------------------------------
 * Nothing here is keyed to a chipset.  The host-bridge VID/DID is read only
 * to print a human-readable label; all behaviour comes from probing the
 * ACPI power-management device by its PCI class code:
 *
 *   06:80 "other bridge" -> PIIX4-style PM (PMBA @0x40, PMIOSE @0x80 bit0)
 *   06:01 "ISA bridge"   -> ICH/LPC-style PM (PMBASE @0x40, ACPI_EN @0x44 bit7)
 *
 * The matching EDK2 change (shared/patch_ovmf_platform.py) applies the same
 * principle inside the firmware: DIDs without dedicated OVMF code fall back
 * to a conservative generic path, and PM registers are resolved from the
 * real device.  A new hypervisor or a new machine needs no source edit, no
 * patch and no separate firmware build.
 *
 * WHY NO RUNTIME DID SPOOF
 * ------------------------
 * Both loaders used to contain a "VMware fix" that tried to overwrite the PCI
 * host-bridge Device ID.  It never worked, for three independent reasons:
 *
 *   1. A 32-bit read at PCI config offset 0 returns VID in bits [15:0] and
 *      DID in bits [31:16].  Measured: QEMU -M pc = 0x12378086, -M q35 =
 *      0x29C08086, VMware = 0x71908086.  The old code compared against
 *      0x80867190, the byte-swapped nonsense value.
 *   2. It wrote to port 0x0CFA.  The correct port for the 16-bit DID at
 *      config offset 2 is 0xCFC + 2 = 0xCFE, and the config address must be
 *      dword-aligned (0x80000000, not 0x80000002).
 *   3. VID/DID are HwInit (read-only) in PCI config space.  Verified on QEMU
 *      10.2.2 via QMP: writing 0x1237 to 0xCFE left the dword unchanged.
 *      No runtime DID spoof is possible on QEMU, VMware or real silicon.
 *
 * So detection here is informational + a source of *legal* runtime quirks;
 * making OVMF portable is done once, in the firmware source, for everybody.
 */

#ifndef PLATFORM_QUIRKS_H
#define PLATFORM_QUIRKS_H

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* PCI configuration mechanism #1                                           */
/* ------------------------------------------------------------------------- */

#define PQ_CFG_ADDR_PORT 0x0CF8u
#define PQ_CFG_DATA_PORT 0x0CFCu

static inline void pq_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%w1" :: "a"(v), "Nd"(port) : "memory");
}
static inline uint8_t pq_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void pq_outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0,%w1" :: "a"(v), "Nd"(port) : "memory");
}
static inline uint16_t pq_inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %w1,%0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void pq_outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0,%w1" :: "a"(v), "Nd"(port) : "memory");
}
static inline uint32_t pq_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

/* Bits [7:2] of the config address carry the dword-aligned register number;
 * bits [1:0] must be zero.  Selecting a byte/word within the dword is done by
 * using data port 0xCFC + (reg & 3), NOT by putting the offset in 0xCF8. */
#define PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg)                        \
    (0x80000000u | ((uint32_t)(bus) << 16) |                           \
     ((uint32_t)((dev) & 0x1Fu) << 11) |                               \
     ((uint32_t)((fn) & 0x07u) << 8) | ((uint32_t)(reg) & 0xFCu))

static inline uint32_t pq_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                                     uint8_t reg) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    return pq_inl(PQ_CFG_DATA_PORT);
}
static inline uint16_t pq_pci_read16(uint8_t bus, uint8_t dev, uint8_t fn,
                                     uint8_t reg) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    return pq_inw((uint16_t)(PQ_CFG_DATA_PORT + ((reg) & 2u)));
}
static inline uint8_t pq_pci_read8(uint8_t bus, uint8_t dev, uint8_t fn,
                                   uint8_t reg) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    return pq_inb((uint16_t)(PQ_CFG_DATA_PORT + ((reg) & 3u)));
}
static inline void pq_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                                  uint8_t reg, uint32_t v) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    pq_outl(PQ_CFG_DATA_PORT, v);
}
static inline void pq_pci_write16(uint8_t bus, uint8_t dev, uint8_t fn,
                                  uint8_t reg, uint16_t v) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    pq_outw((uint16_t)(PQ_CFG_DATA_PORT + ((reg) & 2u)), v);
}
static inline void pq_pci_write8(uint8_t bus, uint8_t dev, uint8_t fn,
                                 uint8_t reg, uint8_t v) {
    pq_outl(PQ_CFG_ADDR_PORT, PQ_PCI_CONFIG_ADDRESS(bus, dev, fn, reg));
    pq_outb((uint16_t)(PQ_CFG_DATA_PORT + ((reg) & 3u)), v);
}

/* ------------------------------------------------------------------------- */
/* PCI class codes -- the ONLY thing used to decide behaviour                */
/* ------------------------------------------------------------------------- */

#define PQ_PM_CLASS_PIIX4  0x0680u   /* "other bridge": PIIX4-style ACPI PM  */
#define PQ_PM_CLASS_ICH    0x0601u   /* "ISA bridge":   ICH/LPC-style ACPI PM */

/* DIDs below are LABELS ONLY.  They never gate behaviour.  Adding a new
 * platform requires nothing here -- it is reported as generic and handled
 * by the class-code probe. */
#define PQ_DID_I440FX       0x1237u
#define PQ_DID_Q35_MCH      0x29C0u
#define PQ_DID_Q35_MCH_B0   0x29B0u
#define PQ_DID_VMWARE_440BX 0x7190u
#define PQ_DID_INTEL_915G   0x2580u
#define PQ_DID_INTEL_945G   0x2770u
#define PQ_DID_INTEL_945GM  0x27A0u
#define PQ_DID_INTEL_945GME 0x27ACu
#define PQ_DID_CLOUDHV      0x0D57u

/* ------------------------------------------------------------------------- */
/* Platform information                                                      */
/* ------------------------------------------------------------------------- */

typedef enum {
    PQ_PLAT_UNKNOWN = 0,
    PQ_PLAT_QEMU_I440FX,
    PQ_PLAT_QEMU_Q35,
    PQ_PLAT_VMWARE_440BX,
    PQ_PLAT_APPLE_INTEL,
    PQ_PLAT_INTEL_MOBILE,
    PQ_PLAT_CLOUDHV,
    PQ_PLAT_GENERIC         /* any host bridge OVMF handles via fallback     */
} pq_platform_t;

typedef enum {
    PQ_PM_NONE = 0,
    PQ_PM_PIIX4,            /* PMBA @0x40, PMREGMISC @0x80 bit0             */
    PQ_PM_ICH,              /* PMBASE @0x40, ACPI_CNTL @0x44 bit7           */
    PQ_PM_UNKNOWN
} pq_pm_style_t;

typedef struct {
    uint32_t        raw;        /* full 32-bit dword read from 0xCFC         */
    uint16_t        vid;        /* low  16 bits                              */
    uint16_t        did;        /* high 16 bits                              */
    pq_platform_t   kind;
    const char     *name;       /* human-readable label only                 */
    /* 1 = this DID has no dedicated OVMF path and is handled by the
     * firmware's generic fallback.  Informational; NOT a request to patch. */
    int             generic_fallback;
    /* Power-management / ACPI timer device, probed from real hardware. */
    uint8_t         pm_bus, pm_dev, pm_fn;
    uint16_t        pm_class;
    pq_pm_style_t   pm_style;
    int             pm_present;
} pq_platform_info_t;

/* Probe bus 0 for an ACPI PM function by PCI class code, any vendor.
 * Two passes: prefer 06:80 (a PIIX4 chipset has BOTH an ISA bridge and a
 * separate PM function; the latter owns PMBA).  Nothing here knows or cares
 * which hypervisor or machine produced the device. */
static inline void pq_probe_pm_device(pq_platform_info_t *p) {
    uint8_t pass, dev, fn;
    uint16_t want;
    p->pm_present = 0;
    p->pm_style   = PQ_PM_UNKNOWN;
    p->pm_bus = p->pm_dev = p->pm_fn = 0;
    p->pm_class = 0;

    for (pass = 0; pass < 2 && !p->pm_present; pass++) {
        want = (pass == 0) ? PQ_PM_CLASS_PIIX4 : PQ_PM_CLASS_ICH;
        for (dev = 0; dev < 32 && !p->pm_present; dev++) {
            for (fn = 0; fn < 8 && !p->pm_present; fn++) {
                uint32_t r = pq_pci_read32(0, dev, fn, 0);
                uint16_t vid = (uint16_t)(r & 0xFFFFu);
                uint16_t cls;
                if (vid == 0xFFFFu || vid == 0x0000u) continue;
                cls = (uint16_t)(pq_pci_read32(0, dev, fn, 8) >> 16);
                if (cls != want) continue;
                p->pm_bus = 0; p->pm_dev = dev; p->pm_fn = fn;
                p->pm_class = cls;
                p->pm_style = (cls == PQ_PM_CLASS_PIIX4)
                              ? PQ_PM_PIIX4 : PQ_PM_ICH;
                p->pm_present = 1;
            }
        }
    }
}

static inline pq_platform_info_t pq_platform_detect(void) {
    pq_platform_info_t p;
    p.raw = pq_pci_read32(0, 0, 0, 0);
    p.vid = (uint16_t)(p.raw & 0xFFFFu);
    p.did = (uint16_t)(p.raw >> 16);
#ifdef PQ_TEST_FORCE_UNKNOWN_DID
    /* Test hook: exercise the generic fallback on an ordinary QEMU machine.
     * Build with -DPQ_TEST_FORCE_UNKNOWN_DID and the loader must still detect
     * the PM device, log "generic fallback" and hand off normally.  Proves
     * no behaviour is keyed to a chipset DID. */
    p.did = 0xDEADu;
#endif
    p.kind = PQ_PLAT_GENERIC;
    p.name = "generic host bridge (OVMF fallback path)";
    /* DIDs with dedicated OVMF code.  Everything else uses the generic
     * fallback -- this is a special-case list, NOT a supported-chipset
     * allowlist. */
    p.generic_fallback = 1;

    switch (p.did) {
    case PQ_DID_I440FX:
        p.kind = PQ_PLAT_QEMU_I440FX;
        p.name = "QEMU i440FX / PIIX4";
        p.generic_fallback = 0;
        break;
    case PQ_DID_Q35_MCH:
    case PQ_DID_Q35_MCH_B0:
        p.kind = PQ_PLAT_QEMU_Q35;
        p.name = "QEMU Q35 / ICH9";
        p.generic_fallback = 0;
        break;
    case PQ_DID_CLOUDHV:
        p.kind = PQ_PLAT_CLOUDHV;
        p.name = "Cloud Hypervisor";
        p.generic_fallback = 0;
        break;
    case PQ_DID_VMWARE_440BX:
        p.kind = PQ_PLAT_VMWARE_440BX;
        p.name = "VMware virtual 440BX";
        break;
    case PQ_DID_INTEL_915G:
    case PQ_DID_INTEL_945G:
    case PQ_DID_INTEL_945GM:
    case PQ_DID_INTEL_945GME:
        p.kind = PQ_PLAT_INTEL_MOBILE;
        p.name = "Intel 915/945 + ICH7 (early Intel Mac / DTK class)";
        break;
    default:
        /* Deliberately no chipset table.  Unknown DIDs are supported. */
        break;
    }
    pq_probe_pm_device(&p);
    return p;
}

/* ------------------------------------------------------------------------- */
/* Legal runtime quirks                                                      */
/* ------------------------------------------------------------------------- */

/* Enable ACPI PM I/O space only if the host firmware has NOT already done so.
 * Reprogramming PMBASE underneath a running firmware would break its SMI/SCI
 * handling, so this is deliberately a no-op when the enable bit is set.
 * Returns 1 if we programmed it, 0 if already enabled / unsupported. */
static inline int pq_enable_pm_io(const pq_platform_info_t *p) {
    uint8_t bus, dev, fn;
    if (!p->pm_present) return 0;
    bus = p->pm_bus; dev = p->pm_dev; fn = p->pm_fn;

    if (p->pm_style == PQ_PM_PIIX4) {
        if (pq_pci_read8(bus, dev, fn, 0x80) & 0x01u) return 0;   /* PMIOSE */
        pq_pci_write32(bus, dev, fn, 0x40, 0xB000u);              /* PMBA */
        pq_pci_write8 (bus, dev, fn, 0x04,
                       (uint8_t)(pq_pci_read8(bus, dev, fn, 0x04) | 0x01u));
        pq_pci_write8 (bus, dev, fn, 0x80,
                       (uint8_t)(pq_pci_read8(bus, dev, fn, 0x80) | 0x01u));
        return 1;
    }
    if (p->pm_style == PQ_PM_ICH) {
        if (pq_pci_read8(bus, dev, fn, 0x44) & 0x80u) return 0;   /* ACPI_EN */
        pq_pci_write32(bus, dev, fn, 0x40, 0x0600u);              /* PMBASE */
        pq_pci_write8 (bus, dev, fn, 0x04,
                       (uint8_t)(pq_pci_read8(bus, dev, fn, 0x04) | 0x01u));
        pq_pci_write8 (bus, dev, fn, 0x44,
                       (uint8_t)(pq_pci_read8(bus, dev, fn, 0x44) | 0x80u));
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CMOS (RTC/NVRAM) access                                                   */
/* ------------------------------------------------------------------------- */

static inline uint8_t pq_cmos_read(uint8_t idx) {
    pq_outb(0x70u, (uint8_t)(idx | 0x80u));   /* bit7 = disable NMI */
    return pq_inb(0x71u);
}
static inline void pq_cmos_write(uint8_t idx, uint8_t v) {
    pq_outb(0x70u, (uint8_t)(idx | 0x80u));
    pq_outb(0x71u, v);
}

/* Publish "memory above 4 GiB" in 64 KiB units -- the encoding EDK2's
 * PlatformGetSystemMemorySizeAbove4gb() expects (CMOS 0x5D/0x5C/0x5B).
 *
 * NO platform allowlist.  First prove 0x5B..0x5D behave as ordinary scratch
 * RAM: write a test pattern, read it back, restore the originals.  Machines
 * where those bytes belong to an EC/SMM fail the readback and are left
 * untouched.  Only then are the real values written, and read back again.
 * Returns 1 on success, 0 if the bytes are not safe to use. */
static inline int pq_cmos_set_highmem(uint64_t above_4g_bytes) {
    uint8_t ob = pq_cmos_read(0x5Bu);
    uint8_t om = pq_cmos_read(0x5Cu);
    uint8_t oh = pq_cmos_read(0x5Du);
    uint64_t units = above_4g_bytes >> 16;
    uint8_t b, m, h;

    pq_cmos_write(0x5Bu, 0xA5u);
    pq_cmos_write(0x5Cu, 0x5Au);
    pq_cmos_write(0x5Du, 0x3Cu);
    if (pq_cmos_read(0x5Bu) != 0xA5u ||
        pq_cmos_read(0x5Cu) != 0x5Au ||
        pq_cmos_read(0x5Du) != 0x3Cu) {
        pq_cmos_write(0x5Bu, ob);
        pq_cmos_write(0x5Cu, om);
        pq_cmos_write(0x5Du, oh);
        return 0;
    }

    if (units > 0xFFFFFFull) units = 0xFFFFFFull;
    b = (uint8_t)(units & 0xFFu);
    m = (uint8_t)((units >> 8) & 0xFFu);
    h = (uint8_t)((units >> 16) & 0xFFu);
    pq_cmos_write(0x5Bu, b);
    pq_cmos_write(0x5Cu, m);
    pq_cmos_write(0x5Du, h);
    return (pq_cmos_read(0x5Bu) == b &&
            pq_cmos_read(0x5Cu) == m &&
            pq_cmos_read(0x5Du) == h);
}

/* ------------------------------------------------------------------------- */
/* CPUID helpers                                                             */
/* ------------------------------------------------------------------------- */

static inline void pq_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                            uint32_t *c, uint32_t *d) {
    uint32_t ea = leaf, eb = 0, ec = 0, ed = 0;
    __asm__ volatile("cpuid"
                     : "+a"(ea), "+b"(eb), "+c"(ec), "+d"(ed));
    *a = ea; *b = eb; *c = ec; *d = ed;
}

static inline int pq_cpu_has_long_mode(void) {
    uint32_t a, b, c, d;
    pq_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u) return 0;
    pq_cpuid(0x80000001u, &a, &b, &c, &d);
    return (int)((d >> 29) & 1u);
}

static inline int pq_cpu_has_1g_pages(void) {
    uint32_t a, b, c, d;
    pq_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u) return 0;
    pq_cpuid(0x80000001u, &a, &b, &c, &d);
    return (int)((d >> 26) & 1u);
}

static inline uint32_t pq_cpu_phys_bits(void) {
    uint32_t a, b, c, d, bits;
    pq_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000008u) return 36;
    pq_cpuid(0x80000008u, &a, &b, &c, &d);
    bits = a & 0xFFu;
    if (bits == 0 || bits > 48) return 36;
    return bits;
}

#endif /* PLATFORM_QUIRKS_H */
