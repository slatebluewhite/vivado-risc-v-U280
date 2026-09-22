// split_mm.c — **하나의 matmul**을 두 가속기에 쪼개 넣는다 (E170).
//
// 지금까지(E156~E169)는 **독립적인 matmul 두 개**를 겹쳐 돌린 것이다.
// 실용적으로 더 중요한 질문은 "**단일 matmul** 하나를 두 어레이가 나눠 계산할 수 있는가"다
// — 응용을 고칠 필요가 없기 때문이다.
//
// 분할 축:
//   K 축(부분합)  → 부분합 덧셈이 호스트 몫. E151에서 호스트 원소 작업이 치명적임을 확인. 배제.
//   출력 타일 분할 → 통신 없음. 다만 **한쪽 피연산자를 양쪽이 중복 적재**한다.
//     C를 행으로 나누면: 가속기1 = A 위쪽 절반 + B **전체**, 가속기2 = A 아래쪽 + B **전체**
//     → B의 DMA가 두 배. E158에서 상한이 온칩 패브릭 대역폭이었으므로 이득이 깎일 수 있다.
//
// 비교: 단일 가속기가 전체를 계산 vs 두 가속기가 행으로 나눠 계산. 결과는 동일해야 한다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

// d9는 코어당 INT8 16x16 하나뿐이라 이 실험은 2i8f50(한 코어에 둘)에서 돈다
static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx AC2 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define MAXN 256
static int8_t A[MAXN*MAXN], B[MAXN*MAXN], C[MAXN*MAXN] __attribute__((aligned(64)));
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 단일 가속기가 전체를 계산
static void whole(void){ grt_matmul(&AC1, A, B, C, SZ,SZ,SZ); }

// 두 가속기가 **출력 행**을 절반씩 — C의 위쪽은 가속기1, 아래쪽은 가속기2.
// A는 각자 자기 행만, B는 **둘 다 전체**를 읽는다(중복).
static void split(void){
  const int d=16, half=SZ/2;
  grt_work w1 = { .c=&AC1, .A=A,                 .B=B, .C=C,                 .M=half,.N=SZ,.K=SZ };
  grt_work w2 = { .c=&AC2, .A=A+(size_t)half*SZ, .B=B, .C=C+(size_t)half*SZ, .M=half,.N=SZ,.K=SZ };
  (void)d;
  grt_mm2_i8i8(&w1, &w2);
}
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c]=(r==c)?1:0; B[r*SZ+c]=(int8_t)((r+c)%7-3); }
}
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=B[r*SZ+c]) b++; return b; }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(C,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/split.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); grt_flush_ctx(&AC2);

  mark("=== 단일 matmul을 두 가속기에 행 분할 (cpu=%d, best-of-5) ===", sched_getcpu());
  mark("대조: 독립 matmul 두 개 겹치기는 1.79~1.91배 (중복 적재 없음)");
  mark("주: grt_step은 타일마다 A/B를 다시 적재하므로 행 분할로 적재량이 늘지 않는다(E170)");
  mark("%7s %9s %9s %9s %8s %8s", "크기", "단일ms", "단일재", "분할ms", "속도", "정확성");
  /* E170의 192³ 이상치 확인: 점을 촌촌히 찍고, 크기별로 웜업을 먼저 돌리며,
     단일을 분할 전후로 두 번 재서 드리프트를 본다.
     (콜드 스타트라면 먼저 재는 whole이 느려져 비가 **부풀려야** 하는데
      192³은 오히려 낮게 나왔으므로 다른 원인이다.) */
  int Ns[]={64,96,128,160,192,224,256};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    whole(); split();                      /* 웜업 */
    double tw=timeit(whole); int b1=chk();
    double td=timeit(split); int b2=chk();
    double tw2=timeit(whole);              /* 드리프트 확인 */
    double tb=(tw<tw2)?tw:tw2;
    mark("%6d³ %9.3f %9.3f %9.3f %7.2f배 %8s", SZ, tw*1e3, tw2*1e3, td*1e3,
         tb/td, (b1||b2)?"FAIL":"PASS");
  }
  mark("=== SPLIT_DONE ===");
  return 0;
}
