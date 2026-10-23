"""TLS-lab: the Python gate (elf_audit.verdict) says what the C++ loader does about TLS, with and without it.

usage: tls_gate_test.py DIR   (DIR from test-elf-tls.sh: tls_desc.so, tls_trad.so, tls_ie.so)

OFF (the default, a build without ALEKS2_MOD_TLS): any TLS is a blocker. ON: TLSDESC and the traditional
dialect pass for linux-aarch64; initial-exec (TLS_TPREL64 / DF_STATIC_TLS) is still refused; a switch-arm64
package with TLS is still refused; a thread-local import other than std::call_once's pair is refused.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import elf_audit  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    failures += 0 if cond else 1


# The test modules also import aleks2_test_host_var, a thread-local only the test harnesses (and the Switch
# self-test) offer: the gate is right to name it, so it is set aside here and checked on its own.
TEST_VARIABLE = "thread-local imports the game does not offer: aleks2_test_host_var"


def tls_blockers(blockers):
    return [b for b in blockers if ("TLS" in b or "thread-local" in b or "TPREL" in b) and b != TEST_VARIABLE]


d = pathlib.Path(sys.argv[1])
for on in (False, True):
    elf_audit.set_mod_tls(on)
    # Audited again in each mode, as a gate does after set_mod_tls (the verdict needs the imports).
    audits = {n: elf_audit.audit_elf(elf_audit.Elf((d / f"{n}.so").read_bytes(), n)) for n in ("tls_desc", "tls_trad", "tls_ie")}
    v = {n: a["verdict"] for n, a in audits.items()}
    linux = {n: tls_blockers(x["linux_compat"]["blockers"]) for n, x in v.items()}
    switch = {n: tls_blockers(x["switch_package"]["blockers"]) for n, x in v.items()}
    if not on:
        check(all(linux.values()), "TLS off: every TLS module blocked (linux-aarch64)")
    else:
        check(not linux["tls_desc"], f"TLS on: TLSDESC module passes ({linux['tls_desc']})")
        check(not linux["tls_trad"], f"TLS on: traditional dialect passes ({linux['tls_trad']})")
        check(bool(linux["tls_ie"]), f"TLS on: initial-exec still blocked ({linux['tls_ie']})")
        check(all(switch.values()), "TLS on: switch-arm64 with TLS still blocked")
        res = v["tls_desc"]["linux_compat"]["resolution"]
        check(set(res["tls_accessor"]) == {"_ZSt11__once_call", "_ZSt15__once_callable"},
              f"TLS on: call_once thread-locals through accessors ({res['tls_accessor']})")
        check({"__once_proxy", "pthread_once", "__cxa_thread_atexit"} <= set(res["shim"]),
              "TLS on: __once_proxy, pthread_once and __cxa_thread_atexit are shims")
        check(TEST_VARIABLE in v["tls_desc"]["linux_compat"]["blockers"],
              "TLS on: a foreign thread-local import (the harness's aleks2_test_host_var) is blocked")
elf_audit.set_mod_tls(False)
print("tls_gate_test: " + ("all checks passed" if not failures else f"{failures} check(s) failed"))
sys.exit(1 if failures else 0)
