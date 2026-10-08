// SPDX-License-Identifier: MIT
// Compare V3D-style cache-line loops on owned, page-aligned Pi memory.
// This exercises DC CIVAC in userspace; native driver tests must also
// qualify DC IVAC and CPU/GPU ownership before adopting a kernel change.
#include <OS.h>
#include <algorithm>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <syscalls.h>
struct Buffer { void* address; size_t size; };
static sigjmp_buf fault;
static volatile sig_atomic_t probing;
static volatile uint64_t checksum;
static void handler(int signo) { if(probing)siglongjmp(fault,signo); _exit(128+signo); }
static inline size_t line_size() { uint64_t ctr;asm volatile("mrs %0, ctr_el0":"=r"(ctr));return 4ul<<((ctr>>16)&15); }
static inline void civac(uintptr_t address) {asm volatile("dc civac, %0"::"r"(address):"memory");}
static inline void barrier() {asm volatile("dsb sy":::"memory");}
// Keep the baseline's memory-clobber-dependent loop bound exactly as in V3D.
__attribute__((noinline,noclone)) static void original(Buffer* buffer)
{
 size_t lineSize=line_size();
 for(uintptr_t line=(uintptr_t)buffer->address;
     line<(uintptr_t)buffer->address+buffer->size;line+=lineSize)civac(line);
 barrier();
}
__attribute__((noinline,noclone)) static void bounded(Buffer* buffer)
{
 const uintptr_t start=(uintptr_t)buffer->address;
 const uintptr_t end=start+buffer->size;
 const size_t lineSize=line_size();
 for(uintptr_t line=start;line<end;line+=lineSize)civac(line);
 barrier();
}
__attribute__((noinline,noclone)) static void unrolled(Buffer* buffer)
{
 const uintptr_t start=(uintptr_t)buffer->address;
 const uintptr_t end=start+buffer->size;
 const size_t lineSize=line_size();
 uintptr_t line=start;
 while(end-line>=8*lineSize) {
  civac(line);civac(line+lineSize);civac(line+2*lineSize);civac(line+3*lineSize);
  civac(line+4*lineSize);civac(line+5*lineSize);civac(line+6*lineSize);civac(line+7*lineSize);
  line+=8*lineSize;
 }
 for(;line<end;line+=lineSize)civac(line);
 barrier();
}
static void require(bool ok,const char* message) {if(!ok){fprintf(stderr,"FAIL %s errno=%d\n",message,errno);exit(1);}}
int main()
{
 setvbuf(stdout,nullptr,_IOLBF,0);
 uint64_t cpus=1;
 status_t pinned=_kern_set_thread_affinity(find_thread(nullptr),&cpus,sizeof(cpus));
 require(pinned==B_OK,"pin CPU 0");
 struct sigaction sa={};sa.sa_handler=handler;sigemptyset(&sa.sa_mask);
 require(sigaction(SIGILL,&sa,nullptr)==0&&sigaction(SIGSEGV,&sa,nullptr)==0,"signal handler");
 void* base=nullptr;require(posix_memalign(&base,4096,8*1024*1024)==0,"allocate");
 memset(base,0x5a,8*1024*1024);Buffer buffer={base,8*1024*1024};
 probing=1;
 int signal=sigsetjmp(fault,1);
 if(signal){printf("SKIP user cache maintenance denied signal=%d\n",signal);free(base);return 77;}
 civac((uintptr_t)base);barrier();probing=0;
 printf("CACHE_LOOP line_size=%zu cpu=0 instruction=DC_CIVAC\n",line_size());
 struct Variant { const char* name;void(*run)(Buffer*); };
 Variant variants[]={{"original",original},{"bounded",bounded},{"unrolled8",unrolled}};
 for(size_t size:{size_t(4096),size_t(65536),size_t(1048576),size_t(3686400),size_t(6681600),size_t(8388608)}) {
  if(getenv("CACHE_BENCH_QUICK") && size>4096)continue;
  buffer.size=size;
  for(const char* mode:{"cold","read","dirty"}) {
   for(unsigned round=0;round<2;++round) for(unsigned order=0;order<3;++order) {
    unsigned which=round?2-order:order;auto variant=variants[which];
    std::vector<bigtime_t> times;
    for(unsigned n=0;n<80;++n) {
     if(!strcmp(mode,"dirty"))memset(base,0x5a,size);
     else if(!strcmp(mode,"read")) {
      uint64_t sum=0;auto data=(const volatile uint64_t*)base;
      for(size_t offset=0;offset<size/8;offset+=line_size()/8)sum+=data[offset];
      checksum=sum;
     }
     bigtime_t start=system_time();variant.run(&buffer);bigtime_t elapsed=system_time()-start;
     if(n>=8)times.push_back(elapsed);
    }
    std::sort(times.begin(),times.end());
    printf("CACHE_LOOP size=%zu mode=%s round=%u variant=%s median_us=%lld min_us=%lld p90_us=%lld samples=%zu\n",
     size,mode,round,variant.name,(long long)times[times.size()/2],(long long)times.front(),(long long)times[times.size()*9/10],times.size());
    auto bytes=(const unsigned char*)base;
    for(size_t i=0;i<size;++i)require(bytes[i]==0x5a,"cache operation preserved data");
   }
  }
 }
 free(base);puts("CACHE_LOOP_PASS");
}
