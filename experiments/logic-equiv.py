#!/usr/bin/env python3
"""생성 Verilog 두 판이 **논리적으로** 같은지 본다 (E856).

왜 필요한가: Chisel 은 `@[file line:col]` 을 생성 Verilog 에 심고 assertion 문자열에도
줄번호를 넣는다. 그래서 **논리와 무관한 소스 편집(주석 한 줄)이 비트스트림 md5 를
무효화한다.** md5 가 달라졌다고 설계가 바뀐 것이 아니다 — 먼저 이걸로 확인할 것.

방법: 모듈 이름을 빼고 본문 해시의 **다중집합**을 비교한다. 이름/번호는 config 마다
Chisel 이 다르게 붙이므로(`Queue_148` 대 `Queue_136`) 이름을 빼야 진짜 차이만 남는다.

사용:
    experiments/logic-equiv.py A.v B.v          # 같은지 판정 (종료코드 0/1)
    experiments/logic-equiv.py A.v B.v --which  # 다른 모듈의 이름을 찍는다

⚠ 한계: 이것은 **패치 드리프트를 재구성 오류와 구별하지 못한다**. 기준 산출물이 현재
RTL 과 다른 패치 상태에서 지어졌으면 전부 "다름" 으로 나온다 (E850j). 같은 RTL 상태에서
나온 두 판을 비교할 때만 쓸 것.
"""
import sys, re, hashlib, collections

ANN = re.compile(r'// @\[.*?\]')
LN  = re.compile(r'([A-Za-z0-9_]+\.scala)[: ]\d+(:\d+)?')

def modules(path):
    """모듈 이름 -> 정규화한 본문 해시."""
    out, cur, buf = {}, None, []
    for line in open(path, errors='replace'):
        m = re.match(r'^module ([A-Za-z_][A-Za-z0-9_]*)\(', line)
        if m:
            cur, buf = m.group(1), []
            continue
        if cur is not None:
            if line.startswith('endmodule'):
                t = LN.sub(r'\1:N', ANN.sub('', ''.join(buf)))
                out[cur] = hashlib.md5(t.encode()).hexdigest()[:8]
                cur = None
                continue
            buf.append(line)
    return out

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    which = '--which' in sys.argv
    if len(args) != 2:
        print(__doc__); return 2
    A, B = modules(args[0]), modules(args[1])

    if which:
        d = sorted(k for k in set(A) | set(B) if A.get(k) != B.get(k))
        print(f"  다른 모듈 {len(d)} 개: " + (", ".join(d[:12]) + (" ..." if len(d) > 12 else "") if d else "없음"))
        return 0 if not d else 1

    ca, cb = collections.Counter(A.values()), collections.Counter(B.values())
    oa, ob = sum((ca - cb).values()), sum((cb - ca).values())
    print(f"  모듈 {len(A)} 대 {len(B)}  |  좌측만 {oa}  우측만 {ob}  "
          f"-> {'논리 동일' if oa == 0 and ob == 0 else '**다름**'}")
    return 0 if oa == 0 and ob == 0 else 1

sys.exit(main())
