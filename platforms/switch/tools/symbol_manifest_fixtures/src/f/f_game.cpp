// Fixture for test-symbol-manifest.sh: the second file, with the other `action_check` and the
// functions a mod imports, and an exported thread-local variable, which the loader has no TLS
// for and so must never resolve.
static int action_check(int v) { return v - 1; }

extern "C" int game_c_function(int v) { return v + 2; }
int game_unexported(int v) { return v * 5; }

int (*keep_game_action)(int) = action_check;
thread_local int game_tls_counter = 3;

extern "C" long loop_target(long n);
int call_ring_one_instruction();
extern "C" long short_target(long n);

int main() {
    return game_c_function(0) + game_unexported(1) + game_tls_counter + call_ring_one_instruction() +
           static_cast<int>(loop_target(3) + short_target(1));
}
