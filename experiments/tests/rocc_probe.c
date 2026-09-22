// rocc_probe.c — BF16 행(hang)의 위치를 명령 단위로 특정한다 (E42 후속)
//
// mvin_mvout이 시스템 전체를 교착시키므로, 어느 RoCC 명령이 버스를 무는지
// 한 단계씩 진행하며 각 단계 직전에 표식을 출력하고 즉시 flush 한다.
// 콘솔에 마지막으로 찍힌 STEP 번호가 곧 범인이다.
//
// 주의: 행이 나면 시스템이 죽으므로 출력이 버퍼에 남으면 안 된다.
// 매 단계마다 fflush(stdout) + 짧은 지연으로 UART가 실제로 비워지게 한다.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

static void mark(int n, const char *what) {
  printf("STEP %d: %s\n", n, what);
  fflush(stdout);
  // UART가 실제로 문자를 내보낼 시간을 준다 (31.25MHz, 115200bps)
  for (volatile int i = 0; i < 200000; i++) ;
}

static elem_t In[DIM][DIM] row_align(1);
static elem_t Out[DIM][DIM] row_align(1);

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("=== ROCC PROBE start (DIM=%d) ===\n", DIM);
  fflush(stdout);

  // 데이터 준비는 순수 CPU 작업 — 여기서 죽으면 하드웨어와 무관하다
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++) {
      In[i][j] = 0;   // BF16이든 INT8이든 0은 안전한 비트패턴
      Out[i][j] = 0;
    }
  mark(0, "buffers ready (CPU only)");

  // 1) flush — 가장 단순한 RoCC 명령. 메모리 접근이 없다.
  mark(1, "about to gemmini_flush");
  gemmini_flush(0);
  mark(2, "gemmini_flush returned");

  // 2) config_ld — 설정만 바꾼다. 여전히 메모리 접근 없음.
  mark(3, "about to config_ld");
  gemmini_config_ld(DIM * sizeof(elem_t));
  mark(4, "config_ld returned");

  // 3) config_st
  mark(5, "about to config_st");
  gemmini_config_st(DIM * sizeof(elem_t));
  mark(6, "config_st returned");

  // 4) mvin — 첫 메모리 접근. E33이 지목한 경로.
  mark(7, "about to mvin (first memory access)");
  gemmini_mvin(In, 0);
  mark(8, "mvin issued (not yet fenced)");

  // 5) fence — 여기서 비로소 완료를 기다린다.
  //    mvin이 응답을 못 받으면 STEP 8까지 찍히고 여기서 죽는다.
  mark(9, "about to fence after mvin");
  gemmini_fence();
  mark(10, "fence after mvin returned");

  // 6) mvout
  mark(11, "about to mvout");
  gemmini_mvout(Out, 0);
  mark(12, "mvout issued");

  mark(13, "about to fence after mvout");
  gemmini_fence();
  mark(14, "fence after mvout returned");

  printf("=== ROCC PROBE survived all steps ===\n");
  fflush(stdout);
  return 0;
}
