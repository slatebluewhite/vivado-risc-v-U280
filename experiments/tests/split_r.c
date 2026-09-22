// split_r.c — 기준선 활용률을 올린 뒤에도 두 가속기의 이득이 남는가 (E172).
//
// E171에서 단일 matmul 행 분할이 1.6~2.0배로 나왔지만, 그때의 단일 가속기 기준선은
// **활용률이 12%뿐**이었다. 가속기가 88% 놀고 있으니 두 번째 가속기가 채울 여유가
// 컸던 것이고, 효율적인 기준선에서는 이득이 줄어들 것으로 예상해야 한다.
//
// 그래서 재사용 스테퍼(gemmini_rt.h의 GRT_DEFINE_STEP_R)를 만들었다:
//   - 루프 순서 i,k,j 로 바꿔 A 타일을 j 전체에 재사용 (A의 mvin이 N/dim분의 1)
//   - B를 두 주소에 번갈아 실어 WAR을 끊고 mvin과 compute를 겹칠 수 있게
//   타일 연산당 명령 6개 → 3개.
//
// 네 가지를 **같은 바이너리·같은 데이터**로 나란히 잰다:
//   단일 옛 / 단일 재사용 / 분할 옛 / 분할 재사용
// 핵심 질문은 "단일 기준선이 빨라진 만큼 분할 이득이 깎이는가"다.
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

// 2i8f50: 한 코어에 INT8 16x16 두 개 (custom3, custom2)
static const grt_ctx AC1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx AC2 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define MAXN 256
#define FREQ 50.0e6            // Rocket64b2gem2i8f50
#define PEAK (16.0*16.0*FREQ)  // MAC/s (메쉬가 꽉 찼을 때)

static int8_t A[MAXN*MAXN], B[MAXN*MAXN], C[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t REF[MAXN*MAXN];
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void whole_old(void){ grt_matmul(&AC1, A, B, C, SZ,SZ,SZ); }
static void whole_new(void){
  grt_work w = { .c=&AC1, .A=A, .B=B, .C=C, .M=SZ,.N=SZ,.K=SZ };
  grt_matmul_r_i8(&w);
}
static void split_old(void){
  const int half=SZ/2;
  grt_work w1 = { .c=&AC1, .A=A,                 .B=B, .C=C,                 .M=half,.N=SZ,.K=SZ };
  grt_work w2 = { .c=&AC2, .A=A+(size_t)half*SZ, .B=B, .C=C+(size_t)half*SZ, .M=half,.N=SZ,.K=SZ };
  grt_mm2_i8i8(&w1, &w2);
}
static void split_new(void){
  const int half=SZ/2;
  grt_work w1 = { .c=&AC1, .A=A,                 .B=B, .C=C,                 .M=half,.N=SZ,.K=SZ };
  grt_work w2 = { .c=&AC2, .A=A+(size_t)half*SZ, .B=B, .C=C+(size_t)half*SZ, .M=half,.N=SZ,.K=SZ };
  grt_mm2_r_i8i8(&w1, &w2);
}

// A는 대각 + 한 칸 옆(순환) 두 개만 1 → C[i][j] = B[i][j] + B[(i+1)%SZ][j].
// int8 포화를 피하면서도 k 방향 누산과 **타일 경계를 넘는 기여**를 함께 검사한다.
// (단순 항등 행렬이면 누산기 주소가 틀려도 통과할 여지가 있다.)
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A[r*SZ+c] = (c==r) || (c==(r+1)%SZ);
    B[r*SZ+c] = (int8_t)((r*3+c*5)%7-3);
  }
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++)
    REF[r*SZ+c] = (int8_t)(B[r*SZ+c] + B[((r+1)%SZ)*SZ+c]);
}
static int chk(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++) if(C[r*SZ+c]!=REF[r*SZ+c]) b++; return b; }

static double timeit(void (*fn)(void), int *bad){
  double best=1e30; *bad=0;
  for(int s=0;s<5;s++){ memset(C,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt;
    *bad += chk(); }
  return best;
}
static double util(double t){ return 100.0*((double)SZ*SZ*SZ/t)/PEAK; }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/split_r.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&AC1); grt_flush_ctx(&AC2);

  mark("=== 재사용 스테퍼로 기준선을 올린 뒤의 분할 이득 (cpu=%d, best-of-5) ===", sched_getcpu());
  mark("가속기 두 개 모두 INT8 16x16 @ 50MHz, 피크 %.2f GMAC/s", PEAK/1e9);
  mark("%5s | %8s %6s | %8s %6s | %6s | %8s %8s | %6s %6s | %s",
       "크기","단일옛ms","활용%","단일새ms","활용%","개선",
       "분할옛ms","분할새ms","이득옛","이득새","정확성");

  int Ns[]={64,96,128,160,192,224,256};
  for(unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    whole_old(); whole_new(); split_old(); split_new();   /* 웜업 */
    int b1,b2,b3,b4;
    double to=timeit(whole_old,&b1);
    double tn=timeit(whole_new,&b2);
    double so=timeit(split_old,&b3);
    double sn=timeit(split_new,&b4);
    mark("%4d³ | %8.3f %5.1f%% | %8.3f %5.1f%% | %5.2fx | %8.3f %8.3f | %5.2fx %5.2fx | %s",
         SZ, to*1e3, util(to), tn*1e3, util(tn), to/tn,
         so*1e3, sn*1e3, to/so, tn/sn,
         (b1||b2||b3||b4)?"FAIL":"PASS");
    if(b1||b2||b3||b4)
      mark("     불일치 원소수: 단일옛=%d 단일새=%d 분할옛=%d 분할새=%d", b1,b2,b3,b4);
  }
  mark("=== SPLIT_R_DONE ===");
  return 0;
}
