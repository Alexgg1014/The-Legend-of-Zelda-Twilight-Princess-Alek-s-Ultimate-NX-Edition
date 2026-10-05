// Fixture for test-symbol-manifest.sh: a stand-in for a game file, with what by-name hooks
// target -- a member function, a file-local static whose name also exists in another file, and
// a data member.
static int action_check(int v) { return v * 3 + 1; }

struct dMenu_Ring_c {
    int _create();
    static int count;
};
int dMenu_Ring_c::count = 4;
int dMenu_Ring_c::_create() { return action_check(count); }

int (*keep_ring_action)(int) = action_check;

// H37-c2: hook targets the post-link check must refuse. Kept by `used`, reached by nothing:
// what an inlined-away function's leftover copy looks like.
__attribute__((used)) static int ring_unreached(int v) { return v * 7 + 2; }
// One instruction (ret) followed by alignment padding: funchook's patch fits in the padding,
// so this one is hookable.
__attribute__((noinline)) void ring_one_instruction() { asm volatile(""); }  // kept: a side effect
int call_ring_one_instruction() {
    ring_one_instruction();
    return 0;
}
