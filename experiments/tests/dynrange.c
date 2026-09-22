// dynrange.c — **동적 범위**가 INT8을 무너뜨리는 지점을 찾는다 (E143).
//
// 왜:
//   E142에서 INT8의 누적 오차는 2.1~4.6%에 그쳤다. 즉 "정밀도 때문에 FP32"라는 논리는 약하다.
//   FP32의 진짜 가치는 **동적 범위**에 있다는 것이 E142의 결론이었는데, 그건 주장일 뿐
//   측정된 적이 없다. 여기서 잰다.
//
//   대칭 양자화는 스케일 = max|x|/127이다. 따라서 **이상치 하나가 스케일을 끌어올리면
//   나머지 값들이 표현할 수 있는 단계 수가 줄어든다.** 이것이 LLM 양자화의 알려진 난점이다.
//
// 방법: 값의 1%를 배율 R로 키우고 R을 훑는다. R=1이 E142와 같은 조건이다.
//   예측: INT8 오차는 R에 대략 비례해 커지고, FP32는 변하지 않는다.
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

#define N 64
static elem_t Am[N*N] row_align(1);
static elem_t Bm[N*N] row_align(1);
static acc_t  Cm[N*N] row_align_acc(1);
static double Ad[N*N], Bd[N*N], Ref[N*N];

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static unsigned long rs;
static double nextv(void){ rs = rs*6364136223846793005UL + 1442695040888963407UL;
  return ((double)((rs >> 33) & 0xFFFFFF) / 8388608.0) - 1.0; }

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/dynrange.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  gemmini_flush(0);

  int is_int = (sizeof(elem_t) == 1);
  mark("=== 동적 범위 훑기: DIM=%d elem=%zuB opcode=custom%d, N=K=%d ===",
       DIM, sizeof(elem_t), XCUSTOM_ACC, N);
  mark("값의 1%%를 배율 R로 키운다. 대칭 양자화 스케일 = max|x|/127.");
  mark("%8s %14s %14s", "R", "상대오차 평균", "0으로 뭉갬");

  double Rs[] = {1, 10, 100, 1000};
  for (unsigned ri=0; ri<sizeof(Rs)/sizeof(Rs[0]); ri++){
    double Rv = Rs[ri];
    rs = 12345;                                   // 두 바이너리가 동일 데이터를 쓰게 고정
    for (int i=0;i<N*N;i++) Ad[i]=nextv();
    for (int i=0;i<N*N;i++) Bd[i]=nextv();
    // 1%를 이상치로 (결정론적 위치)
    for (int i=0;i<N*N;i+=100){ Ad[i]*=Rv; Bd[i]*=Rv; }

    double amax=0,bmax=0;
    for (int i=0;i<N*N;i++){ if(fabs(Ad[i])>amax)amax=fabs(Ad[i]); if(fabs(Bd[i])>bmax)bmax=fabs(Bd[i]); }

    for (int i=0;i<N;i++) for(int j=0;j<N;j++){
      double acc=0; for(int k=0;k<N;k++) acc += Ad[i*N+k]*Bd[k*N+j];
      Ref[i*N+j]=acc;
    }

    double sa=amax/127.0, sb=bmax/127.0;
    long zeroed=0;
    for (int i=0;i<N*N;i++){
      if (is_int){
        long ra=lrint(Ad[i]/sa), rb=lrint(Bd[i]/sb);
        if(ra>127)ra=127; if(ra<-128)ra=-128;
        if(rb>127)rb=127; if(rb<-128)rb=-128;
        if (ra==0 && Ad[i]!=0.0) zeroed++;         // 0으로 뭉개진 원소 수
        ((int8_t*)Am)[i]=(int8_t)ra; ((int8_t*)Bm)[i]=(int8_t)rb;
      } else {
        ((float*)Am)[i]=(float)Ad[i]; ((float*)Bm)[i]=(float)Bd[i];
      }
    }
    memset(Cm,0,sizeof(Cm));
    tiled_matmul_auto(N,N,N, Am,Bm,NULL,Cm, N,N,N,N,
        MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
        NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,/*full_C=*/true,false,0,WS);

    // 상대오차는 0 근처 분모에서 폭발하므로, 출력 RMS로 정규화한다 (E142의 교훈)
    double rms=0; for (int i=0;i<N*N;i++) rms += Ref[i]*Ref[i];
    rms = sqrt(rms/(N*N));
    double sum=0;
    for (int i=0;i<N*N;i++){
      double got = is_int ? (double)((int32_t*)Cm)[i]*sa*sb : (double)((float*)Cm)[i];
      sum += fabs(got-Ref[i]);
    }
    mark("%8.0f %13.4f%% %9ld/%d", Rv, sum/(N*N)/rms*100.0, zeroed, N*N);
  }
  mark("=== DYN_DONE ===");
  return 0;
}
