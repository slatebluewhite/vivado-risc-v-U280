// prec_cmp.c — INT8과 FP32의 **수치 오차**를 같은 데이터·같은 코어에서 비교한다 (E142).
//
// 왜 필요한가:
//   E139~E141로 정밀도의 **비용**은 나왔다(작은 행렬 1.83배, 큰 행렬 4.4배).
//   그런데 **이득**을 모르면 판단할 수 없다. INT8이 실제로 얼마나 틀리는가?
//
// 방법: 같은 실수 행렬에서 출발한다.
//   기준: CPU double로 계산 (오라클)
//   FP32: 그대로 가속기에 넣는다
//   INT8: 대칭 양자화(스케일 = max|x|/127)해서 넣고, 결과를 역양자화한다
//   두 결과의 상대 오차를 비교한다.
//
// 주의: INT8 누산은 32비트지만 mvout 스케일링이 Float(8,24)를 거치므로 유효 24비트다.
//   K가 크면 그 한계에 닿을 수 있으므로 K도 함께 훑는다.
//
// 헤더를 바꿔 INT8판/FP32판 두 바이너리로 빌드한다. 오라클과 비교 로직은 동일하다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include "include/gemmini_testutils.h"

#define MAXN 128
static elem_t Am[MAXN*MAXN] row_align(1);
static elem_t Bm[MAXN*MAXN] row_align(1);
static acc_t  Cm[MAXN*MAXN] row_align_acc(1);   // 전폭 누산값을 받는다(full_C=true)
static double Ad[MAXN*MAXN], Bd[MAXN*MAXN], Ref[MAXN*MAXN];

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}

// 재현 가능한 의사난수 (Math.random 없이)
static unsigned long rs = 12345;
static double nextv(void){ rs = rs*6364136223846793005UL + 1442695040888963407UL;
  return ((double)((rs >> 33) & 0xFFFFFF) / 8388608.0) - 1.0; }   // 대략 [-1,1)

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/prec.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);

  mark("=== 정밀도 비교: DIM=%d elem=%zuB opcode=custom%d ===", DIM, sizeof(elem_t), XCUSTOM_ACC);
  mark("%5s %14s %14s", "N(=K)", "상대오차 평균", "상대오차 최대");

  int sizes[] = {32, 64, 128};
  for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); si++){
    int n = sizes[si];
    rs = 12345;                                  // 두 바이너리가 같은 데이터를 쓰도록 고정
    double amax=0, bmax=0;
    for (int i=0;i<n*n;i++){ Ad[i]=nextv(); if (fabs(Ad[i])>amax) amax=fabs(Ad[i]); }
    for (int i=0;i<n*n;i++){ Bd[i]=nextv(); if (fabs(Bd[i])>bmax) bmax=fabs(Bd[i]); }

    // 오라클: double로 직접 계산
    for (int i=0;i<n;i++) for(int j=0;j<n;j++){
      double acc=0; for(int k=0;k<n;k++) acc += Ad[i*n+k]*Bd[k*n+j];
      Ref[i*n+j]=acc;
    }

    double sa = amax/127.0, sb = bmax/127.0;     // INT8 대칭 양자화 스케일
    int is_int = (sizeof(elem_t) == 1);
    for (int i=0;i<n*n;i++){
      if (is_int){
        double qa = Ad[i]/sa, qb = Bd[i]/sb;
        long ra = lrint(qa), rb = lrint(qb);
        if (ra>127) ra=127; if (ra<-128) ra=-128;
        if (rb>127) rb=127; if (rb<-128) rb=-128;
        ((int8_t*)Am)[i] = (int8_t)ra;  ((int8_t*)Bm)[i] = (int8_t)rb;
      } else {
        ((float*)Am)[i] = (float)Ad[i]; ((float*)Bm)[i] = (float)Bd[i];
      }
    }
    memset(Cm,0,(size_t)n*n*sizeof(acc_t));

    // full_C=true (뒤에서 네 번째 인자)로 누산기 전폭(acc_t)을 그대로 받는다.
    // false로 두면 결과가 elem_t로 좁혀지며 INT8에서 ±127로 포화해 값이 무의미해진다
    // — 첫 시도에서 실제로 그렇게 재서 평균 오차 99%가 나왔다(E142).
    tiled_matmul_auto(n,n,n, Am,Bm,NULL,Cm, n,n,n,n,
        MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
        NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,/*full_C=*/true,false,0,WS);

    double sum=0, mx=0; int cnt=0;
    for (int i=0;i<n*n;i++){
      double got = is_int ? (double)((int32_t*)Cm)[i] * sa * sb   // acc_t = int32
                          : (double)((float*)Cm)[i];                // acc_t = float
      double want = Ref[i];
      double den = fabs(want) > 1e-9 ? fabs(want) : 1e-9;
      double rel = fabs(got-want)/den;
      sum += rel; if (rel>mx) mx=rel; cnt++;
    }
    mark("%5d %13.4f%% %13.2f%%", n, sum/cnt*100.0, mx*100.0);
  }
  mark("=== PREC_DONE ===");
  return 0;
}
