/* Switch compatibility shim (devkitA64 newlib has no <sys/mman.h>).
 * Only sljit (N64Recomp LiveRecomp, used for code mods) includes it; mods are
 * disabled on Switch, so every mapping request fails cleanly at runtime. */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif
static inline void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return MAP_FAILED;
}
static inline int munmap(void* addr, size_t len) { (void)addr; (void)len; return -1; }
static inline int mprotect(void* addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return -1; }
#ifdef __cplusplus
}
#endif
