/*
 * Local hooks for the payload SDK's libc.a when it is linked into a
 * home-screen module. Not from ProsperoStore.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * OpenSSL asks for dladdr. No public stub exports it, and libc's dlfcn.o
 * would also define dlopen and leave a weak __dlopen import the module
 * converter rejects. dlopen/dlsym/dlclose/dlerror stay libkernel imports.
 */
int dladdr(const void *addr, void *info)
{
    (void)addr;
    (void)info;
    return 0;
}

/*
 * libc mman.o is omitted so mmap/mprotect come from libkernel. If a weak
 * reference still survives, executable remaps fail instead of importing a
 * symbol no stub exports.
 */
__attribute__((weak)) int kernel_mprotect(int pid, long addr, unsigned long size, int prot)
{
    (void)pid;
    (void)addr;
    (void)size;
    (void)prot;
    return -1;
}

/*
 * Same register shuffle as the payload SDK's __syscall. A strong copy in
 * libc syscalls.o overrides this when that object is linked.
 */
__attribute__((weak)) void __syscall(void)
{
    __asm__ volatile(
        "mov %rdi, %rax\n\t"
        "mov %rsi, %rdi\n\t"
        "mov %rdx, %rsi\n\t"
        "mov %rcx, %rdx\n\t"
        "mov %r8, %r10\n\t"
        "mov %r9, %r8\n\t"
        "mov 8(%rsp), %r9\n\t"
        "syscall\n\t"
        "ret\n\t");
}

__attribute__((weak)) int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso)
{
    (void)dtor;
    (void)obj;
    (void)dso;
    return 0;
}
