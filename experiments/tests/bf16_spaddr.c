// bf16_spaddr.c — 라이브러리가 B를 두는 "높은 주소"가 정상인지 확인한다 (E104 후속)
// sp_tiled_matmul_ws: B_sp_addr_start = BANK_NUM*BANK_ROWS - K*J*DIM
// 1타일이면 16384-8 = 16376. 이 영역이 죽어 있으면 B가 0이 되어 곱이 전부 0이 된다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"
static elem_t X[DIM][DIM] row_align(1), Y[DIM][DIM] row_align(1);
static inline float b2f(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f2b(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,x=0x7fffu+l;return (uint16_t)((c.u+x)>>16);}

static void rt(uint32_t addr, const char *tag) {
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) Y[i][j]=f2b(9.0f);
  gemmini_mvin(X, addr);
  gemmini_fence();
  gemmini_mvout(Y, addr);
  gemmini_fence();
  size_t ok=0;
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) if (b2f(Y[i][j])==7.0f) ok++;
  printf("%-28s addr=%u  일치 %lu/%d  Y[0][0]=%.1f\n", tag, addr, (unsigned long)ok, DIM*DIM, b2f(Y[0][0]));
  fflush(stdout);
}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) X[i][j]=f2b(7.0f);
  gemmini_config_ld(DIM*sizeof(elem_t)); gemmini_config_st(DIM*sizeof(elem_t));
  printf("=== BF16 스크래치패드 주소 시험 (BANK_NUM*BANK_ROWS=%d) ===\n", BANK_NUM*BANK_ROWS);
  fflush(stdout);
  rt(0,                              "낮은 주소 0");
  rt(BANK_ROWS,                      "뱅크1 시작");
  rt(2*BANK_ROWS,                    "뱅크2 시작");
  rt(3*BANK_ROWS,                    "뱅크3 시작");
  rt(BANK_NUM*BANK_ROWS - DIM,       "★라이브러리 B 위치(최상단)");
  printf("=== SPADDR_DONE ===\n"); fflush(stdout);
  return 0;
}
