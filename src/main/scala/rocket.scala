package Vivado

import Chisel._
import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.devices.debug.DebugModuleKey
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.subsystem._
import freechips.rocketchip.devices.tilelink._
import freechips.rocketchip.tile.{BuildRoCC, OpcodeSet, TileKey}
import freechips.rocketchip.util.DontTouch
import freechips.rocketchip.system._
import freechips.rocketchip.rocket._

class RocketSystem(implicit p: Parameters) extends RocketSubsystem
    with HasAsyncExtInterrupts
    with CanHaveMasterAXI4MemPort
    with CanHaveMasterAXI4MMIOPort
    with CanHaveSlaveAXI4Port
{
  val bootROM  = p(BootROMLocated(location)).map { BootROM.attach(_, this, CBUS) }
  override lazy val module = new RocketSystemModuleImp(this)
}

class RocketSystemModuleImp[+L <: RocketSystem](_outer: L) extends RocketSubsystemModuleImp(_outer)
    with HasRTCModuleImp
    with HasExtInterruptsModuleImp
    with DontTouch

class WithGemmini(mesh_size: Int, bus_bits: Int) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits)))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

class WithDebugProgBuf(prog_buf_words: Int, imp_break: Boolean) extends Config((site, here, up) => {
  case DebugModuleKey => up(DebugModuleKey, site).map(_.copy(nProgramBufferWords = prog_buf_words, hasImplicitEbreak = imp_break))
})

/*----------------- 32-bit RocketChip ---------------*/
/* Note: Linux not supported yet on 32-bit cores     */

/* 32-bit config, max memory 2GB */
class Rocket32BaseConfig extends Config(
  new WithBootROMFile("workspace/bootrom.img") ++
  new WithExtMemSize(0x80000000L) ++
  new WithNExtTopInterrupts(8) ++
  new WithDTS("freechips,rocketchip-vivado", Nil) ++
  new WithDebugSBA ++
  new WithEdgeDataBits(64) ++
  new WithCoherentBusTopology ++
  new WithoutTLMonitors ++
  new BaseConfig)

class Rocket32s1 extends Config(
  new WithNBreakpoints(8) ++
  new WithNSmallCores(1)  ++
  new WithRV32            ++
  new Rocket32BaseConfig)

class Rocket32s2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNSmallCores(2)  ++
  new WithRV32            ++
  new Rocket32BaseConfig)

/* With exposed JTAG port */
class Rocket32s2j extends Config(
  new WithNBreakpoints(8) ++
  new WithJtagDTM         ++
  new WithNSmallCores(2)  ++
  new WithRV32            ++
  new Rocket32BaseConfig)

class Rocket32s4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNSmallCores(4)  ++
  new WithRV32            ++
  new Rocket32BaseConfig)

class Rocket32s8 extends Config(
  new WithNBreakpoints(8) ++
  new WithNSmallCores(8)  ++
  new WithRV32            ++
  new Rocket32BaseConfig)

class Rocket32s16 extends Config(
  new WithNBreakpoints(8) ++
  new WithNSmallCores(16) ++
  new WithRV32            ++
  new Rocket32BaseConfig)

/*----------------- 64-bit RocketChip ---------------*/

/*
 * WithExtMemSize(0x380000000L) = 14GB (16GB minus 2GB for IO) is max supported by the base config.
 * Actual memory size depends on the target board.
 * The Makefile changes the size to correct value during build.
 * It also sets right core clock frequency.
 */
class RocketBaseConfig extends Config(
  new WithBootROMFile("workspace/bootrom.img") ++
  new WithExtMemSize(0x380000000L) ++
  new WithNExtTopInterrupts(8) ++
  new WithDTS("freechips,rocketchip-vivado", Nil) ++
  new WithDebugSBA ++
  new WithEdgeDataBits(64) ++
  new WithCoherentBusTopology ++
  new WithoutTLMonitors ++
  new BaseConfig)

class RocketWideBusConfig extends Config(
  new WithBootROMFile("workspace/bootrom.img") ++
  new WithExtMemSize(0x380000000L) ++
  new WithNExtTopInterrupts(8) ++
  new WithDTS("freechips,rocketchip-vivado", Nil) ++
  new WithDebugSBA ++
  new WithEdgeDataBits(256) ++
  new WithCoherentBusTopology ++
  new WithoutTLMonitors ++
  new BaseConfig)

class Rocket64b1 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(1)    ++
  new RocketBaseConfig)

class Rocket64b2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

/* With exposed BSCAN port - the name must end with 'e' */
/* With up to 256GB memory */
/* Note: lower 2GB are used for memory mapped IO, so max usable RAM size is 254GB */
class Rocket64b1e extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(1)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

class Rocket64b2e extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

/* With up to 256GB memory */
/* Note: lower 2GB are used for memory mapped IO, so max usable RAM size is 254GB */
class Rocket64b2m extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

/* With up to 256GB memory, L2 cache, wide memory bus, 2 memory controllers */
class Rocket64b2m2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b4m2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(4)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b8m2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(8)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b16m2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(16)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b24m2 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(24)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

/* With up to 256GB memory, L2 cache, wide memory bus, 4 memory controllers */
/* Note: lower 2GB are used for memory mapped IO, so max usable RAM size is 254GB */
class Rocket64b2m4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNBanks(4) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b4m4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(4)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(8) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b8m4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(8)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(8) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

class Rocket64b16m4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(16)    ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(8) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

/* With exposed JTAG port */
class Rocket64b2j extends Config(
  new WithNBreakpoints(8) ++
  new WithJtagDTM         ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

/* Smaller debug module */
class Rocket64b2d1 extends Config(
  new WithNBreakpoints(1) ++
  new WithNBigCores(2)    ++
  new WithDebugProgBuf(1, true) ++
  new RocketBaseConfig)

/* Smaller debug module */
class Rocket64b2d2 extends Config(
  new WithNBreakpoints(2) ++
  new WithNBigCores(2)    ++
  new WithDebugProgBuf(2, true) ++
  new RocketBaseConfig)

/* Smaller debug module */
class Rocket64b2d3 extends Config(
  new WithNBreakpoints(3) ++
  new WithNBigCores(2)    ++
  new WithDebugProgBuf(2, false) ++
  new RocketBaseConfig)

/* With 512KB level 2 cache */
/* Note: adding L2 cache reduces max CPU clock frequency */
class Rocket64b2l2 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

/* With Gemmini 4x4 and 2 small cores */
/* Note: small core has no MMU and cannot boot mainstream Linux */
class Rocket64s2gem4 extends Config(
  new WithGemmini(4, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNSmallCores(2)  ++
  new RocketBaseConfig)

/* With Gemmini 4x4 and 2 medium cores */
/* Note: cannot get medium core to boot Linux: Oops - illegal instruction */
class Rocket64m2gem4 extends Config(
  new WithGemmini(4, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNMedCores(2)    ++
  new RocketBaseConfig)

/* With Gemmini 4x4 */
class Rocket64b1gem4 extends Config(
  new WithGemmini(4, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(1)    ++
  new RocketBaseConfig)

/* With Gemmini 8x8 */
class Rocket64b1gem8 extends Config(
  new WithGemmini(8, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(1)    ++
  new RocketBaseConfig)

/* With Gemmini 16x16 */
class Rocket64b1gem16 extends Config(
  new WithGemmini(16, 64) ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(1)    ++
  new RocketBaseConfig)

/* With Gemmini 4x4, 2 big cores */
class Rocket64b2gem4 extends Config(
  new WithGemmini(4, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

/* With Gemmini 8x8, 2 big cores */
class Rocket64b2gem8 extends Config(
  new WithGemmini(8, 64)  ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

/* With Gemmini 16x16, 2 big cores */
class Rocket64b2gem16 extends Config(
  new WithGemmini(16, 64) ++
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(2)    ++
  new RocketBaseConfig)

class Rocket64b4 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(4)    ++
  new RocketBaseConfig)

/* With level 2 cache and wide memory bus */
class Rocket64b4l2w extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new WithNBigCores(4)    ++
  new RocketWideBusConfig)

class Rocket64b8 extends Config(
  new WithNBreakpoints(8) ++
  new WithNBigCores(8)    ++
  new RocketBaseConfig)

class Rocket64b16m extends Config(
  new WithNBreakpoints(4) ++
  new WithNBigCores(16)   ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

class Rocket64b24m extends Config(
  new WithNBreakpoints(4) ++
  new WithNBigCores(24)   ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

class Rocket64b32m extends Config(
  new WithNBreakpoints(4) ++
  new WithNBigCores(32)   ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketBaseConfig)

/* Without slave port - for use in HDL simulation */
class Rocket64b2s extends Config(
  new WithNBigCores(2)    ++
  new WithBootROMFile("workspace/bootrom.img") ++
  new WithExtMemSize(0x40000000) ++
  new WithNExtTopInterrupts(8) ++
  new WithEdgeDataBits(64) ++
  new WithCoherentBusTopology ++
  new WithoutTLMonitors ++
  new WithNoSlavePort ++
  new BaseConfig)

/*----------------- Sonic BOOM   ---------------*/

class Rocket64w1 extends Config(
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNSmallBooms(1) ++
  new RocketBaseConfig)

class Rocket64x1 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNMediumBooms(1) ++
  new RocketWideBusConfig)

/* Note: multi-core BOOM appears unstable */
class Rocket64x2 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNMediumBooms(2) ++
  new RocketWideBusConfig)

class Rocket64x4 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNMediumBooms(4) ++
  new RocketWideBusConfig)

class Rocket64x8 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(4) ++
  new boom.v3.common.WithNMediumBooms(8) ++
  new RocketWideBusConfig)

class Rocket64x12 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(4) ++
  new boom.v3.common.WithNMediumBooms(12) ++
  new RocketWideBusConfig)

/* With up to 256GB memory, L2 cache, wide memory bus, 2 memory controllers */
/* Note: lower 2GB are used for memory mapped IO, so max usable RAM size is 254GB */
class Rocket64x12m4 extends Config(
  new WithNBreakpoints(4) ++
  new boom.v3.common.WithNMediumBooms(12) ++
  new WithExtMemSize(0x3f80000000L) ++
  new WithNMemoryChannels(2) ++
  new WithNBanks(8) ++
  new WithInclusiveCache ++
  new RocketWideBusConfig)

/* Note: 3-way BOOM appears unstable */
class Rocket64y1 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNLargeBooms(1) ++
  new RocketWideBusConfig)

/* Note: 4-way BOOM appears unstable */
class Rocket64z1 extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNMegaBooms(1) ++
  new RocketWideBusConfig)

/* With up to 256GB memory */
/* Note: lower 2GB are used for memory mapped IO, so max usable RAM size is 254GB */
/* Note: 4-way BOOM appears unstable */
class Rocket64z2m extends Config(
  new WithInclusiveCache  ++
  new WithNBreakpoints(8) ++
  new boom.v3.common.WithNMegaBooms(2) ++
  new WithExtMemSize(0x3f80000000L) ++
  new RocketWideBusConfig)

/* ============================================================================
 * E850: 아래 블록은 **재구성본**이다. 2026-09-02 에 `git checkout` 으로 이 파일의
 * 미커밋 변경을 통째로 날렸고(E850 참조), git 에 staged 이력이 없어 복원이 불가능했다.
 * 파라미터는 두 경로가 서로를 확인해 준다:
 *   ① 같은 세션에 읽은 원문 (shapeshift 계열)
 *   ② `workspace/<config>/system-u280.v` 에서 역산한 값
 *      (ScratchpadBank/AccumulatorMem/AccScalePipe 인스턴스 수, io_shape 포트 폭)
 * 둘이 13 개 config 전부에서 일치했다. 그래도 **원문과 바이트 동일하다는 보장은 없다.**
 * ========================================================================== */

/* stock Gemmini + 공유 scale 유닛(num_scale_units). E314/E320 이 62.5 MHz 를 위해
 * 도입했고 `patches/gemmini-accscale-norm0.patch` 를 함께 요구한다. */
class WithGemminiSu(mesh_size: Int, bus_bits: Int, nsu: Int, nxacts: Int = 16,
                    /* E851: AccScalePipe 조합 사슬 분할 단수. 0 = stock. */
                    pipe_split: Int = 0)
    extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits,
        max_in_flight_mem_reqs = nxacts,
        acc_scale_args = Some(gemmini.GemminiConfigs.defaultConfig.acc_scale_args.get.copy(
          num_scale_units = nsu, pipe_split = pipe_split)))))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

/* Shapeshift (E446): 실행 중에 배열을 접는 Gemmini. `max_segments` 하나가 세 가지를
 * 켜므로(접힌 mesh · acc_banks · GEMV FSM — E836b/c) 축을 인자로 분리했다. */
class WithGemminiShapeshift(mesh_size: Int, bus_bits: Int, max_segments: Int,
                            row_floor: Int = 4, sp_banks: Int = 4, nsu: Int = -1,
                            /* E679: 미해결 메모리 요청 수. 기본 16 x 64 B 이고, E678 이 잰
                             * 적재 천장(열 7.96 / 냉 5.2 B/cycle)이 Little's law 로 지연
                             * ~128/~197 cyc 에 맞는다 — 이 경로는 **요청 수 바운드**다. */
                            nxacts: Int = 16,
                            /* E836b: 누산기 뱅크 수. 접기는 세그먼트마다 뱅크가 필요해
                             * 기본값은 max_segments 를 따라가지만(-1) 둘은 **다른 축**이다.
                             * `max_segments=1` 을 그냥 주면 acc_banks 도 1 이 되어
                             * **stock 의 2 보다 적은** 누산기를 얻는다. */
                            acc_banks: Int = -1,
                            /* E836c: GEMV FSM 을 접기와 분리해 켠다. 기본 false 는
                             * "접기가 켜지면 FSM 도 켜진다"(기존 동작). */
                            gemv_fsm: Boolean = false,
                            /* E840d: 누산기 총 용량(KB). 뱅크를 늘리면 뱅크당 깊이가 반이
                             * 되어 BRAM 추론이 깨지므로(512 -> 256 에서 128 RAMB18 이
                             * 10,240 LUTRAM 이 된다) 깊이를 지키려면 용량도 키워야 한다. */
                            acc_kb: Int = -1,
                            /* E851: AccScalePipe 의 조합 사슬을 몇 단으로 쪼갤지.
                             * 총 지연(latency=8)은 유지되므로 사이클 수는 안 변한다.
                             * 1 = scale_func 뒤/포화 앞, 2 = 거기에 활성화 뒤/scale_func 앞. */
                            pipe_split: Int = 0) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits,
        acc_banks = if (acc_banks > 0) acc_banks else max_segments, sp_banks = sp_banks,
        acc_capacity = if (acc_kb > 0) gemmini.CapacityInKilobytes(acc_kb)
                       else gemmini.GemminiConfigs.defaultConfig.acc_capacity,
        max_in_flight_mem_reqs = nxacts,
        shapeshift_max_segments = max_segments,
        shapeshift_gemv_fsm = gemv_fsm,
        shapeshift_row_floor = row_floor,
        /* E642: 기본(-1)은 원소마다 scale_func 를 펼쳐 배선이 터진다. 공유 유닛 분기(nsu=16)는
         * scale_func **앞**에 레지스터를 두어 임계 경로를 끊는다 (E313/E314/E320). */
        acc_scale_args = Some(gemmini.GemminiConfigs.defaultConfig.acc_scale_args.get.copy(
          num_scale_units = nsu, pipe_split = pipe_split)))))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

/* 8x8 shapeshift, total_rows 바닥 2 (E516~E529 의 floor-2 묶음) */
class Rocket64b1gem8ssf2 extends Config(
  new WithGemminiShapeshift(8, 64, 4, 2) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem16ss extends Config(
  new WithGemminiShapeshift(16, 64, 4) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E596: sp_banks=8 — 접힌 D 가 세그먼트마다 뱅크를 쓰므로 A 와의 겹침을 없앤다 */
class Rocket64b1gem16ss8b extends Config(
  new WithGemminiShapeshift(16, 64, 4, 4, 8) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem16ss8bsu16f50 extends Config(
  new WithGemminiShapeshift(16, 64, 4, 4, 8, 16) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E642: 62.5 MHz 는 정적으로 닫히지만 **보드가 부팅하지 않는다** — 쓰지 말 것 */
class Rocket64b1gem16ss8bsu16f62 extends Config(
  new WithGemminiShapeshift(16, 64, 4, 4, 8, 16) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem16ss8bsu16x64f50 extends Config(
  new WithGemminiShapeshift(16, 64, 4, 4, 8, 16, 64) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E681/E685: 시스템 버스 256-bit. 적재가 버스 바운드였고 이것이 그것을 푼다 */
class Rocket64b1gem16ss8bsu16w256f50 extends Config(
  new WithGemminiShapeshift(16, 256, 4, 4, 8, 16) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E836c: FSM 만 (접기 없음). acc_banks 를 stock 과 같은 2 로 고정해야
 * FSM 만의 값어치가 누산기 축소와 섞이지 않는다 */
class Rocket64b1gem16ss8bsu16w256ms1f50 extends Config(
  new WithGemminiShapeshift(16, 256, 1, 4, 8, 16, 16, 2, gemv_fsm = true) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E837: **권장 구성** — S=2 는 면적도 타이밍도 공짜다 (stock 이 이미 2 뱅크를 낸다) */
class Rocket64b1gem16ss8bsu16w256ms2f50 extends Config(
  new WithGemminiShapeshift(16, 256, 2, 4, 8, 16, 16, 2) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E840: 접기 없이 **넓은 누산기만** — 뱅크 수와 접기를 가르는 귀속용 빌드 */
class Rocket64b1gem16ss8bsu16w256ab4f50 extends Config(
  new WithGemminiShapeshift(16, 256, 1, 4, 8, 16, 16, 4, gemv_fsm = true) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E840d: acc_banks=4 인데 뱅크당 깊이를 512 로 유지 (LUTRAM 이 깊이 탓인지 가른다) */
class Rocket64b1gem16ss8bsu16w256ab4k128f50 extends Config(
  new WithGemminiShapeshift(16, 256, 1, 4, 8, 16, 16, 4, gemv_fsm = true, acc_kb = 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E840e: S=4 + 깊이 유지 — S=4 의 비용이 24,842 -> 12,536 LUT 로 절반이 된다 */
class Rocket64b1gem16ss8bsu16w256k128f50 extends Config(
  new WithGemminiShapeshift(16, 256, 4, 4, 8, 16, 16, 4, acc_kb = 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E650: 기준선 — stock + 공유 scale 유닛 @50 MHz */
class Rocket64b1gem16su16f50 extends Config(
  new WithGemminiSu(16, 64, 16) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* ============================================================================
 * E850a: 다중 가속기 계열 — **재구성본이고 shapeshift 쪽보다 확신이 낮다.**
 * 원문을 읽은 적이 없어 교차 확인 경로가 하나뿐이다(빌드 산출물 역산):
 *   가속기 수 = Verilog 의 `Gemmini v / v_1 / v_2` 인스턴스 수 (gem2 -> 2, gem3 -> 3)
 *   nsu       = AccScalePipe 인스턴스 수 (su20 -> norm 없이 16, `n` 판 20 = nsu 20)
 *   버스      = 이름의 w256
 *   opcode    = experiments/tests/include/gemmini_rt.h 의 규약
 *               가속기 0/1/2 -> custom3(GRT_OP_INT8) / custom2 / custom1
 * **등가 확인됨**: 같은 config 를 재생성해 대조하면 `Gemmini` 와 `AccumulatorScale`
 * 이 byte-identical 이다 (assertion 문자열의 줄 번호 제외). 안쪽 모듈의 차이는
 * config 이 아니라 그 사이의 **패치 진화** 다 — 옛 빌드끼리는 서로 같다.
 * **CLAUDE.md 의 이 계열 측정치는 비트스트림과 리포트가 남아 있어 유효하다** —
 * 없어진 것은 config 정의뿐이고, 아래는 그것을 되살리려 한 것이다.
 * ========================================================================== */

/** 한 코어에 같은 INT8 Gemmini 를 `n` 개. opcode 는 custom3/2/1 순서 (E167). */
class WithGemminiMulti(n: Int, mesh_size: Int, bus_bits: Int, nsu: Int = -1,
                       nxacts: Int = 16, sp_kb: Int = -1, acc_kb: Int = -1,
                       norms: Boolean = false) extends Config((site, here, up) => {
  // E850f: 가속기는 **타일 0 에만** 붙는다. 1 코어 판에서는 무해하지만 2 코어 판에서는
  // 이 가드가 없으면 코어마다 n 개가 붙어 산출물이 달라진다 (원본은 타일0=n, 타일1=0).
  // 모듈 본문 diff 로는 안 잡힌다 — 배치는 부모 모듈 쪽 이야기다.
  case BuildRoCC => up(BuildRoCC) ++
    (if (site(TileKey).tileId == 0) (0 until n).toSeq else Seq.empty[Int]).map { i =>
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      val ops = i match {
        case 0 => OpcodeSet.custom3
        case 1 => OpcodeSet.custom2
        case _ => OpcodeSet.custom1
      }
      val base = gemmini.GemminiConfigs.defaultConfig
      LazyModule(new gemmini.Gemmini(base.copy(
        opcodes = ops,
        /* E850a: **`dma_buswidth` 는 올리지 않는다.** 원본은 가속기 DMA 를 128 비트로
         * 두고 `SystemBusKey.beatBytes` 만 32 로 올렸다 (CLAUDE.md). 여기를 bus_bits 로
         * 주면 Scratchpad 내부 xbar 가 256 이 되어 원본과 달라진다 — 그것이 재구성이
         * 어긋난 지점이었고, TileLink `denied`/`corrupt` 유무로 드러났다. */
        meshRows = mesh_size, meshColumns = mesh_size,
        max_in_flight_mem_reqs = nxacts,
        sp_capacity  = if (sp_kb  > 0) gemmini.CapacityInKilobytes(sp_kb)  else base.sp_capacity,
        acc_capacity = if (acc_kb > 0) gemmini.CapacityInKilobytes(acc_kb) else base.acc_capacity,
        has_normalizations = norms,
        acc_scale_args = Some(base.acc_scale_args.get.copy(num_scale_units = nsu)))))
    }
  }
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

/* E238: 두/세 INT8 가속기 + 256-bit 버스 @50 MHz */
class Rocket64b1gem2i8w256f50 extends Config(
  new WithGemminiMulti(2, 16, 256) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem3i8w256f50 extends Config(
  new WithGemminiMulti(3, 16, 256) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E314/E320: 공유 scale 유닛(nsu=20)으로 AccumulatorScale 경로를 끊어 62.5 MHz */
class Rocket64b1gem2i8w256su20f62 extends Config(
  new WithGemminiMulti(2, 16, 256, 20) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem3i8w256su20f62 extends Config(
  new WithGemminiMulti(3, 16, 256, 20) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E376: 온칩 메모리 2 배 — `I*J <= 64` 가 legal 해져 (8,8) 블록을 쓸 수 있다 */
class Rocket64b1gem2i8w256su20mem2f62 extends Config(
  new WithGemminiMulti(2, 16, 256, 20, 16, 512, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem3i8w256su20mem2f62 extends Config(
  new WithGemminiMulti(3, 16, 256, 20, 16, 512, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E850a: `q32` = **미해결 메모리 요청 32 개**. 원본의 TileLink `source` 가 6 비트인데
 * 기본(16)이면 5 비트라 갈렸다 — diff 가 그 한 필드를 정확히 가리켰다. */
class Rocket64b1gem2i8w256su20q32f62 extends Config(
  new WithGemminiMulti(2, 16, 256, 20, 32) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* n: WithGemminiNorm 상당 — LayerNorm/Softmax/iGELU 유닛을 켠다 (AccScalePipe 16 -> 20) */
class Rocket64b1gem2i8nw256su20f50 extends Config(
  new WithGemminiMulti(2, 16, 256, 20, 16, -1, -1, norms = true) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem2i8nw256su20f40 extends Config(
  new WithGemminiMulti(2, 16, 256, 20, 16, -1, -1, norms = true) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* ============================================================================
 * E850d: 2 코어·4 코어 계열 재구성. 파라미터는 전부 산출물 역산이고 각 항목의 근거는:
 *   DMA 폭   `auto_spad_id_out_a_bits_data` 포트 폭 (64 -> `WithGemmini(_,64)`, 128 -> `w`)
 *   L2 크기  system.dts 의 `cache-size` (524288 기본 / 2097152 = `l2b`,`wl2`)
 *   norm     AccumulatorScale 안의 `igelu` 참조 수 (0 대 52) -> `n`
 *   가속기 수 `Gemmini v/v_1/v_2` 인스턴스
 * ========================================================================== */

/* `n`: LayerNorm/Softmax/iGELU 유닛을 켠 판 (E?? 계열) */
class WithGemminiNorm(mesh_size: Int, bus_bits: Int) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits,
        has_normalizations = true)))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

class Rocket64b2gem16f40 extends Config(
  new WithGemmini(16, 64) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

/* `l2b`: L2 512 KB -> **2 MB** (CLAUDE.md 의 "L2 512KB->2MB" 실험) */
class Rocket64b2gem16l2b extends Config(
  new WithGemmini(16, 64) ++
  new WithInclusiveCache(capacityKB = 2048) ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16n extends Config(
  new WithGemminiNorm(16, 64) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

/* `w`: 가속기 DMA 64 -> **128 비트** */
class Rocket64b2gem16w extends Config(
  new WithGemmini(16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16wf40 extends Config(
  new WithGemmini(16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16nw extends Config(
  new WithGemminiNorm(16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16nwf25 extends Config(
  new WithGemminiNorm(16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16wl2f40 extends Config(
  new WithGemmini(16, 128) ++
  new WithInclusiveCache(capacityKB = 2048) ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem16wl2 extends Config(
  new WithGemmini(16, 128) ++
  new WithInclusiveCache(capacityKB = 2048) ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b4gem16 extends Config(
  new WithGemmini(16, 64) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(4) ++ new RocketBaseConfig)

/* 2 코어 + 다중 INT8 가속기 */
class Rocket64b2gem2i8f50 extends Config(
  new WithGemminiMulti(2, 16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem3i8f50 extends Config(
  new WithGemminiMulti(3, 16, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem3i8w256f50 extends Config(
  new WithGemminiMulti(3, 16, 256) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

/* ============================================================================
 * E850e: Float 계열 재구성.
 * elem_t 가 Float 이면 `GemminiConfigs.defaultConfig` (SInt 입력) 를 copy 할 수 없다 —
 * `mvin_scale_args` 의 함수 타입이 안 맞는다. `ConfigsFP.scala` 의 `GemminiFPConfigs`
 * 가 그 짝을 갖춘 config 를 이미 들고 있다 (FP32DefaultConfig / BF16DefaultConfig).
 * (앞서 `Configs.scala` 만 보고 "기성 Float config 가 없다" 고 판단한 것은 틀렸다.)
 * ========================================================================== */

class WithGemminiFP32(mesh_size: Int, bus_bits: Int) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiFPConfigs.FP32DefaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits)))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

/* `l1` = `tile_latency = 1` (기본 2). BF16 결함이 tile_latency 탓인지 보려고 지은 판. */
class WithGemminiBF16(mesh_size: Int, bus_bits: Int, tile_lat: Int = 2) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiFPConfigs.BF16DefaultConfig.copy(
        meshRows = mesh_size, meshColumns = mesh_size, dma_buswidth = bus_bits,
        tile_latency = tile_lat)))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = bus_bits/8)
})

/* `bl` (big.LITTLE): **코어 0 이 두 가속기를 다 진다** (INT8 16x16 = custom3,
 * FP32 8x8 = custom2), 코어 1 은 없다. BuildRoCC 는 타일마다 평가되므로
 * `site(TileKey).tileId` 로 Seq 길이를 다르게 반환할 수 있다.
 * `gemhet` (타일마다 서로 다른 Gemmini) 은 시스템 버스를 wedge 시킨다 — E123~E129. */
class WithGemminiBigLittle extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ (if (site(TileKey).tileId == 0) Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
        opcodes = OpcodeSet.custom3, meshRows = 16, meshColumns = 16)))
    },
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      LazyModule(new gemmini.Gemmini(gemmini.GemminiFPConfigs.FP32DefaultConfig.copy(
        opcodes = OpcodeSet.custom2, meshRows = 8, meshColumns = 8)))
    }
  ) else Seq())
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = 16)
})

/* `het`: 타일 0 = INT8 16x16, 타일 1 = FP32 8x8, **둘 다 custom3**.
 * 미해결 버그의 재현판이다 — 타일 1 에 custom 명령을 하나라도 보내면 기계가 죽는다. */
class WithGemminiHet extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      implicit val v = implicitly[ValName]
      if (p(TileKey).tileId == 0)
        LazyModule(new gemmini.Gemmini(gemmini.GemminiConfigs.defaultConfig.copy(
          opcodes = OpcodeSet.custom3, meshRows = 16, meshColumns = 16)))
      else
        LazyModule(new gemmini.Gemmini(gemmini.GemminiFPConfigs.FP32DefaultConfig.copy(
          opcodes = OpcodeSet.custom3, meshRows = 8, meshColumns = 8)))
    }
  )
  case SystemBusKey => up(SystemBusKey).copy(beatBytes = 16)
})

class Rocket64b2gem8fp32 extends Config(
  new WithGemminiFP32(8, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem8bf16 extends Config(
  new WithGemminiBF16(8, 128) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gem8bf16l1 extends Config(
  new WithGemminiBF16(8, 128, 1) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gembl extends Config(
  new WithGemminiBigLittle ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gemblf40 extends Config(
  new WithGemminiBigLittle ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gemblf50 extends Config(
  new WithGemminiBigLittle ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

class Rocket64b2gemhet extends Config(
  new WithGemminiHet ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

/* E153 의 비정방형 메쉬 타당성 조사. PE 16 개 고정에 가로세로비만 바꿔 본 것으로,
 * 정방형인 4x4 만 elaborate 됐다 (17 초). `gem2x8`/`gem8x2` 는 `allow_nonsquare = true`
 * 를 줘도 `ExecuteController.scala` 의 `w_mask`(block_size = 행 방향) 대
 * `sp_width`(열 방향) 충돌에서 막혀 산출물이 안 나왔다 — 그래서 재구성하지 않는다.
 * 결론은 "가능하지만 병목이 메쉬가 아니다" 였다. */
class Rocket64b2gem4x4 extends Config(
  new WithGemmini(4, 64) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(2) ++ new RocketBaseConfig)

/* E851: 클럭 축 실험 — AccScalePipe 조합 사슬 분할. 나머지는 권장 구성과 동일. */
class Rocket64b1gem16ss8bsu16w256ms2ps1f50 extends Config(
  new WithGemminiShapeshift(16, 256, 2, 4, 8, 16, 16, 2, false, -1, 1) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem16ss8bsu16w256ms2ps2f50 extends Config(
  new WithGemminiShapeshift(16, 256, 2, 4, 8, 16, 16, 2, false, -1, 2) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E851: 62.5 MHz(주기 16 ns) 판정용. ps2 = AccScalePipe 조합 사슬 2 분할.
 * 대조군은 `...ms2f62` (ps=0), 같은 것에서 pipe_split 만 다르다. */
class Rocket64b1gem16ss8bsu16w256ms2f62 extends Config(
  new WithGemminiShapeshift(16, 256, 2, 4, 8, 16, 16, 2, false, -1, 0) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

class Rocket64b1gem16ss8bsu16w256ms2ps2f62 extends Config(
  new WithGemminiShapeshift(16, 256, 2, 4, 8, 16, 16, 2, false, -1, 2) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)

/* E854: S=4 접기를 62.5 MHz 에 올릴 수 있는가. `k128f50` 에 pipe_split=2 만 더한 판이다
 * (E851f 가 ms2 에서 +0.050 -> +0.536 ns 를 벌었다). 되면 접기 1.25x 와 클럭 1.25x 를
 * 같이 갖는다. */
class Rocket64b1gem16ss8bsu16w256k128ps2f62 extends Config(
  new WithGemminiShapeshift(16, 256, 4, 4, 8, 16, 16, 4, acc_kb = 128, pipe_split = 2) ++
  new WithInclusiveCache ++ new WithNBreakpoints(8) ++ new WithNBigCores(1) ++ new RocketBaseConfig)
