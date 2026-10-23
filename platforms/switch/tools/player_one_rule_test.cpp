// aleks2 (rc17): extern/aurora/lib/player_one_rule.hpp -- which npad gets player 1's vibration and six-axis.
#include "player_one_rule.hpp"

#include <cstdio>

using namespace aurora::device::player_one_rule;

static int s_failures = 0;

static void expect(const char* what, const Npad got, const Npad want) {
  if (got != want) {
    std::printf("FAIL: %s: got %d, want %d\n", what, static_cast<int>(got), static_cast<int>(want));
    ++s_failures;
  }
}

static Seen seen(const bool no1, const bool handheld, const bool no1Input = false, const bool handheldInput = false) {
  Seen s;
  s.no1Connected = no1;
  s.handheldConnected = handheld;
  s.no1Input = no1Input;
  s.handheldInput = handheldInput;
  return s;
}

int main() {
  // Alex's case: a DualSense on No1 (MissionControl, a Pro Controller) and the Joy-Con on the console.
  expect("both connected, no input yet: the external controller", choose(seen(true, true), Npad::None), Npad::No1);
  expect("both connected, playing on No1", choose(seen(true, true, true, false), Npad::Handheld), Npad::No1);
  expect("both connected, picks up the console", choose(seen(true, true, false, true), Npad::No1), Npad::Handheld);
  expect("both connected, quiet: keeps the last one", choose(seen(true, true), Npad::Handheld), Npad::Handheld);
  expect("both connected, input on both: keeps the last one", choose(seen(true, true, true, true), Npad::No1),
         Npad::No1);
  // One connected: that one, whatever came before.
  expect("docked, Pro Controller only", choose(seen(true, false), Npad::Handheld), Npad::No1);
  expect("handheld only", choose(seen(false, true), Npad::No1), Npad::Handheld);
  expect("handheld only, input on the gone No1 ignored", choose(seen(false, true, true, false), Npad::No1),
         Npad::Handheld);
  expect("controller switched off, console docked", choose(seen(false, false), Npad::No1), Npad::None);
  if (s_failures == 0) {
    std::printf("player_one_rule_test: all checks passed\n");
  }
  return s_failures == 0 ? 0 : 1;
}
