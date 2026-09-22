// accscale.c — E320 준비: mvout 스케일 경로(AccumulatorScale)를 직접 때리는 정확성 시험.
//
// E314가 num_scale_units를 -1(조합)에서 16(공유 유닛)으로 바꿨다. 그 코드 경로는
// 업스트림에서 정규화 구성으로만 쓰였고 내가 세 군데를 고쳤다. 기존 시험들은 A가
// 희소 선택행렬이라 누산값이 |6| 이하로 작아 **포화도 곱셈도 안 때린다.**
//
// 여기서는 조밀 A·B로 누산값을 ±4096까지 키우고 스케일을 바꿔 가며:
//   scale=1.0    -> 대부분 ±127로 포화 (클리핑 경로)
//   scale=1/32   -> 대부분 범위 안 (곱셈·반올림 경로)
//   scale=1/256  -> 작은 값 (반올림 경계)
// CPU 참조: int32 합 -> float 곱 -> round-half-even -> [-128,127] 포화.
//
// 먼저 정상 보드(3가속기 50MHz)에서 통과해야 한다 — 시험 자체를 먼저 검증한다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"
static const grt_ctx AC[2] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
};
#define MM 128
#define KK 256
#define NN 256
#define BI 8
#define BJ 4
#define KCV 8
static int8_t A[MM*KK] __attribute__((aligned(64)));
static int8_t B[KK*NN] __attribute__((aligned(64)));
static int8_t C[MM*NN] __attribute__((aligned(64)));
static int8_t R[MM*NN];
static int32_t ACC[MM*NN];
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }

// grt_config_st는 스케일을 1.0으로 고정한다. 스케일을 바꾸려면 같은 인코딩으로 직접 낸다.
static inline void cfg_st_scale(const grt_ctx *c, uint64_t stride_bytes, uint32_t scale_bits) {
  GRT_INSN_C(c, (0ull << 2) | GRT_CONFIG_ST,
             ((uint64_t)scale_bits << 32) | (uint32_t)stride_bytes, GRT_k_CONFIG);
}
static void cfg(int m, uint32_t sc){
  for(int a=0;a<m;a++){
    grt_config_ex(&AC[a], GRT_WS);
    cfg_st_scale(&AC[a], NN, sc);
    grt_config_ld_id(&AC[a], KK, 0);
    grt_config_ld_id(&AC[a], NN, 1);
    grt_config_ld_id(&AC[a], 0, 2);
  }
}
static void run(int m, uint32_t sc){
  cfg(m, sc);
  int TI=MM/16, Ntil=NN/16, NB=(Ntil+BJ-1)/BJ;
  int i0[2]={0,0}, jb[2], done[2];
  for(int a=0;a<m;a++){ jb[a]=a; done[a]=(a>=NB); }
  int left; do{ left=0;
    for(int a=0;a<m;a++){ if(done[a]) continue;
      int j0=jb[a]*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
      int I=(i0[a]+BI<=TI)?BI:(TI-i0[a]);
      grt_block_ksplit(&AC[a], I,J,KK/16,KCV,
                       A+(size_t)i0[a]*16*KK, B+(size_t)j0*16,
                       C+(size_t)i0[a]*16*NN+(size_t)j0*16, KK,NN,NN, 1);
      i0[a]+=BI;
      if(i0[a]>=TI){ i0[a]=0; jb[a]+=m; if(jb[a]>=NB) done[a]=1; }
      if(!done[a]) left++; }
  }while(left);
  grt_fence();
}
static void fill(void){
  for(int r=0;r<MM;r++) for(int c=0;c<KK;c++) A[(size_t)r*KK+c]=(int8_t)((r*7+c*3)%9-4);
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++) B[(size_t)r*NN+c]=(int8_t)((r*5+c*11)%9-4);
  for(int r=0;r<MM;r++) for(int c=0;c<NN;c++){
    int32_t s=0;
    for(int k=0;k<KK;k++) s += (int32_t)A[(size_t)r*KK+k]*(int32_t)B[(size_t)k*NN+c];
    ACC[(size_t)r*NN+c]=s; }
}
static void ref(float sc){
  for(size_t i=0;i<(size_t)MM*NN;i++){
    double v=rint((double)ACC[i]*(double)sc);      // round-half-even
    if(v>127) v=127; if(v<-128) v=-128;
    R[i]=(int8_t)v; }
}
static int chk(int *sat,int *maxdiff){
  int bad=0; *sat=0; *maxdiff=0;
  for(size_t i=0;i<(size_t)MM*NN;i++){
    if(R[i]==127||R[i]==-128) (*sat)++;
    int d=C[i]-R[i]; if(d<0)d=-d;
    if(d){ bad++; if(d>*maxdiff)*maxdiff=d; } }
  return bad;
}
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/accscale.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<2;a++) grt_flush_ctx(&AC[a]);
  mark("=== mvout 스케일 경로 정확성 (조밀 A·B, [%dx%d]x[%dx%d], 블록 (%d,%d) Kc=%d) ===",MM,KK,KK,NN,BI,BJ,KCV);
  fill();
  int32_t lo=ACC[0],hi=ACC[0];
  for(size_t i=0;i<(size_t)MM*NN;i++){ if(ACC[i]<lo)lo=ACC[i]; if(ACC[i]>hi)hi=ACC[i]; }
  mark("누산값 범위 %d ~ %d  (int8 범위 밖이면 포화 경로를 탄다)",lo,hi);
  mark("%-12s %10s | %8s %8s %8s | %s","스케일","비트","포화개수","불일치","최대차","판정");
  // 1/4은 포화와 비포화가 섞이는 지점(범위 -133..67) — 클리핑·곱셈·반올림을 동시에 때린다.
  struct { const char*nm; uint32_t bits; float f; } SC[]={
    {"1.0",     0x3f800000u, 1.0f},
    {"1/2",     0x3f000000u, 0.5f},
    {"1/4",     0x3e800000u, 0.25f},
    {"1/32",    0x3d000000u, 1.0f/32},
    {"1/256",   0x3b800000u, 1.0f/256},
  };
  for(unsigned i=0;i<sizeof(SC)/sizeof(SC[0]);i++){
    for(int m=1;m<=2;m++){
      memset(C,0,sizeof C);
      run(m, SC[i].bits);
      ref(SC[i].f);
      int sat,md; int bad=chk(&sat,&md);
      mark("%-12s 0x%08x m=%d | %8d %8d %8d | %s",SC[i].nm,SC[i].bits,m,sat,bad,md,bad?"FAIL":"PASS");
    }
  }
  mark("=== ACCSCALE_DONE ===");
  return 0; }
