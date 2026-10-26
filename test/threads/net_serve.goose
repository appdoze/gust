// net::serve (docs/stdlib.md) on worker threads, driven over loopback from
// main: fragmented and pipelined input, a response far larger than any
// buffer, produced in bounded memory and in order with what follows it, a
// slow reader, a client that hangs up mid-response, input past its limit,
// a connection past max_conns, an idle timeout, churn, and a stop with
// connections still open. Then handles across threads, and what the runtime
// closes when a thread program ends by returning or by a contained abort.

import net;

// A line protocol: "echo x" answers x, "big n" n bytes of a pattern, "quit"
// bye and the end of the connection.
struct Lines { left: i64 = 0, at: i64 = 0 }

fn pattern(i: i64) -> u8 { ((i * 7 + i / 251) & 255) as! u8 }

fn lines_d(st: Lines&, ctl: net::Ctl&, input: const u8[:], out: u8[>..]&) -> i64 {
    var nl = -1;
    for i in input.len {
        if input[i] == '\n' { nl = i; break; }
    }
    if nl < 0 { return 0; }
    let line = input[0..nl];
    if starts_with(line, "echo ") {
        out.append(line[5..]);
        out.push('\n');
    } else if starts_with(line, "big ") {
        let n, ok = parse_int(line[4..]);
        st.left = n;
        st.at = 0;
        ctl.produce = ok && n > 0;
    } else if line == "quit" {
        out.append("bye\n");
        ctl.close = true;
    } else if line == "room" {
        format(out, ctl.room, "\n");
    } else {
        out.append("what?\n");
    }
    return nl + 1;
}

fn lines_p(st: Lines&, ctl: net::Ctl&, out: u8[>..]&, room: i64) {
    let n = min(room, st.left);
    for k in n { out.push(pattern(st.at + k)); }
    st.at += n;
    st.left -= n;
    if st.left == 0 { ctl.produce = false; }
}

thread_fn server(l: net::Listener, limits: net::Limits) {
    let p, status = net::poller();
    assert(status == 0);
    qput(net::serve(l, p, net::Conn { .. }, Lines { .. }, limits, net::Replies { busy: "busy\n", .. },
                    1, 1, lines_d, lines_p));
}

// The client's side: one poller in main for every connection it opens.
var hs: i64[>..] = [];
var evs: i64[>..] = [];

fn send_all(p: net::Poller, c: net::Conn, bytes: const u8[:]) -> bool {
    var at = 0;
    while at < bytes.len {
        let n = net::send(p, c, bytes[at..]);
        if n == net::AGAIN { net::wait(p, 10, hs, evs); continue; }
        if n < 0 { return false; }
        at += n;
    }
    return true;
}

// Reads into out until it holds n bytes, the peer closes or ms pass: the
// last recv's result (0: closed).
fn read_n(p: net::Poller, c: net::Conn, out: u8[>..]&, n: i64, ms: i64 = 5000) -> i64 {
    var buf: u8[>..] = [];
    buf.resize(65536, 0);
    let deadline = clock() + ms / 1000.0;
    var last = 1;
    while out.len < n && clock() < deadline {
        let r = net::recv(p, c, buf[0..min(buf.len, n - out.len)]);
        if r == net::AGAIN { net::wait(p, 10, hs, evs); continue; }
        last = r;
        if r <= 0 { break; }
        out.append(buf[0..r]);
    }
    return last;
}

// Waits for the peer to close: true when it did within ms (a reset counts).
fn closes(p: net::Poller, c: net::Conn, ms: i64 = 5000) -> bool {
    var rest: u8[>..] = [];
    let r = read_n(p, c, rest, 1 << 40, ms);
    return r == 0 || r == net::ERROR;
}

fn open(p: net::Poller, port: i64) -> net::Conn {
    let c, status = net::connect(p, "127.0.0.1", port, 2000);
    assert(status == 0);
    return c;
}

fn ask(p: net::Poller, c: net::Conn, line: const u8[:], n: i64) -> u8[>..] {
    var out: u8[>..] = [];
    assert(send_all(p, c, line));
    read_n(p, c, out, n);
    return out;
}

// A one-line answer, read a byte at a time.
fn ask_line(p: net::Poller, c: net::Conn, line: const u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    assert(send_all(p, c, line));
    while out.len == 0 || out[out.len - 1] != '\n' {
        if read_n(p, c, out, out.len + 1) <= 0 { break; }
    }
    return out;
}

fn pattern_ok(bytes: const u8[:], from: i64) -> bool {
    for i in bytes.len {
        if bytes[i] != pattern(from + i) { return false; }
    }
    return true;
}

thread_fn foreign(p: net::Poller, c: net::Conn) {
    var buf: u8[>..] = [];
    buf.resize(8, 0);
    qput(net::count(p));
    qput(net::recv(p, c, buf));
}

// Makes a listener and a poller holding a connection, and leaves both open:
// it returns, or (abort) fails a check.
thread_fn owner(fail: bool) {
    hs.resize(8, 0);    // a thread's own instance of the globals
    evs.resize(8, 0);
    let l, status = net::listen(0, 16, true);
    assert(status == 0);
    let p, ps = net::poller();
    qput(net::port(l));
    var c = net::Conn { .. };
    for tries in 500 {
        let a, accepted = net::accept(p, l);
        if accepted == 0 { c = a; break; }
        net::wait(p, 10, hs, evs);
    }
    qput(c.handle != 0);
    qget<i64>();    // main has seen the connection work
    assert(!fail);
}

fn main() {
    hs.resize(64, 0);
    evs.resize(64, 0);
    let l, ls = net::listen(0, 128, true);
    let port = net::port(l);
    var limits = net::Limits { in_size: 256, out_queue: 65536, out_total: 1048576, idle_ms: 3000,
                               grace_ms: 1000, .. };
    let srv = thread_spawn(server, l, limits);
    let p, ps = net::poller();
    print("listen ", ls, ", poller ", ps);

    let a = open(p, port);
    assert(send_all(p, a, "ec"));
    sleep(0.05);
    var r = ask(p, a, "ho fragmented\n", 11);
    print("fragmented: ", r);
    r = ask(p, a, "echo one\necho two\nnonsense\n", 14);
    print("pipelined: ", r);

    // Eight megabytes against a 64 KB queue, then a line it must answer
    // after the last of them.
    let big = 8000000;
    var got: u8[>..] = [];
    assert(send_all(p, a, "big 8000000\necho after\n"));
    read_n(p, a, got, big + 6);
    print("big: ", got.len == big + 6, ", pattern ", pattern_ok(got[0..big], 0), ", then ",
          got[big..]);

    // A slow reader: 2 MB a little at a time.
    let b = open(p, port);
    assert(send_all(p, b, "big 2000000\n"));
    var slow: u8[>..] = [];
    var bad = false;
    while slow.len < 2000000 {
        var piece: u8[>..] = [];
        if read_n(p, b, piece, min(32768, 2000000 - slow.len)) <= 0 { bad = true; break; }
        if !pattern_ok(piece, slow.len) { bad = true; }
        slow.append(piece);
        sleep(0.002);
    }
    print("slow reader: ", slow.len, " bytes, intact ", !bad);
    print("quit: ", ask(p, b, "quit\n", 4), "closed ", closes(p, b));

    // A client that hangs up 100 KB into 50 MB, and one sending a line
    // longer than in_size.
    let c = open(p, port);
    var part: u8[>..] = [];
    assert(send_all(p, c, "big 50000000\n"));
    read_n(p, c, part, 100000);
    net::close(p, c);
    let d = open(p, port);
    var long: u8[>..] = [];
    for i in 300 { long.push('x'); }
    send_all(p, d, long);
    print("input past in_size closed ", closes(p, d));
    let alive = open(p, port);
    print("still serving: ", ask(p, alive, "echo alive\n", 6), "closing ", net::close(p, alive));

    // Churn: connections opened, used and closed one after another.
    var answered = 0;
    for i in 200 {
        let e = open(p, port);
        if ask(p, e, "echo x\n", 2) == "x\n" { answered++; }
        net::close(p, e);
    }
    print("churn: ", answered, " of 200 answered, poller holds ", net::count(p));

    // max_conns and the idle timeout, on a second server.
    let l2, ls2 = net::listen(0, 16, true);
    let srv2 = thread_spawn(server, l2,
                            net::Limits { max_conns: 1, idle_ms: 300, grace_ms: 1000, .. });
    let f = open(p, net::port(l2));
    print("second server: ", ask(p, f, "echo first\n", 6));
    let g = open(p, net::port(l2));
    var busy: u8[>..] = [];
    read_n(p, g, busy, 5);
    print("past max_conns: ", busy, "closed ", closes(p, g));
    print("idle closed ", closes(p, f, 3000));

    // One output chunk for the whole worker. A client takes 32 MB but reads
    // none of it until the worker's output memory is all its own (past what
    // the kernel's buffers hold, which on Linux is megabytes), as the other
    // connection's room says; then the other asks for a megabyte, which
    // waits, parked, and both are read in turns and get every byte.
    let l3, ls3 = net::listen(0, 16, true);
    let srv3 = thread_spawn(server, l3, net::Limits { out_queue: 16384, out_total: 16384, write_ms: 20000,
                                                      grace_ms: 1000, .. });
    let m1 = open(p, net::port(l3));
    let m2 = open(p, net::port(l3));
    let huge = 33554432;
    assert(send_all(p, m1, "big 33554432\n"));
    var room = ask_line(p, m2, "room\n");
    for tries in 1000 {
        if room == "0\n" { break; }
        sleep(0.01);
        room = ask_line(p, m2, "room\n");
    }
    print("the other connection's room ", room);
    assert(send_all(p, m2, "big 1000000\n"));
    var got1: u8[>..] = [];
    var got2: u8[>..] = [];
    var turns = 0;
    while (got1.len < huge || got2.len < 1000000) && turns < 100000 {
        if got1.len < huge { read_n(p, m1, got1, min(got1.len + 262144, huge), 20); }
        if got2.len < 1000000 { read_n(p, m2, got2, min(got2.len + 8192, 1000000), 20); }
        turns++;
    }
    print("starved worker: ", got1.len, " and ", got2.len, " bytes, intact ",
          pattern_ok(got1, 0) && pattern_ok(got2, 0));

    // A stop with one connection idle and one mid-response, unread.
    let idle = open(p, port);
    print("before the stop: ", ask(p, idle, "echo idle\n", 5));
    let busy_conn = open(p, port);
    assert(send_all(p, busy_conn, "big 50000000\n"));
    sleep(0.1);
    net::request_stop();
    print("idle closed at the stop ", closes(p, idle, 3000));
    print("busy closed within the grace ", closes(p, busy_conn, 5000));
    // The first connection, idle since the slow reader: its idle timeout or
    // the stop closed it.
    print("first closed ", closes(p, a, 100));
    var reclaimed = true;
    var peak = 0;
    var idles = 0;
    var busies = 0;
    var overflows = 0;
    var protocol = 0;
    var parks = 0;
    var accepted = 0;
    for k in 3 {
        let st = qget<net::Stats>();
        reclaimed = reclaimed && st.accepted == st.closed;
        peak = max(peak, st.peak_out);
        idles += st.idle_timeouts;
        busies += st.busy;
        overflows += st.overflows;
        protocol += st.protocol;
        parks += st.parks;
        accepted += st.accepted;
    }
    print("servers: accepted == closed ", reclaimed, ", output memory held ", peak > 0,
          " and within out_total ", peak <= 1048576);
    print("idle timeouts ", idles >= 1, ", busy ", busies, ", overflows ", overflows, ", protocol ",
          protocol, ", accepted ", accepted, ", a producer parked ", parks > 0);
    print("servers returned ", thread_wait(srv), " ", thread_wait(srv2), " ", thread_wait(srv3));

    // Another thread holding this one's handles reaches nothing.
    let h = open(p, port);
    thread_wait(thread_spawn(foreign, p, h));
    print("from another thread: count ", qget<i64>(), ", recv ", qget<i64>());

    // What a thread program leaves open, the runtime closes as it ends.
    for fail in [false, true] {
        let id = thread_spawn_contained(owner, fail);
        let oport = qget<i64>();
        let o = open(p, oport);
        print("owner accepted ", qget<bool>());
        qput(1);
        print("owner ", if fail { "aborted" } else { "returned" }, ": ", thread_wait(id),
              ", its connection closed ", closes(p, o));
        let o2, os = net::connect(p, "127.0.0.1", oport, 500);    // refused, or (Windows) slow to say so
        print("its listener gone ", os < 0);
    }
    print("close ", net::close(l), " ", net::close(l2), " ", net::close(l3), " ", net::close(p));
}
