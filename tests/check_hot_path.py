"""S6 of note 002, checked in the compiled code. Design: docs/design/002-codec.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>

The framers and the template encoder (C) make no call at all; the decoders
(scalar and SIMD, note 009) none but the stack protector's abort.
Encoder B may call memcpy (copying values of variable length) and
__stack_chk_fail (the stack protector's abort), nothing else: no allocation,
no system call. No subject contains a syscall instruction.
"""
import re
import subprocess
import sys

STACK = {"__stack_chk_fail@plt"}  # the stack protector's abort: never taken on a valid path
SUBJECTS = {
    "fixgw_frame": set(),
    "fixgw_frame_simd": set(),
    "fixgw_decode": STACK,
    "fixgw_decode_simd": STACK,
    "fixgw_decode_simd_padded": STACK,
    "fixgw_index_build": set(),
    "fixgw_index_build_padded": set(),
    "fixgw_decode_indexed": STACK,
    "fixgw_encode_c": set(),
    "fixgw_encode_b": {"memcpy@plt", "__stack_chk_fail@plt"},
}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


out = subprocess.run([sys.argv[1], "-d", "--no-show-raw-insn", sys.argv[2]],
                     check=True, capture_output=True, text=True).stdout
funcs, name = {}, None
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
    if m:
        name = m[1]
        funcs[name] = []
        continue
    m = re.match(r"^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$", line)
    if m and name is not None:
        funcs[name].append((m[1], m[2]))

for subject, allowed in SUBJECTS.items():
    ins = funcs.get(subject)
    check(ins is not None, f"{subject} found")
    if ins is None:
        continue
    targets = {re.search(r"<([^>]*)>", o)[1] for mnem, o in ins
               if (mnem.startswith("call") or (mnem.startswith("jmp") and "@plt" in o)) and "<" in o}
    indirect = [o for mnem, o in ins if mnem.startswith("call") and "<" not in o]
    sys_ins = [mnem for mnem, _ in ins if mnem in ("syscall", "sysenter", "int")]
    extra = sorted(targets - allowed)
    check(not extra and not indirect and not sys_ins,
          f"S6 {subject}: {len(ins)} instructions, calls only {sorted(allowed) or 'none'}"
          + (f": found {extra + indirect + sys_ins}" if extra or indirect or sys_ins else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
