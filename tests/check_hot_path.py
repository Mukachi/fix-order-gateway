"""S6 of note 002, checked in the compiled code. Design: docs/design/002-codec.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>

The framers and the template encoder (C) make no call at all; the decoders
(scalar and SIMD, note 009) none but the stack protector's abort.
Encoder B may call memcpy (copying values of variable length) and
__stack_chk_fail (the stack protector's abort), nothing else: no allocation,
no system call. No subject contains a syscall instruction.

The session (note 003, S6) is checked with everything it reaches: every
function called from the inbound and send subjects, transitively, may call
only memory copies, comparisons and searches (strlen included) and the stack
protector's abort,
never through a pointer, and contains no syscall instruction.
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

# Note 003: checked with everything they call.
REACHING = {"fixgw_session_inbound", "fixgw_session_send"}
# __memcpy_chk: memcpy with a bounds check (_FORTIFY_SOURCE, Ubuntu's default);
# strlen: a reason given as a C string (the codec's `why`) made a string_view.
MEMORY = {"memcpy@plt", "__memcpy_chk@plt", "memmove@plt", "memchr@plt", "memcmp@plt", "strlen@plt",
          "__stack_chk_fail@plt"}

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

def calls(ins):
    direct = {re.search(r"<([^>]*)>", o)[1] for mnem, o in ins
              if (mnem.startswith("call") or mnem.startswith("jmp")) and "<" in o}
    direct = {t.split("+")[0] for t in direct}
    indirect = [o for mnem, o in ins if mnem.startswith("call") and "<" not in o]
    return direct, indirect


for subject in sorted(REACHING):
    if subject not in funcs:
        check(False, f"{subject} found")
        continue
    seen, todo, bad = set(), [subject], []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        direct, indirect = calls(funcs[f])
        bad += [f"{f}: indirect call {o}" for o in indirect]
        bad += [f"{f}: {m}" for m, _ in funcs[f] if m in ("syscall", "sysenter", "int")]
        for t in direct:
            if t.endswith("@plt"):
                if t not in MEMORY:
                    bad.append(f"{f}: calls {t}")
            elif t in funcs and t != f:
                todo.append(t)
    size = sum(len(funcs[f]) for f in seen)
    check(not bad, f"S6 (note 003) {subject}: {len(seen)} functions, {size} instructions, calls only "
                   f"memory functions and the stack protector" + (f": found {bad[:5]}" if bad else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
