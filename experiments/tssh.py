#!/usr/bin/env python3
"""U280 타깃에 ssh 로 명령을 보낸다 (CLAUDE.md 의 `scratchpad/tssh.py` 재작성).

왜 pty 인가: 호스트에 `sshpass` 가 없고 키 인증도 안 깔려 있어서, ssh 가 비밀번호를
**터미널에서만** 읽는다. 그래서 pty 를 붙여 'assword' 프롬프트를 보면 넣어 준다.

왜 `experiments/` 인가: 원본이 `scratchpad/` 에 있다가 통째로 사라졌다 (E857).
결론을 떠받치는 도구는 저장소 안에 둘 것.

사용:  experiments/tssh.py '<command>' [timeout_sec]
환경:  TGT (기본 debian@192.168.1.120), TPW (기본 debian)
"""
import os, pty, sys, time, select, subprocess

HOST = os.environ.get("TGT", "debian@192.168.1.120")
PW   = os.environ.get("TPW", "debian") + "\n"

def run(cmd, timeout=60):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("ssh", ["ssh", "-tt",
                          "-o", "StrictHostKeyChecking=no",
                          "-o", "UserKnownHostsFile=/dev/null",
                          "-o", "ConnectTimeout=10",
                          "-o", "NumberOfPasswordPrompts=1",
                          HOST, cmd])
        os._exit(1)
    out, sent, t0 = b"", False, time.time()
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
        if not sent and b"assword" in out[-200:]:
            os.write(fd, PW.encode()); sent = True
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        pass
    # 프롬프트 에코와 CR 을 걷어낸다
    txt = out.decode("utf-8", "replace").replace("\r\n", "\n")
    lines = [l for l in txt.split("\n")
             if "assword" not in l and not l.startswith("Warning: Permanently added")]
    return "\n".join(lines)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    to = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    print(run(sys.argv[1], to), end="")
