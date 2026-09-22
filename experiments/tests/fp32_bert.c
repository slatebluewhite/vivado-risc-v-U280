// fp32_bert.c — BERT-base 인코더 한 층의 행렬곱 전체를 실측한다 (E118 후속)
// E118은 어텐션만 재고 층 비용을 추정했다. 여기서는 층 전체를 실제로 돌린다.
//   QKV 투영 3회 : (128×768)·(768×768)
//   어텐션 12헤드 : QK^T (128×64)·(64×128), SV (128×128)·(128×64)
//   출력 투영     : (128×768)·(768×768)
//   FFN1          : (128×768)·(768×3072)
//   FFN2          : (128×3072)·(3072×768)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define SEQ 128
#define HID 768
#define FFN 3072
#define HEADS 12
#define HD (HID/HEADS)

static elem_t Abuf[SEQ][FFN] row_align(1);      // 1.5MB
static elem_t Bbuf[FFN][HID] row_align(1);      // 9.4MB
static elem_t Cbuf[SEQ][FFN] row_align(1);      // 1.5MB

static void mm(size_t M,size_t K,size_t N,size_t sA,size_t sB,size_t sC){
  tiled_matmul_auto(M,N,K,(elem_t*)Abuf,(elem_t*)Bbuf,NULL,(void*)Cbuf,
      sA,sB,sC,sC,
      MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
      NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
}
static unsigned long timed(size_t M,size_t K,size_t N,size_t sA,size_t sB,size_t sC,int it){
  mm(M,K,N,sA,sB,sC);
  unsigned long best=~0UL;
  for(int r=0;r<3;r++){
    unsigned long s=read_cycles();
    for(int t=0;t<it;t++) mm(M,K,N,sA,sB,sC);
    unsigned long e=read_cycles();
    unsigned long p=(e-s)/it; if(p<best) best=p;
  }
  return best;
}
int main(){
#ifndef BAREMETAL
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for(size_t i=0;i<SEQ;i++)for(size_t j=0;j<FFN;j++) Abuf[i][j]=(elem_t)(((int)((i+j)%7)-3)*0.125f);
  for(size_t i=0;i<FFN;i++)for(size_t j=0;j<HID;j++) Bbuf[i][j]=(elem_t)(((int)((i+j)%5)-2)*0.25f);

  printf("=== FP32 BERT-base 인코더 1층 (seq=%d, hid=%d, ffn=%d, heads=%d) ===\n",SEQ,HID,FFN,HEADS);
  fflush(stdout);

  unsigned long qkv  = timed(SEQ,HID,HID, FFN,HID,FFN, 3);
  unsigned long qk   = timed(SEQ, HD,SEQ, FFN,HID,FFN, 10);
  unsigned long sv   = timed(SEQ,SEQ, HD, FFN,HID,FFN, 10);
  unsigned long proj = qkv;                       // 출력 투영은 QKV 1회와 동일 형상
  unsigned long ffn1 = timed(SEQ,HID,FFN, FFN,HID,FFN, 2);
  unsigned long ffn2 = timed(SEQ,FFN,HID, FFN,HID,FFN, 2);

  unsigned long attn = (unsigned long)HEADS * (qk + sv);
  unsigned long total = 3*qkv + attn + proj + ffn1 + ffn2;

  printf("QKV 투영 x3   %7lu틱\n", 3*qkv);
  printf("어텐션 x%d헤드 %7lu틱  (QK^T %lu + SV %lu)\n", HEADS, attn, qk, sv);
  printf("출력 투영     %7lu틱\n", proj);
  printf("FFN1          %7lu틱\n", ffn1);
  printf("FFN2          %7lu틱\n", ffn2);
  printf("─────────────────────────\n");
  printf("층 합계       %7lu틱 = %.1f ms (31.25MHz)\n", total, total/312.5);
  // 층 전체 MAC 수
  double macs = 3.0*SEQ*HID*HID + (double)HEADS*(SEQ*HD*SEQ + SEQ*SEQ*HD)
              + (double)SEQ*HID*HID + (double)SEQ*HID*FFN + (double)SEQ*FFN*HID;
  printf("층 MAC %.1fM, 평균 %.1f MAC/사이클, 활용률 %.1f%%\n",
         macs/1e6, macs/(total*100.0), 100.0*macs/(total*100.0)/(DIM*DIM));
  printf("=== BERT_DONE ===\n"); fflush(stdout);
  return 0;
}
