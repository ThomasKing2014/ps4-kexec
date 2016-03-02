/*
 * ps4-kexec - a kexec() implementation for Orbis OS / FreeBSD
 *
 * Copyright (C) 2015-2016 shuffle2 <godisgovernment@gmail.com>
 * Copyright (C) 2015-2016 Hector Martin "marcan" <marcan@marcan.st>
 *
 * This code is licensed to you under the 2-clause BSD license. See the LICENSE
 * file for more information.
 */

#include "kernel.h"
#include "string.h"
#include "elf.h"
#include "x86.h"

#define KERNBASE    0xffffffff80000000ull
#define KERNSIZE    0x2000000

struct ksym_t kern;
int (*early_printf)(const char *fmt, ...) = NULL;

#define eprintf(...) do { if (early_printf) early_printf(__VA_ARGS__); } while(0)

static const u8 ELF_IDENT[9] = "\x7f" "ELF\x02\x01\x01\x09\x00";
static Elf64_Sym *symtab;
static char *strtab;
static size_t strtab_size;

static Elf64_Ehdr *find_kern_ehdr(void)
{
    // Search for the kernel copy embedded in ubios, then follow it to see
    // where it was relocated to
    for (uintptr_t p = KERNBASE; p < KERNBASE + KERNSIZE; p += PAGE_SIZE) {
        Elf64_Ehdr *ehdr = (Elf64_Ehdr *)p;
        if (!memcmp(ehdr->e_ident, ELF_IDENT, sizeof(ELF_IDENT))) {
            for (size_t i = 0; i < ehdr->e_phnum; i++) {
                Elf64_Phdr *phdr = (Elf64_Phdr *)(p + ehdr->e_phoff) + i;
                if (phdr->p_type == PT_PHDR) {
                    return (Elf64_Ehdr *)(phdr->p_vaddr - ehdr->e_phoff);
                }
            }
        }
    }
    return NULL;
}

static Elf64_Dyn *elf_get_dyn(Elf64_Ehdr *ehdr)
{
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)ehdr + ehdr->e_phoff);
    for (size_t i = 0; i < ehdr->e_phnum; i++, phdr++) {
        if (phdr->p_type == PT_DYNAMIC) {
            return (Elf64_Dyn *)phdr->p_vaddr;
        }
    }
    return NULL;
}

static int elf_parse_dyn(Elf64_Dyn *dyn)
{
    for (Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL; dp++) {
        switch (dp->d_tag) {
            case DT_SYMTAB:
                symtab = (Elf64_Sym *)dp->d_un.d_ptr;
                break;
            case DT_STRTAB:
                strtab = (char *)dp->d_un.d_ptr;
                break;
            case DT_STRSZ:
                strtab_size = dp->d_un.d_val;
                break;
        }
    }
    return symtab && strtab && strtab_size;
}

void *kernel_resolve(const char *name)
{
    for (Elf64_Sym *sym = symtab; (uintptr_t)(sym + 1) < (uintptr_t)strtab; sym++) {
        if (!strcmp(name, &strtab[sym->st_name])) {
            eprintf("kern.%s = %p\n", name, (void*)sym->st_value);
            return (void *)sym->st_value;
        }
    }
    eprintf("Failed to resolve symbol '%s'\n", name);
    return NULL;
}

#define RESOLVE(name) if (!(kern.name = kernel_resolve(#name))) return 0;

static int resolve_symbols(void)
{
    RESOLVE(printf);
    early_printf = kern.printf;
    RESOLVE(copyin);
    RESOLVE(copyout);
    RESOLVE(copyinstr);
    RESOLVE(kernel_map);
    RESOLVE(kernel_pmap_store);
    RESOLVE(kmem_alloc_contig);
    RESOLVE(kmem_free);
    RESOLVE(pmap_extract);
    RESOLVE(pmap_protect);
    RESOLVE(sysent);
    RESOLVE(sched_pin);
    RESOLVE(sched_unpin);
    RESOLVE(smp_rendezvous);
    RESOLVE(smp_no_rendevous_barrier);
    return 1;
}

#define	M_WAITOK 0x0002
#define	M_ZERO   0x0100

#define	VM_MEMATTR_DEFAULT		0x06

void *kernel_alloc_contig(size_t size)
{
    // use kmem_alloc_contig instead of contigalloc to avoid messing with a malloc_type...
    vm_offset_t ret = kern.kmem_alloc_contig(
        *kern.kernel_map, size, M_ZERO | M_WAITOK, (vm_paddr_t)0,
        ~(vm_paddr_t)0, 1, 0, VM_MEMATTR_DEFAULT);

    if (!ret) {
        kern.printf("Failed to allocate %d bytes\n", size);
        return NULL;
    }
    return (void *)PA_TO_DM(kern.pmap_extract(kern.kernel_pmap_store, ret));
}

void kernel_free_contig(void *addr, size_t size)
{
    if (!addr)
        return;
    kern.kmem_free(*kern.kernel_map, (vm_offset_t)addr, size);
}

int kernel_hook_install(void *target, void *hook)
{
    uintptr_t t = (uintptr_t)target; // addr to redirect to
    uintptr_t h = (uintptr_t)hook; // place to write the thunk

    if (!hook || !target) {
        return 0;
    }

    if (!(t & (1L << 63))) {
        kern.printf("\n===================== WARNING =====================\n");
        kern.printf("hook target function address: %p\n", t);
        kern.printf("It looks like we're running from userland memory.\n");
        kern.printf("Please run this code from a kernel memory mapping.\n\n");
        return 0;
    }

    struct __attribute__((packed)) jmp_t{
        u8 op[1];
        s32 imm;
    } jmp = {
        .op = { 0xe9 },
        .imm = t - (h + sizeof(jmp)),
    };
    ASSERT_STRSIZE(struct jmp_t, 5);

    kern.sched_pin();
    cr0_write(cr0_read() & ~CR0_WP);
    memcpy(hook, &jmp, sizeof(jmp));
    wbinvd();
    cr0_write(cr0_read() | CR0_WP);
    kern.sched_unpin();

    return 1;
}

void kernel_syscall_install(int num, void *call, int narg)
{
    struct sysent_t *sy = &kern.sysent[num];

    kern.sched_pin();
    cr0_write(cr0_read() & ~CR0_WP);

    memset(sy, 0, sizeof(*sy));
    sy->sy_narg = narg;
    sy->sy_call = call;
    sy->sy_thrcnt = 1;

    cr0_write(cr0_read() | CR0_WP);
    kern.sched_unpin();
}

#ifndef DO_NOT_REMAP_RWX
extern u8 _start[], _end[];
#endif

void kernel_remap(void *start, void *end, int perm)
{
    u64 s = ((u64)start) & ~(u64)(PAGE_SIZE-1);
    u64 e = ((u64)end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE-1);

    kern.printf("pmap_protect(pmap, %p, %p, %d)\n", (void*)s, (void*)e, perm);
    kern.pmap_protect(kern.kernel_pmap_store, s, e, 7);
}

static volatile int _global_test = 0;

static int patch_pmap_check(void)
{
    u8 *p;

    for (p = (u8*)kern.pmap_protect;
         p < ((u8*)kern.pmap_protect + 0x500); p++) {
        if (!memcmp(p, "\x83\xe0\x06\x83\xf8\x06", 6)) {
            p[2] = 0;
            kern.printf("pmap_protect patch successful (found at %p)\n", p);
            return 1;
        }
    }
    kern.printf("pmap_protect patch failed!\n");
    return 0;
}

int kernel_init(void)
{
    eprintf("kernel_init()\n");

    // We may not be mapped writable yet, so to be able to write to globals
    // we need WP disabled.
    disable_interrupts();
    cr0_write(cr0_read() & ~CR0_WP);

    Elf64_Ehdr *ehdr = find_kern_ehdr();
    if (!ehdr) {
        eprintf("Could not find kernel ELF header\n");
        goto err;
    }
    eprintf("ELF header at %p\n", ehdr);

    Elf64_Dyn *dyn = elf_get_dyn(ehdr);
    if (!dyn) {
        eprintf("Could not find kernel dynamic header\n");
        goto err;
    }
    eprintf("ELF dynamic section at %p\n", dyn);

    if (!elf_parse_dyn(dyn)) {
        eprintf("Failed to parse ELF dynamic section\n");
        goto err;
    }

    if (!resolve_symbols()) {
        eprintf("Failed to resolve all symbols\n");
        goto err;
    }

#ifndef DO_NOT_REMAP_RWX
    if (!patch_pmap_check())
        goto err;
#endif

    // kernel_remap may need interrupts, but may not write to globals!
    cr0_write(cr0_read() | CR0_WP);
    enable_interrupts();

#ifndef DO_NOT_REMAP_RWX
    kernel_remap(_start, _end, 7);
#endif

    // Writing to globals is now safe.

    kern.printf("Testing global variable access (write protection)...\n");
    _global_test = 1;
    kern.printf("OK.\n");

    kern.printf("Kernel interface initialized\n");
    return 0;

err:
    cr0_write(cr0_read() | CR0_WP);
    enable_interrupts();
    return -1;
}