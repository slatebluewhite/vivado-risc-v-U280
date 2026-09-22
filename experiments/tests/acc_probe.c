// acc_probe.c — 누산기 경로가 "0을 내는지" "쓰레기를 내는지" 구분한다 (E60 후속)
//
// bf16_verify가 상대오차 정확히 100%를 냈다 = 결과가 전부 0.
// 그런데 그것이 (a) 누산기 읽기 자체가 데이터를 못 내보내는 것인지
// (b) 스케일 경로가 0을 곱하는 것인지 아직 구분되지 않았다.
//
// 연산을 전혀 하지 않고 누산기에 넣었다 빼기만 해서, 값이 어떻게 나오는지 직접 본다.
// 계산이 없으므로 결과는 입력과 같아야 한다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

static void mark(int n, const char *what) {
  printf("A%d: %s\n", n, what);
  fflush(stdout);
  for (volatile int i = 0; i < 200000; i++) ;
}

static elem_t In[DIM][DIM] row_align(1);
static elem_t Out[DIM][DIM] row_align(1);
static acc_t  OutAcc[DIM][DIM] row_align_acc(1);

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
  printf("=== ACC PROBE (DIM=%d) ===\n", DIM);
  fflush(stdout);

  // 입력을 뚜렷이 구분되는 비트패턴으로 채운다.
  // 0이 나오면 "데이터 부재", 입력과 같으면 정상, 그 외면 스케일 경로 문제.
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) {
      In[i][j]  = (elem_t)(0x3f80 + i);   // BF16이면 1.0 근처, INT8이면 그냥 상수
      Out[i][j] = 0;
      OutAcc[i][j] = 0;
    }

  gemmini_flush(0);
  const uint32_t acc_addr = (uint32_t)1 << (ADDR_LEN - 1);

  mark(1, "config");
  gemmini_config_ld(DIM * sizeof(elem_t));
  gemmini_config_st(DIM * sizeof(elem_t));

  mark(2, "mvin -> accumulator");
  gemmini_mvin(In, acc_addr);
  gemmini_fence();

  mark(3, "mvout <- accumulator (elem_t 폭)");
  gemmini_mvout(Out, acc_addr);
  gemmini_fence();

  mark(4, "done, 값 출력");

  // 입력 / 출력 첫 행을 비트로 찍는다. 눈으로 바로 구분된다.
  printf("IN [0][0..3] = %04x %04x %04x %04x\n",
         (unsigned)In[0][0], (unsigned)In[0][1], (unsigned)In[0][2], (unsigned)In[0][3]);
  printf("OUT[0][0..3] = %04x %04x %04x %04x\n",
         (unsigned)Out[0][0], (unsigned)Out[0][1], (unsigned)Out[0][2], (unsigned)Out[0][3]);

  for (size_t i = 0; i < DIM; i++) {
    size_t rs = 0;
    for (size_t j = 0; j < DIM; j++) if (Out[i][j] == In[i][j]) rs++;
    printf("ROW %lu: match=%lu/%lu  in=%04x out=%04x\n",
           (unsigned long)i, (unsigned long)rs, (unsigned long)DIM,
           (unsigned)In[i][0], (unsigned)Out[i][0]);
  }
  fflush(stdout);

  size_t zeros = 0, same = 0;
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) {
      if (Out[i][j] == 0) zeros++;
      if (Out[i][j] == In[i][j]) same++;
    }
  printf("ACC_RESULT zeros=%lu same=%lu total=%lu\n",
         (unsigned long)zeros, (unsigned long)same, (unsigned long)(DIM * DIM));

  if (same == DIM * DIM)      printf("ACC_VERDICT ROUNDTRIP_OK\n");
  else if (zeros == DIM * DIM) printf("ACC_VERDICT ALL_ZERO (데이터가 안 나옴)\n");
  else                         printf("ACC_VERDICT GARBAGE (경로는 사는데 값이 틀림)\n");
  fflush(stdout);
  return 0;
}
