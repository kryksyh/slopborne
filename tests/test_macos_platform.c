#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_restart(void) { abort(); }
uint32_t runtime_thread_tcb_offset(void);
int32_t runtime_thread_spawn(void **,void *(ABI *)(void *),void *,uint64_t,const char *);
int32_t runtime_thread_join_spawned(void *,void **);
static void *(ABI *read_guest_tcb)(void);
static void *main_tcb;
static void *(ABI *guest_self)(void);
static void check_thread_handle(uint64_t *tcb) {
    uintptr_t handle=(uintptr_t)guest_self();
    /* The game's lock owner comparison uses the full self handle after
     * storing only its low 40 bits. Check both main and spawned threads. */
    assert(handle && handle==(handle&UINT64_C(0xffffffffff)));
    assert(tcb[2]==handle);
}
static void *ABI worker(void *arg) {
    assert(arg==main_tcb);
    uint64_t *tcb=read_guest_tcb();
    assert(tcb && tcb!=main_tcb && tcb[0]==(uintptr_t)tcb);
    assert((uintptr_t)tcb<UINT64_C(0x10000000000));
    check_thread_handle(tcb);
    assert(*((unsigned char *)tcb-64)==42);
    *((unsigned char *)tcb-64)=99;
    return tcb;
}
int main(void) {
    size_t size=16384;
    unsigned char *page=runtime_low_map(size,PROT_READ|PROT_WRITE);
    assert(page && (uintptr_t)page<UINT64_C(0x10000000000));
    page[0]=123;
    assert(bb_macos_map_fixed((uintptr_t)page,size,PROT_READ|PROT_WRITE)==MAP_FAILED);
    assert(page[0]==123); /* fixed allocations must never clobber host mappings */
    const unsigned char initial[64]={42};
    runtime_set_main_tls(initial,sizeof(initial),sizeof(initial),64);
    runtime_thread_attach_main();
    guest_self=(void *(ABI *)(void))runtime_thread_resolve("aI+OeCz8xrQ#p#J");
    assert(guest_self);
    unsigned char code[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    uint32_t offset=runtime_thread_tcb_offset();
    memcpy(code+5,&offset,sizeof(offset));
    memcpy(page,code,sizeof(code));
    assert(mprotect(page,size,PROT_READ|PROT_EXEC)==0);
    read_guest_tcb=(void *(ABI *)(void))page;
    main_tcb=read_guest_tcb();
    assert(main_tcb && *(uint64_t *)main_tcb==(uintptr_t)main_tcb);
    assert((uintptr_t)main_tcb<UINT64_C(0x10000000000));
    check_thread_handle(main_tcb);
    void *thread=NULL,*result=NULL;
    assert(runtime_thread_spawn(&thread,worker,main_tcb,1024*1024,"mac-tls-test")==0);
    assert((uintptr_t)thread<UINT64_C(0x10000000000));
    assert(runtime_thread_join_spawned(thread,&result)==0 && result!=main_tcb);
    assert(*((unsigned char *)main_tcb-64)==42);
    puts("PASS: fixed mapping collisions, executable guest GS loads, isolated thread TLS");
}
