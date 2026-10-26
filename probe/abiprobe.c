// Checks the two Android facts the Switch native host depends on:
// 1) anonymous memory can be written, then executed (the loaded game modules);
// 2) bionic's TLS_SLOT_APP (tpidr_el0[2]) is free per thread and survives syscalls,
//    so it can hold the host's per-thread slot block (Apple keeps it at tpidrro_el0).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t *tp(void) { uint64_t *t; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t)); return t; }

static void *worker(void *arg) {
    uint64_t *t = tp();
    printf("thread %ld: tp[2] before = 0x%llx\n", (long)arg, (unsigned long long)t[2]);
    t[2] = 0xabc0000 + (uint64_t)arg;
    usleep(20000);   // syscalls and a context switch in between
    printf("thread %ld: tp[2] after  = 0x%llx\n", (long)arg, (unsigned long long)tp()[2]);
    return NULL;
}

int main(void) {
    uint32_t code[] = {0xd2800540, 0xd65f03c0};   // mov x0, #42; ret
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(p, code, sizeof code);
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC)) { perror("mprotect RX"); return 1; }
    __builtin___clear_cache((char *)p, (char *)p + sizeof code);
    printf("executed mapped code: %d\n", ((int (*)(void))p)());
    pthread_t th[3];
    for (long i = 0; i < 3; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    uint64_t x18; __asm__ volatile("mov %0, x18" : "=r"(x18));
    printf("main: tp[2] = 0x%llx, x18 = 0x%llx\n", (unsigned long long)tp()[2], (unsigned long long)x18);
    return 0;
}
