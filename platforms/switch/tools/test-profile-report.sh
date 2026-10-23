#!/usr/bin/env bash
# V1-rc12: profile_report.py (the main-thread sampler's host side) on a made-up symbol table and profile: self,
# inclusive and stacks counted as expected, addresses outside the NRO kept apart. No hardware.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/dusk-profile-report-test"
mkdir -p "$out"
cat > "$out/nm.txt" <<'EOF'
0000000000001000 T PSMTXConcat
0000000000001100 T PSMTXMultVec
0000000000002000 T J3DModel::calc()
0000000000003000 T dBgS::Move()
0000000000004000 T main01()
0000000000005000 r some_rodata
EOF
base=0x200000000
{
  echo "# dusklight sampler v1: build test, NRO base 0x200000000, interval ~1 ms, 6 samples, zone test"
  echo "# pc lr f1 f2"
  echo "200001010 200002010 200002010 200004010"   # PSMTXConcat <- J3DModel::calc <- main01
  echo "200001020 200002020 200002020 200004020"
  echo "200001110 200002030 200002030 200004030"   # PSMTXMultVec <- J3DModel::calc <- main01
  echo "200003010 200004040 0 0"                   # dBgS::Move, leaf: caller from LR
  echo "900000000 200004050 0 0"                   # a mod's code
  echo "garbage line"
} > "$out/profile.txt"
python3 "$here/profile_report.py" "$out/profile.txt" --symbols "$out/nm.txt" --top 10 > "$out/report.txt"
cat "$out/report.txt"
grep -q "^5 samples" "$out/report.txt"
grep -Eq "40\.00% +2 +PSMTXConcat$" "$out/report.txt"
grep -Eq "20\.00% +1 +PSMTXMultVec$" "$out/report.txt"
grep -Eq "60\.00% +3 +J3DModel::calc\(\)$" "$out/report.txt"
grep -Eq "40\.00% +2 +PSMTXConcat <- J3DModel::calc\(\) <- main01\(\)$" "$out/report.txt"
grep -Eq "20\.00% +1 +dBgS::Move\(\) <- main01\(\)$" "$out/report.txt"
grep -q "\[outside NRO\]" "$out/report.txt"
echo "test-profile-report: all checks passed"
