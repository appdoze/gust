// runtime-define: GS_STACK_RESERVE=1048576
// Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
// Gust: containment covers aborts (§9.3), not a data stack running into its
// guard gap. That ends the program even in a contained worker (§11.2), so a
// server that runs each connection contained still dies of one: what
// docs/gust/backend_api.md says crash isolation does not cover. Expected to
// abort.
import std;

thread_fn runaway() {
    var s: u8[>..] = [];
    for i in 2000000 { s.push(1); }
    print(s.len);
}

fn main() {
    print("spawning");
    thread_wait(thread_spawn_contained(runaway));
    print("not reached");
}
