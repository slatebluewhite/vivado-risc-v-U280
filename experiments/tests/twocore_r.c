// twocore_r.c — 발행이 병목이 된 뒤에도 "코어별 = 단일 코어"인가 (E174).
//
// E168은 d9(코어마다 INT8 16x16 하나)에서 두 코어 스핀 동기화가 1.88배,
// 단일 코어 두 가속기 교차 발행이 1.90배로 **같다**고 결론했다.
// 그런데 그건 옛 스테퍼(타일당 명령 6개, 활용률 12%) 기준이다.
//
// E173에서 분할 경로는 타일 연산당 여유가 2.6 cycle뿐인 **발행 바운드**임을 확인했다.
// 코어별 방식은 발행이 두 코어로 나뉘므로 이 병목을 받지 않는다.
// 따라서 재사용 스테퍼에서는 **두 코어가 단일 코어를 이겨야 한다.**
//
// 같은 비트스트림·같은 바이너리에서 옛/새 스테퍼를 모두 재서, E168의 1.88배가
// 재현되는지까지 함께 확인한다(재현되면 비교의 기준선이 성립한다).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

// d9는 **코어마다** INT8 16x16 하나 — 양쪽 다 custom3다.
static const grt_ctx I8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };

#define MAXN 256
#define FREQ 40.0e6            // Rocket64b2gem16wf40
#define PEAK (16.0*16.0*FREQ)
static int8_t A1[MAXN*MAXN], B1[MAXN*MAXN], C1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN], B2[MAXN*MAXN], C2[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  pthread_setaffinity_np(pthread_self(),sizeof(s),&s); sched_yield(); }

static void mm_old(const int8_t *A, const int8_t *B, int8_t *C){
  grt_matmul(&I8, A, B, C, SZ,SZ,SZ);
}
static void mm_new(const int8_t *A, const int8_t *B, int8_t *C){
  grt_work w = { .c=&I8, .A=A, .B=B, .C=C, .M=SZ,.N=SZ,.K=SZ };
  grt_matmul_r_i8(&w);
}

// 스레드는 측정 밖에서 한 번만 만든다 (E168의 1차 측정 결함).
// 동기화는 스핀 — 조건변수는 고정 1.2 ms를 먹어 비교가 성립하지 않는다 (E169).
static volatile int spin_go = 0, spin_done = 0, spin_quit = 0;
static volatile int use_new = 0;
static void *spin_worker(void *_){
  (void)_; pin(1);
  grt_flush_ctx(&I8);                       // 코어1 쪽 가속기도 깨워둔다
  for(;;){
    while (!__atomic_load_n(&spin_go, __ATOMIC_ACQUIRE)) { if (spin_quit) return NULL; }
    __atomic_store_n(&spin_go, 0, __ATOMIC_RELAXED);
    if (use_new) mm_new(A2,B2,C2); else mm_old(A2,B2,C2);
    __atomic_store_n(&spin_done, 1, __ATOMIC_RELEASE);
  }
}
static void par(void){
  __atomic_store_n(&spin_done, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&spin_go, 1, __ATOMIC_RELEASE);
  if (use_new) mm_new(A1,B1,C1); else mm_old(A1,B1,C1);
  while (!__atomic_load_n(&spin_done, __ATOMIC_ACQUIRE)) { }
}
static void seq(void){
  if (use_new) { mm_new(A1,B1,C1); mm_new(A2,B2,C2); }
  else         { mm_old(A1,B1,C1); mm_old(A2,B2,C2); }
}
static void one(void){ if (use_new) mm_new(A1,B1,C1); else mm_old(A1,B1,C1); }

static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    int8_t a = (c==r) || (c==(r+1)%SZ);
    int8_t b = (int8_t)((r*3+c*5)%7-3);
    A1[r*SZ+c]=a; B1[r*SZ+c]=b; A2[r*SZ+c]=a; B2[r*SZ+c]=b;
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (int8_t)(B1[r*SZ+c] + B1[((r+1)%SZ)*SZ+c]);
}
static int chk(int both){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    if(C1[r*SZ+c]!=REF[r*SZ+c]) b++;
    if(both && C2[r*SZ+c]!=REF[r*SZ+c]) b++;
  } return b; }
static double timeit(void (*fn)(void), int both, int *bad){
  double best=1e30; *bad=0;
  for(int s=0;s<5;s++){ memset(C1,0,(size_t)SZ*SZ); memset(C2,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; *bad+=chk(both); }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/twocore_r.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  pin(0);
  grt_flush_ctx(&I8);

  pthread_t th;
  if (pthread_create(&th,NULL,spin_worker,NULL)!=0){ mark("스레드 생성 실패"); exit(1); }
  struct timespec ts={0,200*1000*1000}; nanosleep(&ts,NULL);   // 워커가 코어1에 자리잡을 시간

  mark("=== 코어별 가속기 (d9, 40MHz, 코어마다 INT8 16x16) — 옛/새 스테퍼 (best-of-5) ===");
  mark("온라인 CPU=%ld, 주 스레드 cpu=%d", sysconf(_SC_NPROCESSORS_ONLN), sched_getcpu());
  mark("대조군(2i8f50, 단일 코어 두 가속기): 옛 1.90배 / 새 1.77배(256³)");
  mark("%5s %4s | %8s %7s | %8s %8s | %7s | %s",
       "크기","스텝","단독ms","활용%","순차ms","병렬ms","속도","정확성");

  int Ns[]={128,256};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    for(int nv=0; nv<2; nv++){
      use_new = nv;
      one(); seq(); par();                          /* 웜업 */
      int b0,b1,b2;
      double t1=timeit(one,0,&b0);
      double ts_=timeit(seq,1,&b1);
      double tp=timeit(par,1,&b2);
      mark("%4d³ %4s | %8.3f %6.1f%% | %8.3f %8.3f | %6.2fx | %s",
           SZ, nv?"새":"옛", t1*1e3, 100.0*((double)SZ*SZ*SZ/t1)/PEAK,
           ts_*1e3, tp*1e3, ts_/tp, (b0||b1||b2)?"FAIL":"PASS");
      if(b0||b1||b2) mark("     불일치: 단독=%d 순차=%d 병렬=%d", b0,b1,b2);
    }
  }
  __atomic_store_n(&spin_quit,1,__ATOMIC_RELEASE);
  __atomic_store_n(&spin_go,1,__ATOMIC_RELEASE);
  pthread_join(th,NULL);
  mark("=== TWOCORE_R_DONE ===");
  return 0;
}
