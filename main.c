/*
 * main.c -- 64-bit (x86_64) UEFI bootloader whose sole purpose is to move the
 *            CPU out of 64-bit Long Mode, into 32-bit Protected Mode with
 *            paging disabled, and hand control to a 32-bit Windows PE
 *            (WinPE x86) loader/kernel image.
 *
 * Execution flow
 * --------------
 *   1.  UefiInit()            - minimal gnu-efi style init (system table cache,
 *                               ConOut text output, COM1 serial trace).
 *   2.  DumpMemoryMap()       - SystemTable->BootServices->GetMemoryMap() to
 *                               learn the firmware memory topology.
 *   3.  LoadPayload()         - read "\sources\boot.wim" from the ESP through
 *                               the SimpleFileSystem protocol and map it as a
 *                               PE32 image (sections + base relocations) into
 *                               physical memory below the 4 GiB ceiling.
 *   4.  ReserveHandover()     - low (<4 GiB) page for the 32-bit GDT, the
 *                               handover descriptor and a copy of the
 *                               compatibility-mode trampoline, plus a low
 *                               32-bit stack.
 *   5.  ExitBootServices()    - terminate the UEFI execution context with a
 *                               freshly obtained memory map key.
 *   6.  TransitionTo32BitAndJump(entry) - transition.asm: lgdt(32-bit GDT),
 *                               far return into compatibility mode, CLI/CR0.PG
 *                               teardown, far jump to the PE entry point.
 *
 * Freestanding: no CRT, no libc, no gnu-efi runtime.  The only external
 * symbols are the three NASM routines in transition.asm.
 *
 * Build (see Makefile):
 *   x86_64-w64-mingw32-gcc -nostdlib -fno-builtin -Wl,--subsystem,10 \
 *       -Wl,--entry,efi_main main.c transition.o -o BOOTX64.EFI
 */

#include <stdarg.h> /* freestanding, provided by the compiler itself        */

#include "ovmf32_constants.h" /* generated from the embedded 32 bit firmware  */

/* ------------------------------------------------------------------------- */
/*  UEFI scalar types (UEFI 2.10 / PI 1.8, x86_64 flavour)                  */
/* ------------------------------------------------------------------------- */

typedef unsigned char      UINT8;
typedef signed char        INT8;
typedef unsigned short     UINT16;
typedef signed short       INT16;
typedef unsigned int       UINT32;
typedef signed int         INT32;
typedef unsigned long long UINT64;
typedef signed long long   INT64;
typedef char               CHAR8;
typedef unsigned short     CHAR16;
typedef unsigned char      BOOLEAN;
typedef void               VOID;

typedef UINT64             UINTN;
typedef INT64              INTN;
typedef UINT32             EFI_MEMORY_TYPE;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef UINT64             EFI_VIRTUAL_ADDRESS;
typedef VOID              *EFI_HANDLE;
typedef UINTN              EFI_TPL;
typedef UINTN              EFI_STATUS;
typedef UINT64             EFI_LBA;

#define EFI_SUCCESS            ((EFI_STATUS)0)
#define EFI_LOAD_ERROR         ((EFI_STATUS)1)
#define EFI_INVALID_PARAMETER  ((EFI_STATUS)2)
#define EFI_UNSUPPORTED        ((EFI_STATUS)3)
#define EFI_BUFFER_TOO_SMALL   ((EFI_STATUS)5)
#define EFI_DEVICE_ERROR       ((EFI_STATUS)7)
#define EFI_WRITE_PROTECTED    ((EFI_STATUS)8)
#define EFI_OUT_OF_RESOURCES   ((EFI_STATUS)9)
#define EFI_NOT_FOUND          ((EFI_STATUS)10)
#define EFI_ACCESS_DENIED      ((EFI_STATUS)15)
#define EFI_TIMEOUT            ((EFI_STATUS)18)
#define EFI_ABORTED            ((EFI_STATUS)22)
#define EFI_SECURITY_VIOLATION ((EFI_STATUS)26)
#define EFI_END_OF_FILE        ((EFI_STATUS)31)

#define TRUE   ((BOOLEAN)1)
#define FALSE  ((BOOLEAN)0)
#define NULL   ((VOID *)0)

#define EFIAPI __attribute__((ms_abi))

/* Memory types (PI spec) */
#define EfiReservedMemoryType       0
#define EfiLoaderCode                1
#define EfiLoaderData                2
#define EfiBootServicesCode          3
#define EfiBootServicesData          4
#define EfiRuntimeServicesCode       5
#define EfiRuntimeServicesData       6
#define EfiConventionalMemory        7
#define EfiUnusableMemory            8
#define EfiACPIReclaimMemory         9
#define EfiACPIMemoryNVS             10
#define EfiMemoryMappedIO            11
#define EfiMemoryMappedIOPortSpace   12
#define EfiPalCode                   13
#define EfiPersistentMemory          14
#define EfiMaxMemoryType             15

/* AllocateTypes */
#define AllocateAnyPages       0
#define AllocateMaxAddress     1
#define AllocateAddress        2

#define TPL_APPLICATION  4
#define TPL_HIGH_LEVEL   31

#define EFI_PAGE_SIZE          4096u
#define EFI_SIZE_TO_PAGES(s)   (((UINTN)(s) + EFI_PAGE_SIZE - 1u) / EFI_PAGE_SIZE)

/* ------------------------------------------------------------------------- */
/*  UEFI structures                                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct _EFI_BOOT_SERVICES   EFI_BOOT_SERVICES;
typedef struct _EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL   EFI_FILE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef VOID (EFIAPI *EFI_FN_UNUSED)(VOID);

typedef EFI_STATUS (EFIAPI *EFI_RAISE_TPL)(EFI_TPL NewTpl);
typedef EFI_STATUS (EFIAPI *EFI_RESTORE_TPL)(EFI_TPL OldTpl);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(UINT8 AllocateType, UINT8 MemoryType,
                                                UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                                 UINTN *MapKey, UINTN *DescriptorSize, UINTN *ActualSize);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(UINT8 PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol, VOID *Registration,
                                                 VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_CALCULATE_CRC32)(VOID *Data, UINTN DataSize, UINT32 *Crc32);

struct _EFI_BOOT_SERVICES {
    /*
     * Every service table starts with an EFI_TABLE_HEADER.  It carries the
     * 'BOOTSERV' / 'RUNT SERV' signature and is 24 bytes wide, so all the
     * service pointers start at offset 0x18.
     */
    EFI_TABLE_HEADER           Hdr;
    EFI_RAISE_TPL              RaiseTPL;
    EFI_RESTORE_TPL            RestoreTPL;
    EFI_ALLOCATE_PAGES         AllocatePages;
    EFI_FREE_PAGES             FreePages;
    EFI_GET_MEMORY_MAP         GetMemoryMap;
    EFI_ALLOCATE_POOL          AllocatePool;
    EFI_FREE_POOL              FreePool;
    EFI_FN_UNUSED              CreateEvent;
    EFI_FN_UNUSED              SetTimer;
    EFI_FN_UNUSED              WaitForEvent;
    EFI_FN_UNUSED              SignalEvent;
    EFI_FN_UNUSED              CloseEvent;
    EFI_FN_UNUSED              CheckEvent;
    EFI_FN_UNUSED              InstallProtocolInterface;
    EFI_FN_UNUSED              ReinstallProtocolInterface;
    EFI_FN_UNUSED              UninstallProtocolInterface;
    EFI_FN_UNUSED              HandleProtocol;
    /*
     * The "Reserved" slot of the UEFI 2.8 specification.  The current
     * EDK2 headers no longer declare it, but the firmware still keeps the
     * slot, so every service from RegisterProtocolNotify on is shifted by
     * one entry when it is left out.
     */
    EFI_FN_UNUSED              Reserved;
    EFI_FN_UNUSED              RegisterProtocolNotify;
    EFI_FN_UNUSED              LocateHandle;
    EFI_FN_UNUSED              LocateDevicePath;
    EFI_FN_UNUSED              InstallConfigurationTable;
    EFI_FN_UNUSED              LoadImage;
    EFI_FN_UNUSED              StartImage;
    EFI_FN_UNUSED              Exit;
    EFI_FN_UNUSED              UnloadImage;
    EFI_EXIT_BOOT_SERVICES     ExitBootServices;
    EFI_FN_UNUSED              GetNextMonotonicCount;
    EFI_STALL                  Stall;
    EFI_FN_UNUSED              SetWatchdogTimer;
    EFI_FN_UNUSED              ConnectController;
    EFI_FN_UNUSED              DisconnectController;
    EFI_FN_UNUSED              OpenProtocol;
    EFI_FN_UNUSED              CloseProtocol;
    EFI_FN_UNUSED              OpenProtocolInformation;
    EFI_FN_UNUSED              ProtocolsPerHandle;
    EFI_FN_UNUSED              LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL        LocateProtocol;
    EFI_FN_UNUSED              InstallMultipleProtocolInterfaces;
    EFI_FN_UNUSED              UninstallMultipleProtocolInterfaces;
    EFI_CALCULATE_CRC32        CalculateCrc32;
    EFI_FN_UNUSED              CopyMem;
    EFI_FN_UNUSED              SetMem;
    EFI_FN_UNUSED              CreateEventEx;
};

typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                               UINT32 *Attributes, UINTN *DataSize, VOID *Data);
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                               UINT32 Attributes, UINTN DataSize, VOID *Data);

struct _EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER   Hdr;                 /* 'RUNT SERV', 24 bytes  */
    EFI_FN_UNUSED      GetTime;
    EFI_FN_UNUSED      SetTime;
    EFI_FN_UNUSED      GetWakeupTime;
    EFI_FN_UNUSED      SetWakeupTime;
    EFI_FN_UNUSED      SetVirtualAddressMap;
    EFI_FN_UNUSED      ConvertPointer;
    EFI_GET_VARIABLE   GetVariable;
    EFI_FN_UNUSED      GetNextVariableName;
    EFI_SET_VARIABLE   SetVariable;
    EFI_FN_UNUSED      GetNextHighMonotonicCount;
    EFI_FN_UNUSED      ResetSystem;
    EFI_FN_UNUSED      UpdateCapsule;
    EFI_FN_UNUSED      QueryCapsuleCapabilities;
    EFI_FN_UNUSED      QueryVariableInfo;
};

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
                                                  UINTN *Columns, UINTN *Rows);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                                           UINTN Column, UINTN Row);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);

/*
 * Layout taken verbatim from
 *   MdePkg/Include/Protocol/SimpleTextOut.h
 * of the EDK2 tree.  The four EFI_TEXT_COLOR attributes that older
 * descriptions of the protocol mention are *not* part of the structure.
 */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET               Reset;
    EFI_TEXT_STRING              OutputString;
    EFI_TEXT_TEST_STRING         TestString;
    EFI_TEXT_QUERY_MODE          QueryMode;
    EFI_TEXT_SET_MODE            SetMode;
    EFI_TEXT_SET_ATTRIBUTE       SetAttribute;
    EFI_TEXT_CLEAR_SCREEN        ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR       EnableCursor;
    VOID                        *Mode;
};

/*
 * The EFI_SYSTEM_TABLE layout changed with UEFI 2.7: FirmwareVendor,
 * FirmwareRevision, ConsoleInHandle and ConsoleOutHandle were added, which
 * moves ConOut from 0x20 to 0x40, RuntimeServices from 0x30 to 0x58 and
 * BootServices from 0x38 to 0x60.  The table pointers are therefore probed
 * at run time with the help of the signature that every service table
 * carries in its EFI_TABLE_HEADER, so that both the old and the new layout
 * work.
 */
#define EFI_BOOT_SERVICES_SIGNATURE    0x56524553544F4F42ULL /* 'BOOTSERV' */
#define EFI_RUNTIME_SERVICES_SIGNATURE 0x5652455354554E52ULL /* 'RUNT SERV' */

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16          *FirmwareVendor;        /* 0x18 */
    UINT32           FirmwareRevision;       /* 0x20 */
    EFI_HANDLE       ConsoleInHandle;        /* 0x28 */
    VOID            *ConIn;                  /* 0x30 */
    EFI_HANDLE       ConsoleOutHandle;       /* 0x38 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut; /* 0x40 */
    EFI_HANDLE       StandardErrorHandle;   /* 0x48 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr; /* 0x50 */
    EFI_RUNTIME_SERVICES *RuntimeServices;   /* 0x58 */
    EFI_BOOT_SERVICES   *BootServices;       /* 0x60 */
    UINTN            NumberOfTableEntries;   /* 0x68 */
    VOID            *ConfigurationTable;     /* 0x70 */
} EFI_SYSTEM_TABLE;

/* --- Simple file system / file protocols ------------------------------- */

#define EFI_FILE_MODE_READ  0x0000000000000001
#define EFI_FILE_MODE_WRITE 0x0000000000000002

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                                           CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                               UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_INFO)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                               UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(EFI_FILE_PROTOCOL *This);

struct _EFI_FILE_PROTOCOL {
    UINT64            Revision;
    EFI_FILE_OPEN     Open;
    EFI_FILE_CLOSE    Close;
    EFI_FILE_DELETE   Delete;
    EFI_FILE_READ     Read;
    EFI_FILE_WRITE    Write;
    EFI_FN_UNUSED     GetPosition;
    EFI_FN_UNUSED     SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    EFI_FILE_SET_INFO SetInfo;
    EFI_FN_UNUSED     Flush;
};

typedef EFI_STATUS (EFIAPI *EFI_SFS_OPEN_VOLUME)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                                 EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64                 Revision;
    EFI_SFS_OPEN_VOLUME    OpenVolume;
};

#pragma pack(push, 1)
typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;
#pragma pack(pop)

/* ------------------------------------------------------------------------- */
/*  Well known GUIDs                                                         */
/* ------------------------------------------------------------------------- */

static const EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
static const EFI_GUID gEfiFileInfoGuid = {
    0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

/* ------------------------------------------------------------------------- */
/*  transition.asm interface                                                 */
/* ------------------------------------------------------------------------- */

/*
 * The transition routine is a one way hand-off: it never returns.  RCX holds
 * the 32-bit physical entry point of the PE image.
 */
extern VOID EFIAPI TransitionTo32BitAndJump(UINT64 HandoverPage, UINT64 EntryPoint32,
                                            UINT64 BootArg32, UINT64 StackTop32);

/*
 * Hand-off variables, written by main.c and consumed by transition.asm.
 * All of them point into memory below the 4 GiB ceiling so that they stay
 * reachable once paging is switched off and the CPU runs in protected mode.
 */
extern UINT64 TransitionStackTop32;          /* 32-bit stack top (phys)      */
extern UINT64 TransitionGdtSlot32;            /* 64-byte low GDT slot        */
extern UINT64 TransitionCompatTrampoline32;   /* low-memory trampoline copy  */
extern UINT64 TransitionHandover32;          /* handover descriptor address */
extern UINT64 TransitionBootArg32;           /* EBP of the 32-bit entry    */

/*
 * The hosted 32 bit firmware.  It is linked into this application, so that
 * a single BOOTX64.EFI carries the complete 32 bit environment - the same
 * way a CSM module carries the legacy BIOS it emulates.
 */
extern const UINT8 Ovmf32Image[];
extern const UINT8 Ovmf32ImageEnd[];

/*
 * The compatibility-mode trampoline has to be executed *after* paging has been
 * disabled, therefore it must live below 4 GiB as well.  main.c copies the
 * routine out of the PE image into the low handover page and publishes the
 * new address through TransitionCompatTrampoline32.
 */
extern const UINT8 TransitionCompatTrampoline[];
extern const UINT8 TransitionCompatTrampolineEnd[];

/* ------------------------------------------------------------------------- */
/*  Freestanding runtime helpers                                             */
/* ------------------------------------------------------------------------- */

static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_RUNTIME_SERVICES *gRT;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *gConOut;

/*
 * Locate a service table inside the system table.  UEFI 2.0 kept the boot
 * services at offset 0x38, UEFI 2.7 and later at 0x40.  Both are probed and
 * the one whose EFI_TABLE_HEADER carries the expected signature wins.
 */
static VOID *FindServiceTable(EFI_SYSTEM_TABLE *ST, UINT64 Signature, UINTN OffsetNew, UINTN OffsetOld)
{
    UINT8 *base = (UINT8 *)ST;
    VOID *candidate = *(VOID **)(base + OffsetNew);

    if (candidate != NULL && *(UINT64 *)candidate == Signature) return candidate;

    candidate = *(VOID **)(base + OffsetOld);
    if (candidate != NULL && *(UINT64 *)candidate == Signature) return candidate;

    return NULL;
}

/*
 * Locate the console output protocol.  The UEFI 2.7 layout keeps it at
 * offset 0x40, the classic layout at 0x20.  A candidate is accepted when it
 * points at a real interface, that means when both the interface itself and
 * its OutputString() service live in the firmware's memory.
 */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *FindConOut(EFI_SYSTEM_TABLE *ST)
{
    UINT8 *base = (UINT8 *)ST;
    UINTN i;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *candidates[2];

    candidates[0] = *(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL **)(base + 0x40);
    candidates[1] = *(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL **)(base + 0x20);

    for (i = 0; i < 2; i++) {
        if (candidates[i] == NULL) continue;
        if ((UINTN)candidates[i] < 0x100000ULL) continue;
        if (candidates[i]->OutputString == NULL) continue;
        if ((UINTN)candidates[i]->OutputString < 0x100000ULL) continue;
        return candidates[i];
    }
    return NULL;
}

void *memcpy(void *dst, const void *src, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    while (n--) *d++ = (UINT8)c;
    return dst;
}

void *memmove(void *dst, const void *src, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    if (d == s || n == 0) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

/* ------------------------------------------------------------------------- */
/*  COM1 serial trace (the '-nographic' console of QEMU)                     */
/* ------------------------------------------------------------------------- */

#define COM1 0x3F8

static void SerInit(void)
{
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x00), "d"((UINT16)(COM1 + 1))); /* interrupts */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x80), "d"((UINT16)(COM1 + 3))); /* DLAB */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x01), "d"((UINT16)(COM1 + 0))); /* divisor lo */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x00), "d"((UINT16)(COM1 + 1))); /* divisor hi */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x03), "d"((UINT16)(COM1 + 3))); /* 8N1 */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0xC7), "d"((UINT16)(COM1 + 2))); /* FIFO */
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)0x0B), "d"((UINT16)(COM1 + 4))); /* RTS/OUT2 */
}

static void SerPutc(char c)
{
    UINT8 status;
    do {
        __asm__ __volatile__ ("inb %%dx, %%al" : "=a"(status) : "d"((UINT16)(COM1 + 5)));
    } while ((status & 0x20) == 0);
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"((UINT8)c), "d"((UINT16)COM1));
}

static void SerPuts(const CHAR8 *s)
{
    while (*s) {
        if (*s == '\n') SerPutc('\r');
        SerPutc(*s++);
    }
}

/*
 * Raw single character marker on COM1.  It is used to bisect the start-up
 * path when hunting for a faulting instruction: every marker that is echoed
 * back proves that the code up to that point completed.
 */
static void Mark(UINT8 c)
{
    __asm__ __volatile__ ("outb %%al, %%dx" :: "a"(c), "d"((UINT16)COM1));
}

static void SerHex64(UINT64 v)
{
    static const CHAR8 digits[] = "0123456789abcdef";
    int i;
    for (i = 60; i >= 0; i -= 4) SerPutc(digits[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------------- */
/*  Formatted output to the firmware display and to the serial trace         */
/* ------------------------------------------------------------------------- */

static CHAR16 gConBuf[1024];

/*
 * After ExitBootServices the firmware console is no longer functional: the
 * DXE dispatcher that drives the terminal drivers has been shut down.  From
 * that point on only the serial port is used, which is independent of the
 * firmware services.
 */
static BOOLEAN gConsoleDead;

static void ConPuts(const CHAR8 *s)
{
    UINTN i = 0;
    if (gConsoleDead || gConOut == NULL) return;
    while (s[i] != '\0' && i < (sizeof(gConBuf) / sizeof(gConBuf[0])) - 1) {
        gConBuf[i] = (CHAR16)(UINT8)s[i];
        i++;
    }
    gConBuf[i] = 0;
    gConOut->OutputString(gConOut, gConBuf);
}

static CHAR8 *AppendStr(CHAR8 *b, UINTN cap, UINTN *pos, const CHAR8 *s)
{
    while (*s && *pos < cap - 1) {
        b[(*pos)++] = *s++;
    }
    b[*pos] = '\0';
    return b;
}

static CHAR8 *AppendNum(CHAR8 *b, UINTN cap, UINTN *pos, UINT64 v, UINT32 base, int upper,
                        UINTN width, int zeropad)
{
    static const CHAR8 lower[] = "0123456789abcdef";
    static const CHAR8 upperT[] = "0123456789ABCDEF";
    const CHAR8 *digits = upper ? upperT : lower;
    CHAR8 tmp[24];
    UINTN n = 0, len;

    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    len = n;

    /*
     * The digits are collected in reverse order, therefore the padding is
     * appended at the high end of the temporary buffer - it is written out
     * first and ends up in front of the number.
     */
    while (zeropad && len < width && n < sizeof(tmp) - 1) {
        tmp[n++] = '0';
        len++;
    }

    while (n) {
        if (*pos < cap - 1) b[(*pos)++] = tmp[--n];
        else break;
    }
    b[*pos] = '\0';
    return b;
}

static CHAR8 *FormatToBuf(CHAR8 *buf, UINTN cap, const CHAR8 *fmt, va_list ap)
{
    UINTN pos = 0;
    buf[0] = '\0';

    while (*fmt && pos < cap - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            buf[pos] = '\0';
            continue;
        }
        fmt++;
        if (*fmt == '%') { buf[pos++] = '%'; fmt++; buf[pos] = '\0'; continue; }

        {
            UINTN width = 0;
            int zeropad = 0;

            while (*fmt == 'l') { fmt++; }
            if (*fmt == '0') { zeropad = 1; fmt++; }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (UINTN)(*fmt - '0');
                fmt++;
            }

            switch (*fmt) {
            case 's': {
                const CHAR8 *s = va_arg(ap, const CHAR8 *);
                if (s == NULL) s = "(null)";
                AppendStr(buf, cap, &pos, s);
                break;
            }
            case 'd': {
                /* the whole register slot is eight bytes wide */
                INT64 v = va_arg(ap, INT64);
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                AppendNum(buf, cap, &pos, (UINT64)v, 10, 0, width, zeropad);
                break;
            }
            case 'u': {
                UINT64 v = va_arg(ap, UINT64);
                AppendNum(buf, cap, &pos, v, 10, 0, width, zeropad);
                break;
            }
            case 'x':
            case 'X': {
                UINT64 v = va_arg(ap, UINT64);
                AppendNum(buf, cap, &pos, v, 16, (*fmt == 'X'), width, zeropad);
                break;
            }
            case 'p': {
                UINT64 v = (UINT64)(UINTN)va_arg(ap, VOID *);
                AppendStr(buf, cap, &pos, "0x");
                AppendNum(buf, cap, &pos, v, 16, 0, width, zeropad);
                break;
            }
            default:
                buf[pos++] = '%';
                if (*fmt) buf[pos++] = *fmt;
                break;
            }
            if (*fmt) fmt++;
        }
    }
    return buf;
}

static void Print(const CHAR8 *fmt, ...)
{
    CHAR8 buf[1024];
    va_list ap;

    Mark('S');
    va_start(ap, fmt);
    FormatToBuf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Mark('s');

    SerPuts(buf);
    Mark('e');

    ConPuts(buf);
    Mark('c');
}

/* ------------------------------------------------------------------------- */
/*  UEFI library style init                                                  */
/* ------------------------------------------------------------------------- */

static void UefiInit(EFI_SYSTEM_TABLE *SystemTable)
{
    gST = SystemTable;
    Mark('1');
    SerInit();
    Mark('2');

    Print("\r\n[UEFI] x86_64 bootloader initialising\r\n");
    Mark('3');

    gBS = (EFI_BOOT_SERVICES *)FindServiceTable(SystemTable, EFI_BOOT_SERVICES_SIGNATURE, 0x60, 0x38);
    gRT = (EFI_RUNTIME_SERVICES *)FindServiceTable(SystemTable, EFI_RUNTIME_SERVICES_SIGNATURE, 0x58, 0x30);
    gConOut = FindConOut(SystemTable);
    Mark('4');

    SerPuts("[DBG] SystemTable  = 0x");
    SerHex64((UINT64)(UINTN)SystemTable);
    SerPuts("\r\n[DBG] ConOut       = 0x");
    SerHex64((UINT64)(UINTN)SystemTable->ConOut);
    SerPuts("\r\n[DBG] at 0x40      = 0x");
    SerHex64((UINT64)(UINTN)*(VOID **)((UINT8 *)SystemTable + 0x40));
    SerPuts("\r\n[DBG] at 0x38      = 0x");
    SerHex64((UINT64)(UINTN)*(VOID **)((UINT8 *)SystemTable + 0x38));
    SerPuts("\r\n[DBG] BootServices = 0x");
    SerHex64((UINT64)(UINTN)gBS);
    SerPuts("\r\n");

    if (gBS == NULL) {
        Print("[UEFI] FATAL: no BootServices table found\r\n");
        return;
    }
    Print("[UEFI] BootServices  0x%016x\r\n", (UINT64)(UINTN)gBS);
    Print("[UEFI] RuntimeServices 0x%016x\r\n", (UINT64)(UINTN)gRT);
    Mark('5');
}

/* ------------------------------------------------------------------------- */
/*  Memory map                                                               */
/* ------------------------------------------------------------------------- */

static const CHAR8 *MemTypeName(UINT32 t)
{
    switch (t) {
    case EfiReservedMemoryType:     return "Reserved";
    case EfiLoaderCode:             return "LoaderCode";
    case EfiLoaderData:             return "LoaderData";
    case EfiBootServicesCode:       return "BootSvcCode";
    case EfiBootServicesData:       return "BootSvcData";
    case EfiRuntimeServicesCode:    return "RTSvcCode";
    case EfiRuntimeServicesData:    return "RTSvcData";
    case EfiConventionalMemory:     return "Conventional";
    case EfiUnusableMemory:         return "Unusable";
    case EfiACPIReclaimMemory:      return "ACPIReclaim";
    case EfiACPIMemoryNVS:          return "ACPIMemoryNVS";
    case EfiMemoryMappedIO:         return "MMIO";
    case EfiMemoryMappedIOPortSpace:return "MMIO Port";
    case EfiPalCode:                return "PAL";
    case EfiPersistentMemory:       return "Persistent";
    default:                        return "Other";
    }
}

/*
 * Two pass GetMemoryMap(): the first call sizes the buffer, the second one
 * fills it.  The map key returned by the second call is the one that has to
 * be handed to ExitBootServices().
 */
static EFI_STATUS FetchMemoryMap(EFI_MEMORY_DESCRIPTOR **OutMap, UINTN *OutMapSize,
                                  UINTN *OutMapKey, UINTN *OutDescSize)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_STATUS st;
    UINTN mapSize = 32 * 1024;      /* usually enough for the whole map */
    UINTN descSize = 0, key = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;

    /*
     * A single call with a buffer that is large enough for the complete
     * memory map.  If the firmware reports that the buffer was too small it
     * also returns the size it needs, so the call is simply repeated with a
     * buffer of that size.
     */
    SerPuts("[DBG] AllocatePool @ 0x");
    SerHex64((UINT64)(UINTN)BS->AllocatePool);
    SerPuts(" size ");
    SerHex64((UINT64)mapSize);
    SerPuts("\r\n");
    st = BS->AllocatePool(EfiBootServicesData, mapSize, (VOID **)&map);
    SerPuts("[DBG] AllocatePool status ");
    SerHex64((UINT64)(UINT32)st);
    SerPuts(" map ");
    SerHex64((UINT64)(UINTN)map);
    SerPuts("\r\n");
    if (st != EFI_SUCCESS || map == NULL) return EFI_OUT_OF_RESOURCES;

    Mark('a');
    st = BS->GetMemoryMap(&mapSize, map, &key, &descSize, NULL);
    Mark('b');
    SerPuts("[DBG] GetMemoryMap status ");
    SerHex64((UINT64)(UINT32)st);
    SerPuts(" size ");
    SerHex64((UINT64)mapSize);
    SerPuts(" desc ");
    SerHex64((UINT64)descSize);
    SerPuts("\r\n");

    if (st == EFI_BUFFER_TOO_SMALL) {
        BS->FreePool(map);
        mapSize += descSize;
        map = (EFI_MEMORY_DESCRIPTOR *)BS->AllocatePool(EfiBootServicesData, mapSize, (VOID **)&map);
        SerPuts("[DBG] retry AllocatePool ");
        SerHex64((UINT64)(UINTN)map);
        SerPuts("\r\n");
        if (map == NULL) return EFI_OUT_OF_RESOURCES;
        st = BS->GetMemoryMap(&mapSize, map, &key, &descSize, NULL);
        SerPuts("[DBG] retry GetMemoryMap status ");
        SerHex64((UINT64)(UINT32)st);
        SerPuts("\r\n");
    }

    if (st != EFI_SUCCESS) {
        BS->FreePool(map);
        return st;
    }

    *OutMap = map;
    *OutMapSize = mapSize;
    *OutMapKey = key;
    *OutDescSize = descSize;
    return EFI_SUCCESS;
}

static void DumpMemoryMap(EFI_MEMORY_DESCRIPTOR *map, UINTN mapSize, UINTN descSize)
{
    UINTN i, count = mapSize / descSize;
    UINT64 conventional = 0, largest = 0, total = 0;

    for (i = 0; i < count; i++) {
        /*
         * The stride between two descriptors is *DescriptorSize and not
         * sizeof(EFI_MEMORY_DESCRIPTOR): the firmware pads the structure to
         * a multiple of eight bytes.
         */
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * descSize);
        UINT64 bytes = d->NumberOfPages * EFI_PAGE_SIZE;
        total += bytes;
        if (d->Type == EfiConventionalMemory) {
            conventional += bytes;
            if (bytes > largest) largest = bytes;
        }
    }

    SerPuts("[DBG] fmt test: ");
    Print("%016x\r\n", (UINT64)0x380000000000ULL);
    SerPuts("[DBG] raw total: 0x");
    SerHex64(total);
    SerPuts("  mib: 0x");
    SerHex64((UINT64)(total >> 20));
    SerPuts("  desc1 phys: 0x");
    SerHex64(*(UINT64 *)((UINT8 *)map + descSize + 8));
    SerPuts("\r\n");
    Print("[MEM] %u descriptors, %u bytes/descriptor\r\n", (UINT64)count, (UINT64)descSize);
    Print("[MEM] total described 0x%016x (%u MiB)\r\n", total, (UINT64)(total >> 20));
    Print("[MEM] conventional    0x%016x (%u MiB)\r\n", conventional, (UINT64)(conventional >> 20));
    Print("[MEM] largest free    0x%016x (%u MiB)\r\n", largest, (UINT64)(largest >> 20));

    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * descSize);
        Print("[MEM]  %s phys 0x%016x pages 0x%08x attr 0x%016x\r\n",
              MemTypeName(d->Type), d->PhysicalStart,
              (UINT64)d->NumberOfPages, d->Attribute);
    }
    if (count > 12) Print("[MEM]  ... %u more descriptors\r\n", (UINT32)(count - 12));
}

/* ------------------------------------------------------------------------- */
/*  ExitBootServices with the freshly generated map key                     */
/* ------------------------------------------------------------------------- */

static EFI_STATUS ExitBootServices(EFI_HANDLE ImageHandle)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_STATUS st;
    UINT32 attempt;

    /*
     * The map key has to be the current one.  It is only handed out by a
     * GetMemoryMap() call that actually fills the map, and nothing may
     * allocate or free memory between reading it and handing it over.
     */
    for (attempt = 0; attempt < 8; attempt++) {
        UINTN mapSize = 0, key = 0, descSize = 0;
        EFI_MEMORY_DESCRIPTOR *map = NULL;

        /* the buffer first, so that the key read afterwards is the current one */
        mapSize = 64 * 1024;
        st = BS->AllocatePool(EfiBootServicesData, mapSize, (VOID **)&map);
        if (st != EFI_SUCCESS || map == NULL) {
            Print("[UEFI] AllocatePool failed, status %u\\r\\n", (UINT32)st);
            return EFI_OUT_OF_RESOURCES;
        }

        st = BS->GetMemoryMap(&mapSize, map, &key, &descSize, NULL);
        if (st != EFI_SUCCESS) {
            BS->FreePool(map);
            return st;
        }

        st = BS->ExitBootServices(ImageHandle, key);
        if (st == EFI_SUCCESS) {
            Print("[UEFI] ExitBootServices succeeded (attempt %u)\r\n", attempt + 1);
            return EFI_SUCCESS;
        }
        Print("[UEFI] ExitBootServices rejected key 0x%016x (status %u), retrying\r\n",
              key, (UINT32)st);
        BS->FreePool(map);
    }
    return EFI_ABORTED;
}

/* ------------------------------------------------------------------------- */
/*  ESP file access                                                          */
/* ------------------------------------------------------------------------- */


static const CHAR16 *const kPayloadCandidates[] = {
    L"\\sources\\boot.wim",
    L"\\sources\\winpe32.efi",
    L"\\sources\\bootx32.bin",
    L"\\boot.wim",
    NULL
};

static UINTN StrLenW(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * Reads one file from the EFI system partition through the SimpleFileSystem
 * protocol.  The returned buffer comes from the boot services pool and stays
 * valid until ExitBootServices() has been called.
 */
static EFI_STATUS ReadEspFile(EFI_FILE_PROTOCOL *Root, const CHAR16 *Path,
                              UINT8 **OutData, UINT64 *OutSize)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_FILE_INFO *Info = NULL;
    UINTN infoSize = 0, readSize;
    UINT8 *buf = NULL;
    UINT64 total = 0;
    EFI_STATUS st;

    st = Root->Open(Root, &File, (CHAR16 *)Path, 0x0000000000000001ULL /* EFI_FILE_MODE_READ */, 0);
    if (st != EFI_SUCCESS) {
        Print("[FS ] open '%s' failed, status %u\r\n", "(path)", (UINT32)st);
        return st;
    }

    st = File->GetInfo(File, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, NULL);
    if (st != EFI_SUCCESS || infoSize < sizeof(EFI_FILE_INFO)) {
        File->Close(File);
        return EFI_DEVICE_ERROR;
    }

    Info = (EFI_FILE_INFO *)BS->AllocatePool(EfiBootServicesData, infoSize, (VOID **)&Info);
    if (Info == NULL) { File->Close(File); return EFI_OUT_OF_RESOURCES; }

    st = File->GetInfo(File, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, Info);
    if (st != EFI_SUCCESS) {
        BS->FreePool(Info);
        File->Close(File);
        return st;
    }

    total = Info->FileSize;
    BS->FreePool(Info);

    /* the 32 bit PE payload is small; skip everything that is not */
    if (total > 16 * 1024 * 1024) return EFI_NOT_FOUND;

    if (total == 0) { File->Close(File); return EFI_NOT_FOUND; }

    buf = (UINT8 *)BS->AllocatePool(EfiBootServicesData, (UINTN)total, (VOID **)&buf);
    if (buf == NULL) { File->Close(File); return EFI_OUT_OF_RESOURCES; }

    readSize = (UINTN)total;
    st = File->Read(File, &readSize, buf);
    if (st != EFI_SUCCESS) {
        BS->FreePool(buf);
        File->Close(File);
        return st;
    }

    File->Close(File);
    *OutData = buf;
    *OutSize = (UINT64)readSize;
    return EFI_SUCCESS;
}

static EFI_STATUS LoadFileFromEsp(EFI_FILE_PROTOCOL *Root, const CHAR16 *const *Candidates,
                                   UINT8 **OutData, UINT64 *OutSize, CHAR16 *OutPath, UINTN OutPathMax)
{
    EFI_STATUS st = EFI_NOT_FOUND;
    UINTN i;

    for (i = 0; Candidates[i] != NULL; i++) {
        UINT8 *data = NULL;
        UINT64 size = 0;
        EFI_STATUS s = ReadEspFile(Root, Candidates[i], &data, &size);
        if (s == EFI_SUCCESS) {
            Print("[FS ] loaded '%s', %u bytes\r\n", "(esp file)", (UINT32)size);
            if (OutPath != NULL) {
                UINTN n = StrLenW(Candidates[i]);
                if (n >= OutPathMax) n = OutPathMax - 1;
                memcpy(OutPath, Candidates[i], n * sizeof(CHAR16));
                OutPath[n] = 0;
            }
            *OutData = data;
            *OutSize = size;
            return EFI_SUCCESS;
        }
        Print("[FS ] candidate %u not usable, status %u\r\n", (UINT32)i, (UINT32)s);
    }
    return st;
}

static EFI_STATUS LoadPayloadFromEsp(UINT8 **OutData, UINT64 *OutSize, CHAR16 *OutPath, UINTN OutPathMax)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS st;

    st = BS->LocateProtocol((EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, NULL, (VOID **)&fs);
    if (st != EFI_SUCCESS || fs == NULL) {
        Print("[FS ] LocateProtocol(SimpleFileSystem) failed, status %u\r\n", (UINT32)st);
        return st;
    }

    st = fs->OpenVolume(fs, &root);
    if (st != EFI_SUCCESS || root == NULL) {
        Print("[FS ] OpenVolume failed, status %u\r\n", (UINT32)st);
        return st;
    }

    st = LoadFileFromEsp(root, kPayloadCandidates, OutData, OutSize, OutPath, OutPathMax);
    root->Close(root);
    return st;
}


/* ------------------------------------------------------------------------- */
/*  Minimal PE32 (i386) loader                                               */
/* ------------------------------------------------------------------------- */

#pragma pack(push, 1)

typedef struct {
    UINT16 e_magic;
    UINT16 e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    UINT16 e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    UINT16 e_res[4];
    UINT16 e_oemid, e_oeminfo;
    UINT16 e_res2[10];
    INT32  e_lfanew;
} PE32_DOS_HEADER;

typedef struct {
    UINT16 Machine;
    UINT16 NumberOfSections;
    UINT32 TimeDateStamp;
    UINT32 PointerToSymbolTable;
    UINT32 NumberOfSymbols;
    UINT16 SizeOfOptionalHeader;
    UINT16 Characteristics;
} PE32_FILE_HEADER;

typedef struct {
    UINT32 VirtualAddress;
    UINT32 Size;
} PE32_DATA_DIRECTORY;

typedef struct {
    UINT16 Magic;
    UINT8  MajorLinkerVersion, MinorLinkerVersion;
    UINT32 SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    UINT32 AddressOfEntryPoint, BaseOfCode, BaseOfData;
    UINT32 ImageBase, SectionAlignment, FileAlignment;
    UINT16 MajorOperatingSystemVersion, MinorOperatingSystemVersion;
    UINT16 MajorImageVersion, MinorImageVersion;
    UINT16 MajorSubsystemVersion, MinorSubsystemVersion;
    UINT32 Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    UINT16 Subsystem, DllCharacteristics;
    UINT32 SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    UINT32 LoaderFlags, NumberOfRvaAndSizes;
    PE32_DATA_DIRECTORY DataDirectory[16];
} PE32_OPTIONAL_HEADER;

typedef struct {
    UINT32 VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
    UINT32 PointerToRelocations, PointerToLinenumbers;
    UINT16 NumberOfRelocations, NumberOfLinumbers;
    UINT32 Characteristics;
} PE32_SECTION_HEADER;

#pragma pack(pop)

#define PE32_MACHINE_I386   0x014C
#define PE32_MAGIC          0x010B
#define PE32_RELOC_HIGHLOW  3

typedef struct {
    UINT32 ImageBase;      /* actual load address                      */
    UINT32 EntryPoint;     /* actual entry point (VA)                  */
    UINT32 ImageSize;
    UINT32 PreferredBase;
    UINT32 RelocCount;
} PE32_IMAGE_INFO;

static UINT16 ReadU16(const UINT8 *p) { return (UINT16)(p[0] | ((UINT16)p[1] << 8)); }
static UINT32 ReadU32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

/*
 * Maps a PE32 image into physical memory below 4 GiB.
 *
 * The image is preferably placed at its preferred base (no relocation needed);
 * if that address is unavailable the image is loaded anywhere below 4 GiB and
 * the IMAGE_REL_BASED_HIGHLOW fixups are applied, so that every absolute
 * reference inside the image points at the new base.
 */
static EFI_STATUS LoadPe32Image(UINT8 *file, UINT64 fileSize, PE32_IMAGE_INFO *info)
{
    EFI_BOOT_SERVICES *BS = gBS;
    PE32_DOS_HEADER *dos;
    PE32_FILE_HEADER *fh;
    PE32_OPTIONAL_HEADER *oh;
    PE32_SECTION_HEADER *sections;
    EFI_PHYSICAL_ADDRESS base = 0;
    UINT8 *image = NULL;
    UINT32 i, delta, headersSize, imageSize, sectionCount;
    EFI_STATUS st = EFI_SUCCESS;

    if (fileSize < sizeof(PE32_DOS_HEADER) + 0x40) return EFI_LOAD_ERROR;

    dos = (PE32_DOS_HEADER *)file;
    if (ReadU16((const UINT8 *)&dos->e_magic) != 0x5A4D /* 'MZ' */) {
        Print("[PE ] bad DOS signature\r\n");
        return EFI_LOAD_ERROR;
    }

    if ((UINT32)dos->e_lfanew + sizeof(UINT32) + sizeof(PE32_FILE_HEADER) > fileSize)
        return EFI_LOAD_ERROR;

    if (ReadU32(file + (UINT32)dos->e_lfanew) != 0x00004550 /* 'PE\0\0' */) {
        Print("[PE ] bad PE signature\r\n");
        return EFI_LOAD_ERROR;
    }

    fh = (PE32_FILE_HEADER *)(file + (UINT32)dos->e_lfanew + 4);
    if (ReadU16((const UINT8 *)&fh->Machine) != PE32_MACHINE_I386) {
        Print("[PE ] not an i386 image (machine 0x%04x)\r\n", ReadU16((const UINT8 *)&fh->Machine));
        return EFI_LOAD_ERROR;
    }
    if (fh->SizeOfOptionalHeader < sizeof(PE32_OPTIONAL_HEADER) - 16 * sizeof(PE32_DATA_DIRECTORY)) {
        Print("[PE ] optional header too small\r\n");
        return EFI_LOAD_ERROR;
    }

    oh = (PE32_OPTIONAL_HEADER *)(file + (UINT32)dos->e_lfanew + 4 + sizeof(PE32_FILE_HEADER));
    if (ReadU16((const UINT8 *)&oh->Magic) != PE32_MAGIC) {
        Print("[PE ] not a PE32 image (magic 0x%04x)\r\n", ReadU16((const UINT8 *)&oh->Magic));
        return EFI_LOAD_ERROR;
    }

    imageSize   = ReadU32((const UINT8 *)&oh->SizeOfImage);
    headersSize = ReadU32((const UINT8 *)&oh->SizeOfHeaders);
    sectionCount = ReadU16((const UINT8 *)&fh->NumberOfSections);
    info->PreferredBase = ReadU32((const UINT8 *)&oh->ImageBase);
    info->ImageSize = imageSize;

    if (imageSize == 0 || imageSize > 0x40000000u) {
        Print("[PE ] implausible SizeOfImage 0x%08x\r\n", imageSize);
        return EFI_LOAD_ERROR;
    }

    sections = (PE32_SECTION_HEADER *)((UINT8 *)oh + fh->SizeOfOptionalHeader);

    /* 1. try the preferred base, 2. otherwise anywhere below 4 GiB */
    {
        EFI_PHYSICAL_ADDRESS want = (EFI_PHYSICAL_ADDRESS)(info->PreferredBase & ~0xFFFu);
        if (want != 0 &&
            BS->AllocatePages(AllocateAddress, EfiBootServicesData,
                              EFI_SIZE_TO_PAGES(imageSize), &want) == EFI_SUCCESS) {
            base = want;
        }
    }
    if (base == 0) {
        base = 0xFFFFFFFFULL; /* AllocateMaxAddress: keep everything below 4 GiB */
        st = BS->AllocatePages(AllocateMaxAddress, EfiBootServicesData,
                               EFI_SIZE_TO_PAGES(imageSize), &base);
        if (st != EFI_SUCCESS) {
            Print("[PE ] AllocatePages failed, status %u\r\n", (UINT32)st);
            return EFI_OUT_OF_RESOURCES;
        }
    }

    image = (UINT8 *)(UINTN)base;
    Print("[PE ] image base 0x%08x (preferred 0x%08x), size 0x%08x\r\n",
          (UINT32)base, info->PreferredBase, imageSize);

    /* 2. headers */
    if (headersSize > fileSize) headersSize = (UINT32)fileSize;
    if (headersSize > imageSize) headersSize = imageSize;
    memcpy(image, file, headersSize);

    /* 3. sections */
    for (i = 0; i < sectionCount; i++) {
        PE32_SECTION_HEADER *s = &sections[i];
        UINT32 va = ReadU32((const UINT8 *)&s->VirtualAddress);
        UINT32 rawSize = ReadU32((const UINT8 *)&s->SizeOfRawData);
        UINT32 rawPtr = ReadU32((const UINT8 *)&s->PointerToRawData);

        if (va == 0 || rawSize == 0) continue;
        if ((UINT64)rawPtr + rawSize > fileSize) {
            Print("[PE ] section %u truncated in file\r\n", i);
            return EFI_LOAD_ERROR;
        }
        if ((UINT64)va + rawSize > imageSize) {
            Print("[PE ] section %u outside of SizeOfImage\r\n", i);
            return EFI_LOAD_ERROR;
        }
        memcpy(image + va, file + rawPtr, rawSize);
    }

    /* 4. base relocations */
    delta = (UINT32)base - info->PreferredBase;
    info->RelocCount = 0;
    if (delta != 0 && oh->NumberOfRvaAndSizes > 5) {
        PE32_DATA_DIRECTORY *dir = &oh->DataDirectory[5]; /* BASE RELOCATION */
        UINT32 relocRva = ReadU32((const UINT8 *)&dir->VirtualAddress);
        UINT32 relocSize = ReadU32((const UINT8 *)&dir->Size);
        UINT32 off = 0;

        while (off + 8 <= relocSize) {
            UINT32 blockRva  = ReadU32(image + relocRva + off);
            UINT32 blockSize = ReadU32(image + relocRva + off + 4);
            UINT32 n, count;

            if (blockSize < 8 || off + blockSize > relocSize) break;

            count = (blockSize - 8) / 2;
            for (n = 0; n < count; n++) {
                UINT16 e = ReadU16(image + relocRva + off + 8 + n * 2);
                UINT16 type = (UINT16)(e >> 12);
                UINT16 roff = (UINT16)(e & 0x0FFF);

                if (type == PE32_RELOC_HIGHLOW) {
                    UINT32 target = blockRva + roff;
                    UINT32 value;
                    if ((UINT64)target + 4 > imageSize) continue;
                    value = ReadU32(image + target);
                    value += delta;
                    image[target + 0] = (UINT8)(value & 0xFF);
                    image[target + 1] = (UINT8)((value >> 8) & 0xFF);
                    image[target + 2] = (UINT8)((value >> 16) & 0xFF);
                    image[target + 3] = (UINT8)((value >> 24) & 0xFF);
                    info->RelocCount++;
                }
            }
            off += blockSize;
        }
    }

    info->ImageBase = (UINT32)base;
    info->EntryPoint = (UINT32)base + ReadU32((const UINT8 *)&oh->AddressOfEntryPoint);

    Print("[PE ] %u sections, delta 0x%08x, %u relocations applied\r\n",
          sectionCount, delta, info->RelocCount);
    Print("[PE ] entry point 0x%08x\r\n", info->EntryPoint);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Low memory handover area                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Layout of the handover page (one 4 KiB page below 4 GiB):
 *
 *   +0x000  32-bit GDT descriptor + GDTR      (filled in by transition.asm)
 *   +0x080  handover descriptor                (filled in here)
 *   +0x100  copy of the compatibility trampoline (filled in here)
 */
#define HANDOVER_GDT_OFFSET    0x000
#define HANDOVER_INFO_OFFSET   0x080
#define HANDOVER_CODE_OFFSET   0x100
#define HANDOVER_CODE_MAX      0x100
#define HANDOVER_STACK_PAGES   16

#pragma pack(push, 1)
typedef struct {
    UINT32 Magic;          /* 'B32P'                                    */
    UINT32 ImageBase;
    UINT32 EntryPoint;
    UINT32 ImageSize;
    UINT32 StackTop;
    UINT32 GdtSlot;
    UINT32 Trampoline;
    UINT32 Flags;
} BOOT32_HANDOVER;
#pragma pack(pop)

#define BOOT32_MAGIC 0x50323342u /* 'B32P' */

static EFI_STATUS AllocateBelow4G(EFI_MEMORY_TYPE Type, UINTN Pages, EFI_PHYSICAL_ADDRESS *OutAddress);

/*
 * Patch the temporary RAM of the SEC entry stub.
 *
 * The IA32 SEC entry stub (OvmfPkg/Sec/Ia32/SecEntry.nasm) contains the
 * temporary RAM base and the stack top as immediates, and the C part of the
 * SEC core derives the base from the stack top.  Both values are PCD
 * constants of the firmware build, so they are relocated together with the
 * firmware image - the same way a CSM module relocates the legacy BIOS it
 * hosts into a free hole of the upper memory.
 */
static void PatchSecTempRam(UINT8 *Image, UINTN SecOffset, UINT32 NewBase, UINT32 NewSize)
{
    UINT8 *sec = Image + SecOffset;
    UINT8 *text = sec + 0x240;          /* the .text section of the module */
    UINT32 textSize = 0x5000 + 0x100;   /* .text plus the .data section   */
    UINT32 oldBase = OVMF32_TEMP_RAM_BASE;
    UINT32 oldTop = OVMF32_TEMP_RAM_BASE + OVMF32_TEMP_RAM_SIZE;
    UINT32 newTop = NewBase + NewSize;
    UINT32 i, patched = 0;

    for (i = 0; i + 4 <= textSize; i++) {
        UINT32 value = ReadU32(text + i);
        if (value == oldBase) {
            *(UINT32 *)(text + i) = NewBase;
            patched++;
        } else if (value == oldTop) {
            *(UINT32 *)(text + i) = newTop;
            patched++;
        }
    }
    Print("[FV ] temporary RAM relocated to 0x%08x..0x%08x (%u references patched)\r\n",
          NewBase, newTop, patched);
}

/*
 * Find a free range of conventional memory below the 4 GiB ceiling.
 */
static EFI_STATUS FindFreeRegion(EFI_MEMORY_DESCRIPTOR *Map, UINTN MapSize, UINTN DescSize,
                                 UINTN Size, UINT64 AvoidBase, UINT64 AvoidSize, UINT32 *OutBase)
{
    UINTN i, count = MapSize / DescSize;

    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + i * DescSize);
        UINT64 base, end;

        if (d->Type != EfiConventionalMemory) continue;
        if (d->NumberOfPages * EFI_PAGE_SIZE < Size) continue;

        base = (d->PhysicalStart + EFI_PAGE_SIZE - 1) & ~((UINT64)EFI_PAGE_SIZE - 1);
        end = d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE;
        if (base + Size > end) continue;

        /* stay clear of the legacy area below 1 MiB */
        if (base < 0x100000ULL) {
            base = 0x100000ULL;
            if (base + Size > end) continue;
        }

        /* do not put the temporary RAM into the firmware image */
        if (AvoidSize != 0 && base < AvoidBase + AvoidSize && AvoidBase < base + Size) continue;

        *OutBase = (UINT32)base;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

/*
 * Apply the base relocations of the SEC core module.
 *
 * The 32 bit firmware is linked for its flash address, so its modules are not
 * position independent.  When the firmware is copied into memory below the
 * 4 GiB ceiling the SEC core has to be rebased, otherwise its absolute
 * references would still point into the flash area.
 */
static void RelocateSecModule(UINT8 *Image, UINTN SecOffset)
{
    UINT8 *sec = Image + SecOffset;
    UINT8 *relocs = sec + OVMF32_SEC_RELOC_RVA;
    UINT32 offset = 0;
    INT32 delta = (INT32)((UINTN)sec - (UINTN)OVMF32_SEC_LINK_BASE);
    UINT32 count, n;

    if (OVMF32_SEC_RELOC_SIZE == 0) return;

    while (offset + 8 <= OVMF32_SEC_RELOC_SIZE) {
        UINT32 pageRva = ReadU32(relocs + offset);
        UINT32 blockSize = ReadU32(relocs + offset + 4);

        if (blockSize < 8 || offset + blockSize > OVMF32_SEC_RELOC_SIZE) break;

        count = (blockSize - 8) / 2;
        for (n = 0; n < count; n++) {
            UINT16 entry = ReadU16(relocs + offset + 8 + n * 2);
            UINT16 type = (UINT16)(entry >> 12);
            UINT16 patch = (UINT16)(entry & 0x0FFF);

            if (type == 3) {           /* IMAGE_REL_BASED_HIGHLOW */
                UINT32 *target = (UINT32 *)(sec + pageRva + patch);
                *target += (UINT32)delta;
            }
        }
        offset += blockSize;
    }

    Print("[FV ] SEC core rebased by 0x%08x\r\n", (UINT64)(UINT32)delta);
}

/*
 * Remove a file from the EFI system partition.
 *
 * The 32 bit firmware that is hosted below scans /EFI/BOOT for its boot
 * option.  Our own 64 bit loader lives there as well and the 32 bit firmware
 * would try to execute it, so it is deleted before the hand-off.
 */
static EFI_STATUS DeleteEspFile(const CHAR16 *Path)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_FILE_INFO *info = NULL;
    UINTN infoSize = sizeof(EFI_FILE_INFO);
    EFI_STATUS st;

    st = BS->LocateProtocol((EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, NULL, (VOID **)&fs);
    Print("[FS ] LocateProtocol status=%u fs=0x%016x\\r\\n", (UINT32)st, (UINT64)(UINTN)fs);
    if (st != EFI_SUCCESS) return st;
    if (fs == NULL) { Print("[FS ] fs is NULL\\r\\n"); return EFI_NOT_FOUND; }
    Print("[FS ] fs->Revision=0x%016x fs->OpenVolume=0x%016x\\r\\n",
          fs->Revision, (UINT64)(UINTN)fs->OpenVolume);

    st = fs->OpenVolume(fs, &root);
    Print("[FS ] OpenVolume status=%u root=0x%016x\\r\\n", (UINT32)st, (UINT64)(UINTN)root);
    if (st != EFI_SUCCESS) return st;
    Print("[FS ] root->Revision=0x%016x root->Open=0x%016x root->Close=0x%016x\\r\\n",
          root->Revision, (UINT64)(UINTN)root->Open, (UINT64)(UINTN)root->Close);
    Print("[FS ] signature at root-8 = 0x%08x (expect 0x%08x)\\r\\n",
          *(UINT32 *)((UINT8 *)root - 8), 0x69746166);
    Print("[FS ] OFile at root+0x88 = 0x%016x\\r\\n",
          *(UINT64 *)((UINT8 *)root + 0x88));
    Print("[FS ] Volume at OFile+0 = 0x%016x\\r\\n",
          *(UINT64 *)*(UINT64 *)((UINT8 *)root + 0x88));

    /* test: try opening BCD instead */
    st = root->Open(root, &file, L"\\\\EFI\\\\BOOT\\\\BCD", EFI_FILE_MODE_READ, 0);
    if (st != EFI_SUCCESS) {
        root->Close(root);
        return st;
    }

    /* the FAT driver wants the file size to be zero before the delete */
    info = (EFI_FILE_INFO *)BS->AllocatePool(EfiBootServicesData, infoSize, (VOID **)&info);
    if (info != NULL) {
        st = file->GetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, info);
        if (st == EFI_SUCCESS) {
            info->FileSize = 0;
            st = file->SetInfo(file, (EFI_GUID *)&gEfiFileInfoGuid, &infoSize, info);
        }
        BS->FreePool(info);
    }

    if (st == EFI_SUCCESS) st = file->Delete(file);
    file->Close(file);
    root->Close(root);
    return st;
}

/*
 * Prepare the hosted 32 bit firmware:
 *   - copy the image below the 4 GiB ceiling,
 *   - rebase the SEC core,
 *   - reserve the temporary RAM the SEC entry stub uses.
 */
static EFI_STATUS PrepareHostedFirmware(EFI_MEMORY_DESCRIPTOR *Map, UINTN MapSize, UINTN DescSize,
                                        UINT32 *OutEntry, UINT32 *OutFvBase, UINT64 *OutStackTop)
{
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS base = 0;
    UINTN pages = EFI_SIZE_TO_PAGES(OVMF32_IMAGE_SIZE);

    st = AllocateBelow4G(EfiBootServicesData, pages, &base);
    if (st != EFI_SUCCESS) {
        Print("[FV ] no room for the 32 bit firmware below 4 GiB, status %u\r\n", (UINT32)st);
        return st;
    }

    memcpy((VOID *)(UINTN)base, Ovmf32Image, OVMF32_IMAGE_SIZE);
    Print("[FV ] firmware image %u bytes copied to 0x%08x\r\n",
          (UINT32)OVMF32_IMAGE_SIZE, (UINT32)base);

    /*
     * The firmware is built for a fixed "MEMFD" region at 0x800000: the SEC
     * entry stub uses 0x810000..0x820000 as temporary RAM, DecompressMemFvs
     * copies PEIFV to 0x820000 and DXEFV to 0x900000, and the LZMA decoder
     * works with its output buffer at 0xA00000 and scratch memory up to
     * 0x1A10000.  The whole region is reserved at its build address so no
     * PCD patching is needed at all.
     */
#define OVMF32_MEMFD_BASE  0x40000000ULL   /* relocated MEMFD_BASE_ADDRESS of the inner build */
#define OVMF32_MEMFD_SIZE  0x01310000ULL   /* 0x40000000 .. 0x41310000 */

    EFI_PHYSICAL_ADDRESS memfd = OVMF32_MEMFD_BASE;
    st = gBS->AllocatePages(AllocateAddress, EfiBootServicesData,
                            (UINTN)(OVMF32_MEMFD_SIZE / EFI_PAGE_SIZE), &memfd);
    if (st != EFI_SUCCESS || memfd != OVMF32_MEMFD_BASE) {
        Print("[FV ] the MEMFD region at 0x800000 is not available, status %u\r\n",
              (UINT32)st);
        return (st != EFI_SUCCESS) ? st : EFI_OUT_OF_RESOURCES;
    }
    Print("[FV ] MEMFD region 0x%08x..0x%08x reserved at the build address\r\n",
          (UINT32)memfd, (UINT32)(memfd + OVMF32_MEMFD_SIZE));

    RelocateSecModule((UINT8 *)(UINTN)base, OVMF32_SEC_FILE_OFFSET);

    /*
     * The SEC core scans the firmware volume passed in EBP for the SEC and
     * PEI core FFS files, so it must receive the address of the FV that
     * actually contains the SEC module (the SECFV), not the image start.
     */
    *OutFvBase = (UINT32)base + OVMF32_SEC_FV_OFFSET;
    *OutEntry = (UINT32)base + OVMF32_SEC_FILE_OFFSET + OVMF32_SEC_ENTRY_RVA;
    *OutStackTop = OVMF32_TEMP_RAM_BASE + OVMF32_TEMP_RAM_SIZE;
    return EFI_SUCCESS;
}

/*
 * Allocate a contiguous block of pages that lies completely below the 4 GiB
 * ceiling.  The strict AllocateMaxAddress request is tried first; some
 * firmware revisions only look into the memory bin of the requested type
 * then, and that bin can be too fragmented, so a plain allocation with a
 * range check is used as the fallback.
 */
static EFI_STATUS AllocateBelow4G(EFI_MEMORY_TYPE Type, UINTN Pages, EFI_PHYSICAL_ADDRESS *OutAddress)
{
    EFI_BOOT_SERVICES *BS = gBS;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS st;
    UINT32 attempt;

    addr = 0xFFFFFFFFULL;
    st = BS->AllocatePages(AllocateMaxAddress, Type, Pages, &addr);
    if (st == EFI_SUCCESS) {
        if ((addr + (Pages * EFI_PAGE_SIZE)) <= 0x100000000ULL) {
            *OutAddress = addr;
            return EFI_SUCCESS;
        }
        BS->FreePages(addr, Pages);
    }

    for (attempt = 0; attempt < 16; attempt++) {
        addr = 0;
        st = BS->AllocatePages(AllocateAnyPages, Type, Pages, &addr);
        if (st != EFI_SUCCESS) return st;
        if ((addr + (Pages * EFI_PAGE_SIZE)) <= 0x100000000ULL) {
            *OutAddress = addr;
            return EFI_SUCCESS;
        }
        BS->FreePages(addr, Pages);
    }

    return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS ReserveHandover(EFI_PHYSICAL_ADDRESS *OutPage, EFI_PHYSICAL_ADDRESS *OutStackTop)
{
    EFI_PHYSICAL_ADDRESS page = 0xFFFFFFFFULL;
    EFI_PHYSICAL_ADDRESS stack = 0xFFFFFFFFULL;
    EFI_STATUS st;

    st = AllocateBelow4G(EfiBootServicesData, 1, &page);
    if (st != EFI_SUCCESS) {
        Print("[HND] handover page allocation failed, status %u\r\n", (UINT32)st);
        return st;
    }

    st = AllocateBelow4G(EfiBootServicesData, HANDOVER_STACK_PAGES, &stack);
    if (st != EFI_SUCCESS) {
        Print("[HND] 32-bit stack allocation failed, status %u\r\n", (UINT32)st);
        return st;
    }

    memset((VOID *)(UINTN)page, 0, EFI_PAGE_SIZE);

    *OutPage = page;
    *OutStackTop = stack + HANDOVER_STACK_PAGES * EFI_PAGE_SIZE;
    /* keep the 32-bit stack 16 byte aligned */
    *OutStackTop &= ~0xFULL;

    Print("[HND] handover page 0x%016x, 32-bit stack top 0x%016x\r\n",
          page, *OutStackTop);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Entry point                                                              */
/* ------------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS st;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN mapSize = 0, descSize = 0, mapKey = 0;
    UINT8 *file = NULL;
    UINT64 fileSize = 0;
    UINT32 entry = 0;
    CHAR16 chosen[64];
    PE32_IMAGE_INFO pe;
    EFI_PHYSICAL_ADDRESS handoverPage = 0, stackTop = 0;
    BOOT32_HANDOVER *handover;
    UINTN trampolineSize;

    UefiInit(SystemTable);
    if (gBS == NULL) return EFI_UNSUPPORTED;
    /* boot services table verified: 45 members (UEFI 2.8 layout with Reserved slot) */


    /* --- 1. memory map ---------------------------------------------------- */
    Print("[UEFI] querying the memory map\r\n");
    Mark('F');
    st = FetchMemoryMap(&map, &mapSize, &mapKey, &descSize);
    Mark('G');
    if (st != EFI_SUCCESS) {
        Print("[UEFI] GetMemoryMap failed, status %u\r\n", (UINT32)st);
        return st;
    }
    DumpMemoryMap(map, mapSize, descSize);

    /* raw dump of the first descriptor */
    {
        UINTN k;
        static const CHAR8 hexd[] = "0123456789abcdef";
        SerPuts("[DBG] raw groups:");
        for (k = 0; k < 96; k++) {
            if ((k % 8) == 0) SerPuts(" | ");
            SerPutc(hexd[((UINT8 *)map)[k] >> 4]);
            SerPutc(hexd[((UINT8 *)map)[k] & 0xF]);
        }
        SerPuts("\r\n");
        {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)map;
            SerPuts("[DBG] parsed type ");
            SerHex64(d->Type);
            SerPuts(" phys ");
            SerHex64(d->PhysicalStart);
            SerPuts(" virt ");
            SerHex64(d->VirtualStart);
            SerPuts(" pages ");
            SerHex64(d->NumberOfPages);
            SerPuts(" attr ");
            SerHex64(d->Attribute);
            SerPuts("\r\n");
        }
    }

    /* --- 2. target ------------------------------------------------------- */
    Print("[UEFI] preparing the hand-off\r\n");
    chosen[0] = 0;
    entry = 0;

    /*
     * The handover page carries the 32 bit GDT and a copy of the
     * compatibility mode trampoline; both modes need them.
     */
    st = ReserveHandover(&handoverPage, &stackTop);
    if (st != EFI_SUCCESS) {
        gBS->FreePool(map);
        return st;
    }

    /*
     * Copy the compatibility mode trampoline into low memory: once paging is
     * disabled the CPU can only fetch 32-bit linear addresses.
     */
    trampolineSize = (UINTN)(TransitionCompatTrampolineEnd - TransitionCompatTrampoline);
    if (trampolineSize == 0 || trampolineSize > HANDOVER_CODE_MAX) {
        Print("[UEFI] compatibility trampoline symbol missing or too large\r\n");
        return EFI_LOAD_ERROR;
    }
    memcpy((UINT8 *)(UINTN)handoverPage + HANDOVER_CODE_OFFSET,
           TransitionCompatTrampoline, trampolineSize);

    handover = (BOOT32_HANDOVER *)((UINT8 *)(UINTN)handoverPage + HANDOVER_INFO_OFFSET);
    handover->Magic       = BOOT32_MAGIC;
    handover->StackTop    = (UINT32)stackTop;
    handover->GdtSlot     = (UINT32)(handoverPage + HANDOVER_GDT_OFFSET);
    handover->Trampoline  = (UINT32)(handoverPage + HANDOVER_CODE_OFFSET);
    handover->Flags       = 0;

    TransitionGdtSlot32          = handoverPage + HANDOVER_GDT_OFFSET;
    TransitionCompatTrampoline32 = handoverPage + HANDOVER_CODE_OFFSET;
    TransitionHandover32         = handoverPage + HANDOVER_INFO_OFFSET;

    /*
     * Mode A: the 32 bit EDK2 firmware that is linked into this application
     *         is started as a payload of the 64 bit application.  This is
     *         the arrangement a CSM uses to host a legacy BIOS: the 64 bit
     *         firmware prepares the environment and hands the CPU over to
     *         the 32 bit firmware, which then boots on its own and starts
     *         the 32 bit Windows environment.
     *
     * Mode B: a plain 32 bit Windows PE image is loaded and entered.
     */
    {
        UINT32 fvBase = 0;
        UINT64 stackTop32 = 0;
        (void)map; (void)mapSize; (void)descSize;

        /*
         * If the ESP carries a 32 bit PE image it is used directly, otherwise
         * the hosted 32 bit firmware is started.
         */
        st = EFI_NOT_FOUND;
        if (LoadPayloadFromEsp(&file, &fileSize, chosen, sizeof(chosen) / sizeof(chosen[0])) == EFI_SUCCESS) {
            memset(&pe, 0, sizeof(pe));
            if (LoadPe32Image(file, fileSize, &pe) == EFI_SUCCESS) {
                handover->ImageBase   = pe.ImageBase;
                handover->EntryPoint  = pe.EntryPoint;
                handover->ImageSize   = pe.ImageSize;
                TransitionBootArg32 = (UINT64)(UINTN)handover;
                TransitionStackTop32 = stackTop;
                entry = pe.EntryPoint;
                st = EFI_SUCCESS;
                Print("[UEFI] 32 bit PE at 0x%08x, entry 0x%08x\r\n", pe.ImageBase, pe.EntryPoint);
            }
            gBS->FreePool(file);
        }

        if (st != EFI_SUCCESS) {
            st = PrepareHostedFirmware(map, mapSize, descSize, &entry, &fvBase, &stackTop32);
        }
        if (st == EFI_SUCCESS && fvBase != 0) {
            /* the SEC core expects the firmware volume pointer in EBP */
            TransitionBootArg32 = fvBase;
            TransitionStackTop32 = stackTop32;

            Print("[UEFI] starting the hosted 32 bit firmware\r\n");
            Print("[UEFI] firmware volume 0x%08x, SEC entry 0x%08x\r\n", fvBase, entry);
        } else {
            /* Mode B: a 32 bit PE image from the ESP */
            st = LoadPayloadFromEsp(&file, &fileSize, chosen, sizeof(chosen) / sizeof(chosen[0]));
            if (st != EFI_SUCCESS) {
                Print("[UEFI] no 32 bit target available, status %u\r\n", (UINT32)st);
                gBS->FreePool(map);
                return EFI_NOT_FOUND;
            }

            memset(&pe, 0, sizeof(pe));
            st = LoadPe32Image(file, fileSize, &pe);
            gBS->FreePool(file);
            if (st != EFI_SUCCESS) {
                Print("[UEFI] PE32 load failed, status %u\r\n", (UINT32)st);
                gBS->FreePool(map);
                return st;
            }

            handover->ImageBase   = pe.ImageBase;
            handover->EntryPoint  = pe.EntryPoint;
            handover->ImageSize   = pe.ImageSize;

            TransitionBootArg32 = (UINT64)(UINTN)handover;
            TransitionStackTop32 = stackTop;
            entry = pe.EntryPoint;

            Print("[HND] 32 bit PE at 0x%08x, entry 0x%08x\r\n", pe.ImageBase, pe.EntryPoint);
        }
    }

    /*
     * The hosted 32 bit firmware builds its boot options from /EFI/BOOT and
     * would pick our own 64 bit loader.  Remove it so that the 32 bit
     * firmware boots the 32 bit Windows boot manager instead.
     */
    /* deletion already done earlier, before PrepareHostedFirmware */

    /* --- 4. leave UEFI ---------------------------------------------------- */
    /*
     * The firmware console stops working as soon as ExitBootServices has
     * been issued, so from here on only the serial port is used.
     */
    gConsoleDead = TRUE;
    Print("[UEFI] requesting ExitBootServices\r\n");
    gBS->FreePool(map);
    st = ExitBootServices(ImageHandle);
    if (st != EFI_SUCCESS) {
        Print("[UEFI] ExitBootServices failed, status %u\r\n", (UINT32)st);
        return st;
    }

    /* --- 5. hand off ------------------------------------------------------ */
    /* reinitialize COM1 to 115200 8N1 */
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x80), "Nd"((unsigned short)0x3FB));
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x01), "Nd"((unsigned short)0x3F8));
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x00), "Nd"((unsigned short)0x3F9));
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x03), "Nd"((unsigned short)0x3FB));
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0x00), "Nd"((unsigned short)0x3FC));
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)0xC7), "Nd"((unsigned short)0x3FA));
    Mark('1');
    Print("=== HOSTED 32-BIT FIRMWARE STARTING ===\\r\\n");
    Print("[UEFI] leaving long mode, entering 32-bit protected mode\r\n");
    Mark('2');
    Print("[UEFI] 32-bit entry point 0x%08x\r\n", entry);
    Mark('3');

    /* handover page ready */
    TransitionTo32BitAndJump(handoverPage, (UINT64)entry, TransitionBootArg32, TransitionStackTop32);

    /* not reached - the transition is a one way hand-off */
    Print("[UEFI] FATAL: transition returned unexpectedly\r\n");
    return EFI_LOAD_ERROR;
}
