"""H38-c4: the gate refuses an NRO whose version is UNKNOWN-VERSION or zero (h38_policy.version_rejections), and accepts the
beta, the numeric and the Build ID labels. Synthetic NROs with an ASET block and a NACP; no build needed.

  python3 app_version_policy_test.py
"""
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import h38_policy  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL {what}")


def make_nro(version, with_aset=True):
    """An NRO as elf2nro writes it, reduced to what the policy reads: the header, one page of 'code', ASET + NACP."""
    body = bytearray(0x1000)
    body[0x10:0x14] = b"NRO0"
    struct.pack_into("<I", body, 0x18, len(body))
    out = bytearray(body)
    if with_aset:
        nacp = bytearray(0x4000)
        nacp[0:9] = b"Dusklight"
        raw = version.encode()
        nacp[0x3060:0x3060 + len(raw)] = raw
        icon = b"\xff\xd8\xff\xd9"
        # ASET header: magic, version, then (offset, size) of icon, NACP and RomFS relative to the ASET start
        aset = bytearray(0x38)
        aset[0:4] = b"ASET"
        struct.pack_into("<QQQQQQ", aset, 8, 0x38, len(icon), 0x38 + len(icon), len(nacp), 0, 0)
        out += aset + icon + nacp
    return bytes(out)


CLEAN_ELF = b"\x7fELF" + b"v1.0.0-beta.1\0" + b"0.0.0.0\0"  # 0.0.0.0 alone in the image is an address, not a version

# acceptable labels
for good in ("1.0.0-beta.1", "1.0.0.0", "1.0.0", "2.0.3-105", "H38-c4", "1.2.3.12"):
    check(h38_policy.version_rejections(make_nro(good), CLEAN_ELF) == [], f"accepted: {good}")
check(h38_policy.nro_nacp_version(make_nro("1.0.0-beta.1")) == "1.0.0-beta.1", "the NACP version is read back")

# refused labels
for bad in ("UNKNOWN-VERSION", "0.0.0.0", "0.0.0", "0", "", "v0.0.0", "0.0.0.0-dirty", "xUNKNOWN-VERSIONx"):
    rej = h38_policy.version_rejections(make_nro(bad), CLEAN_ELF)
    check(len(rej) == 1 and "NACP version" in rej[0], f"refused: {bad!r} -> {rej}")

# no NACP at all: not silently accepted
rej = h38_policy.version_rejections(make_nro("1.0.0", with_aset=False), CLEAN_ELF)
check(len(rej) == 1 and "no NACP" in rej[0], f"no ASET block is a problem: {rej}")
check(h38_policy.nro_nacp_version(b"") is None and h38_policy.nro_nacp_version(b"\0" * 0x40) is None, "garbage has no NACP")

# the string in the image
rej = h38_policy.version_rejections(make_nro("1.0.0-beta.1"), CLEAN_ELF + b"UNKNOWN-VERSION\0")
check(len(rej) == 1 and "ELF carries UNKNOWN-VERSION" in rej[0], f"UNKNOWN-VERSION in the ELF is refused: {rej}")
rej = h38_policy.version_rejections(make_nro("UNKNOWN-VERSION"), CLEAN_ELF + b"UNKNOWN-VERSION\0")
check(len(rej) == 2, f"both are reported: {rej}")

# the text rule on its own
for text, ok in (("1.0.0-beta.1", True), ("v1.0.0", True), ("0.1.0", True), ("0.0.1", True), ("10.0.0", True),
                 ("0.0.0", False), ("0.0.0.0", False), ("v0.0.0", False), ("0", False), ("UNKNOWN-VERSION", False),
                 ("  ", False), (None, False)):
    check((h38_policy.version_text_rejection(text) is None) == ok, f"version_text_rejection({text!r})")

if failures == 0:
    print("app version policy: all checks passed")
sys.exit(1 if failures else 0)
