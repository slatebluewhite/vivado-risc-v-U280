#!/usr/bin/env python3
"""E424: tune.py의 어느 가지가 실제로 발동하는가 (규칙 커버리지).

E405의 동률 처리가 여섯 워크로드에서 한 번도 안 걸리고, 그 근거로 잰 Kc=64는
Ktil=96을 나누지 못해 도구가 고를 수 없는 값이었다. 그런 '죽은 가지'가 또 있는지
형상 격자 전체에 대해 센다. 발동 안 하는 가지는 유지비만 드는 것이고,
그 가지를 정당화한 측정은 다시 인용되면 안 된다.
"""
import sys, os, itertools, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tune

HIT = collections.Counter()
def hit(name): HIT[name] += 1

def probe(K, N, M, m):
    Kt = K // 16
    tg = tune.chunk_target(K, N); hit(f"chunk_target={tg}")
    cand = [b for b in tune.blocks_for(M, N) if tune.legal_kcs(Kt, *b)]
    if not cand: hit("cand 비어 있음"); return
    ok = [b for b in cand if tune.min_chunks(K, *b) <= 4]
    hit("pick_blocks: 목표 도달 블록 있음" if ok else "pick_blocks: E329 청크우선 분기")
    first = (min(ok, key=lambda b:(tune.intensity(b), -b[0] if N>=K else -b[1]))
             if ok else min(cand, key=lambda b:(tune.min_chunks(K,*b), tune.intensity(b))))
    hit("I↔J: N>=K -> I 큰 쪽" if N >= K else "I↔J: N<K -> J 큰 쪽")
    pool = ok or cand
    alts = [b for b in cand if b not in pool and tune.intensity(b) < tune.intensity(first)*0.8]
    hit("E318 alts 있음" if alts else "E318 alts 없음")
    for b in cand:
        kcs = tune.legal_kcs(Kt, *b)
        big = [kc for kc in kcs if kc >= tune.MIN_KC]
        hit("MIN_KC 미달로 전체 풀 사용" if not big else "MIN_KC 정상")
        p = big or kcs
        best = min(abs(Kt//kc - tg) for kc in p)
        ties = [kc for kc in p if abs(Kt//kc - tg) == best]
        if len(ties) > 1:
            hit(f"동률 처리 발동 (목표 {tg})")
        if best > 0: hit("청크 목표 미달 -> 되돌림")
    if M >= 256 and N >= 4*K and (M//16) % 16 == 0 and (N//16) % 4 == 0 and m >= 3:
        hit("(16,4) 힌트 발동")

if __name__ == "__main__":
    tune.ACC_LIMIT, tune.SP_LIMIT, tune.KN_THRESHOLD = 64, 1024, 2.42e6
    Ks = [256,512,768,1024,1280,1536,2048,2560,3072,4096,6144,8192,12288]
    Ns = Ks[:]
    n = 0
    for K, N, M, m in itertools.product(Ks, Ns, (128,256,512), (1,2,3)):
        if (N//16) % 4: continue
        probe(K, N, M, m); n += 1
    print(f"형상 x M x m 조합 {n}개를 통과시킨 결과\n")
    for k, v in sorted(HIT.items(), key=lambda x:-x[1]):
        print(f"  {v:8d}  {k}")
    print("\n한 번도 안 걸린 가지:")
    expected = ["동률 처리 발동 (목표 2)","동률 처리 발동 (목표 4)",
                "MIN_KC 미달로 전체 풀 사용","pick_blocks: E329 청크우선 분기",
                "cand 비어 있음","E318 alts 있음","(16,4) 힌트 발동"]
    dead = [e for e in expected if HIT[e] == 0]
    print("  " + ("\n  ".join(dead) if dead else "없음"))
