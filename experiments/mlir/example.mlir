// ---- 입력: linalg 수준, shape 가 타입에 있다 ----
func.func @two_matmuls(%A0: memref<16x16xi8>, %B0: memref<16x16xi8>, %C0: memref<16x16xi32>,
                       %A1: memref<16x4xi8>,  %B1: memref<4x64xi8>,  %C1: memref<16x64xi32>) {
  linalg.matmul ins(%A0, %B0 : memref<16x16xi8>, memref<16x16xi8>)
               outs(%C0 : memref<16x16xi32>)
  linalg.matmul ins(%A1, %B1 : memref<16x4xi8>, memref<4x64xi8>)
               outs(%C1 : memref<16x64xi32>)
  return
}

// ---- gemmini-select-shape 통과 후: 각 op 에 shape 인덱스가 붙는다 ----
//   %A0 x %B0 : M,K,N = 16,16,16 -> S=1 (16x16),  h=16 >= K,  width 16 <= N
//   %A1 x %B1 : M,K,N = 16,4,64  -> S=4 (4x64),   h=4  >= K,  width 64 <= N
//
// func.func @two_matmuls(...) {
//   linalg.matmul {gemmini.shape = 0 : i32} ins(%A0, %B0 : ...) outs(%C0 : ...)
//   linalg.matmul {gemmini.shape = 2 : i32} ins(%A1, %B1 : ...) outs(%C1 : ...)
//   return
// }

// ---- 그 아래 lowering 이 shape 를 config_ex 에 싣는다 ----
//   gemmini.config_ex 는 rs1 을 만드는데, 이 프로젝트가 rs1[12:10] 을 shape 로 쓴다
//   (CONFIG_EX_RS1_SPACER1 안이라 기존 필드가 하나도 안 움직인다 — JOURNAL E446).
//
//   %rs1 = arith.constant  (dataflow<<2) | (shape<<10) | (a_stride<<16)  : i64
//   "gemmini.config_ex"(%rs1, %rs2) : (i64, i64) -> ()
//   "gemmini.tile_matmul"(...)      : ...

// ---- 동적 shape 이면 컴파일 시간에 못 고르지만, 런타임 산술로 고르면 된다 ----
// func.func @dyn(%A: memref<?x?xi8>, %B: memref<?x?xi8>, %C: memref<?x?xi32>) {
//   %c0 = arith.constant 0 : index
//   %c1 = arith.constant 1 : index
//   %K = memref.dim %A, %c1 : memref<?x?xi8>
//   %N = memref.dim %B, %c1 : memref<?x?xi8>
//   // S=4 는 K<=4 이고 N>=64 일 때, S=2 는 K<=8 이고 N>=32 일 때, 아니면 S=1
//   %shape = func.call @gemmini_pick_shape(%K, %N) : (index, index) -> i32
//   ... config_ex 에 %shape 를 실어서 발행 ...
// }
