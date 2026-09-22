// indep_r.c — 단일 코어 두 가속기를 d9와 **정확히 같은 형태**로 잰다 (E174 유보 2 해소).
//
// E174는 두 코어(독립 matmul 두 개)와 단일 코어(하나의 matmul을 행 분할)를 비교했다.
// 작업 형태가 달라 ±5%쯤 무른 비교였다. 여기서는 단일 코어에서도 **독립 matmul 두 개**를
// 재사용 스테퍼로 교차 발행해, d9의 twocore_r.c와 같은 항목을 같은 방식으로 만든다.
//
// 옛 스테퍼도 함께 재서 E166/E168의 1.90배가 재현되는지 확인한다(기준선 검증).
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

// 2i8f50: 한 코어에 INT8 16x16 두 개
static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx AC2 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define MAXN 256
#define FREQ 50.0e6
#define PEAK (16.0*16.0*FREQ)
static int8_t A1[MAXN*MAXN], B1[MAXN*MAXN], C1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN], B2[MAXN*MAXN], C2[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ, USE_NEW;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 재사용 스테퍼로 custom2 쪽 단일 matmul (헤더에는 custom3용만 있다)
static void mm_r_ac2(const int8_t *A, const int8_t *B, int8_t *C){
  grt_work w = { .c=&AC2, .A=A, .B=B, .C=C, .M=SZ,.N=SZ,.K=SZ };
  grt_cursor s = {0,0,0,0,0};
  grt_config_ex(w.c, GRT_WS);
  while (!s.done) grt_step_r_i8b(&w, &s);
  grt_fence();
}
static void mm_r_ac1(const int8_t *A, const int8_t *B, int8_t *C){
  grt_work w = { .c=&AC1, .A=A, .B=B, .C=C, .M=SZ,.N=SZ,.K=SZ };
  grt_matmul_r_i8(&w);
}

static void one(void){
  if (USE_NEW) mm_r_ac1(A1,B1,C1); else grt_matmul(&AC1,A1,B1,C1,SZ,SZ,SZ);
}
static void seq(void){                       // 한 코어, 두 가속기를 **순차**로
  if (USE_NEW) { mm_r_ac1(A1,B1,C1); mm_r_ac2(A2,B2,C2); }
  else { grt_matmul(&AC1,A1,B1,C1,SZ,SZ,SZ); grt_matmul(&AC2,A2,B2,C2,SZ,SZ,SZ); }
}
static void par(void){                       // 한 코어, 두 가속기를 **타일 단위 교대**
  grt_work w1 = { .c=&AC1, .A=A1, .B=B1, .C=C1, .M=SZ,.N=SZ,.K=SZ };
  grt_work w2 = { .c=&AC2, .A=A2, .B=B2, .C=C2, .M=SZ,.N=SZ,.K=SZ };
  if (USE_NEW) grt_mm2_r_i8i8(&w1,&w2); else grt_mm2_i8i8(&w1,&w2);
}

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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/indep_r.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); grt_flush_ctx(&AC2);

  mark("=== 단일 코어 두 가속기, 독립 matmul 두 개 (2i8f50, 50MHz, best-of-5) ===");
  mark("d9(두 코어)와 같은 항목: 옛 1.89~1.96배 / 새 1.95~2.02배");
  mark("%5s %4s | %8s %7s | %8s %8s | %7s | %s",
       "크기","스텝","단독ms","활용%","순차ms","교대ms","속도","정확성");

  /* 256³은 배열 6개+REF로 448 KB — 512 KB L2 문턱에 걸터앉아 회차마다 값이 뒤집혔다.
     교대 발행은 두 작업셋을 동시에 살려두므로 순차보다 먼저 넘친다. 문턱 아래로 내린다. */
  int Ns[]={64,96,128,160};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    for(int nv=0; nv<2; nv++){
      USE_NEW = nv;
      one(); seq(); par(); one(); seq(); par(); one(); seq(); par();  /* 웜업 3회 */
      int b0,b1,b2;
      double t1=timeit(one,0,&b0);
      double ts_=timeit(seq,1,&b1);
      double tp=timeit(par,1,&b2);
      mark("%4d³ %4s | %8.3f %6.1f%% | %8.3f %8.3f | %6.2fx | %s",
           SZ, nv?"새":"옛", t1*1e3, 100.0*((double)SZ*SZ*SZ/t1)/PEAK,
           ts_*1e3, tp*1e3, ts_/tp, (b0||b1||b2)?"FAIL":"PASS");
      if(b0||b1||b2) mark("     불일치: 단독=%d 순차=%d 교대=%d", b0,b1,b2);
    }
  }
  mark("=== INDEP_R_DONE ===");
  return 0;
}
