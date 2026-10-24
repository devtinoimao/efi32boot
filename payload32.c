/*
 * payload32.c -- IA-32 (i386) Windows PE test payload.
 *
 * This is the "32 bit Windows PE" side of the test: it is linked into a real
 * PE32 image (i686-w64-mingw32-gcc -nostdlib -Wl,--subsystem,10), loaded by
 * the 64 bit bootloader as a PE32 image and entered with a far jump after
 * the CPU has been moved into 32 bit protected mode with paging disabled.
 *
 * It proves that
 *   - the CPU really executes 32 bit code,
 *   - the flat 4 GiB segments address physical memory directly,
 *   - the handover descriptor is intact,
 *   - the serial port is still reachable after the mode switch.
 */

typedef unsigned int   UINT32;
typedef unsigned short UINT16;
typedef unsigned char  UINT8;

#define COM1 0x3F8

typedef struct {
    UINT32 Magic;        /* 'B32P'                                    */
    UINT32 ImageBase;
    UINT32 EntryPoint;
    UINT32 ImageSize;
    UINT32 StackTop;
    UINT32 GdtSlot;
    UINT32 Trampoline;
    UINT32 Flags;
} BOOT32_HANDOVER;

#define BOOT32_MAGIC 0x50323342u

static void SerInit(void)
{
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x00), "d"((UINT16)(COM1 + 1)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x80), "d"((UINT16)(COM1 + 3)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x01), "d"((UINT16)(COM1 + 0)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x00), "d"((UINT16)(COM1 + 1)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x03), "d"((UINT16)(COM1 + 3)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0xC7), "d"((UINT16)(COM1 + 2)));
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x0B), "d"((UINT16)(COM1 + 4)));
}

static void SerPutc(char c)
{
    UINT8 status;
    do {
        __asm__ __volatile__ ("inb %%dx, %%al" : "=a"(status) : "d"((UINT16)(COM1 + 5)));
    } while ((status & 0x20) == 0);
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)c), "d"((UINT16)COM1));
}

static void SerPuts(const char *s)
{
    while (*s) {
        if (*s == '\n') SerPutc('\r');
        SerPutc(*s++);
    }
}

static void SerHex32(UINT32 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = 0;
    SerPuts(buf);
}

void payload_main(BOOT32_HANDOVER *handover, UINT32 size, UINT32 entry, UINT32 base)
{
    SerInit();

    SerPuts("\n[32 ] IA-32 payload is running in 32 bit protected mode\n");

    SerPuts("[32 ] handover magic    0x");
    SerHex32(handover->Magic);
    SerPuts(handover->Magic == BOOT32_MAGIC ? " (OK)\n" : " (BAD)\n");

    SerPuts("[32 ] image base        0x");
    SerHex32(base);
    SerPuts("\n");

    SerPuts("[32 ] entry point       0x");
    SerHex32(entry);
    SerPuts("\n");

    SerPuts("[32 ] image size        0x");
    SerHex32(size);
    SerPuts("\n");

    SerPuts("[32 ] 32 bit stack top  0x");
    SerHex32(handover->StackTop);
    SerPuts("\n");

    SerPuts("[32 ] 32 bit GDT slot   0x");
    SerHex32(handover->GdtSlot);
    SerPuts("\n");

    /*
     * Prove that the flat 4 GiB data segment really maps physical memory:
     * write a signature at the handover page and read it back.
     */
    {
        volatile UINT32 *probe = (volatile UINT32 *)(handover->GdtSlot + 0x0C0);
        *probe = 0xDEADBEEFu;
        SerPuts("[32 ] memory probe      0x");
        SerHex32(*probe);
        SerPuts(*probe == 0xDEADBEEFu ? " (OK)\n" : " (BAD)\n");
    }

    SerPuts("[32 ] handing control back to the firmware is not possible\n");
    SerPuts("[32 ] IA-32 payload finished\n");
}
