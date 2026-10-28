// Gust: a stress of net_serve.goose's starved worker (test/threads), for
// the corruption seen on loaded Windows runners, where a reply arrived at
// its full length with zeros in it. The scenario runs over and over against
// one worker, and each piece is checked as recv hands it over, so that a
// wrong byte says whether it came over the socket or went wrong in the
// client's memory afterwards. Prints one line a round; exits 1 at the first
// corruption, with what it found. What it found was the client's memory:
// 16 zero bytes in every 32 from the start of a newly committed chunk, a
// vector copy resumed after its commit fault with its AVX state zero, which
// runtime.h's GS_TOUCHED keeps from happening. Run by
// .github/workflows/stress.yml, not by the suite.

import net;
import os;

struct Lines { left: i64 = 0, at: i64 = 0 }

fn pattern(i: i64) -> u8 { ((i * 7 + i / 251) & 255) as! u8 }

fn lines_d(st: Lines&, ctl: net::Ctl&, input: const u8[:], out: u8[>..]&) -> i64 {
    var nl = -1;
    for i in input.len {
        if input[i] == '\n' { nl = i; break; }
    }
    if nl < 0 { return 0; }
    let line = input[0..nl];
    if starts_with(line, "big ") {
        let n, ok = parse_int(line[4..]);
        st.left = n;
        st.at = 0;
        ctl.produce = ok && n > 0;
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
    qput(net::serve(l, p, net::Conn { .. }, Lines { .. }, limits, net::Replies { .. },
                    1, 1, lines_d, lines_p));
}

var hs: i64[>..] = [];
var evs: i64[>..] = [];
// The first piece that arrived wrong, as recv handed it over: where it
// starts in its stream, its length and the wrong byte's place in it (-1:
// none yet); and where each of the second stream's pieces began.
var wire_at = -1;
var wire_len = 0;
var wire_k = 0;
var wire_got: u8[>..] = [];
var starts: i64[>..] = [];

// Where the bytes are (net_starved_probe.c).
extern fn starved_addr(bytes: const u8[:]) -> i64;

// Bytes from..to of got, in hex, those not the pattern's marked with *.
fn dump(got: const u8[:], from: i64, to: i64) {
    var line: u8[>..] = [];
    for i in max(0, from)..min(to, got.len) {
        if i % 16 == 0 { format(line, "\n    +", i, ":"); }
        let b = got[i] as i64;
        let hex = "0123456789abcdef";
        line.push(' ');
        line.push(hex[b >> 4]);
        line.push(hex[b & 15]);
        line.push(if got[i] != pattern(i) { '*' } else { ' ' });
    }
    print("  bytes", line);
}

fn first_wrong(bytes: const u8[:], from: i64) -> i64 {
    for i in bytes.len {
        if bytes[i] != pattern(from + i) { return i; }
    }
    return -1;
}

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

// net_serve.goose's read_n, checking each piece of a pattern stream (from
// the stream's offset out.len) before it is appended.
fn read_n(p: net::Poller, c: net::Conn, out: u8[>..]&, n: i64, ms: i64, check: bool, log: bool) -> i64 {
    var buf: u8[>..] = [];
    buf.resize(65536, 0);
    let deadline = clock() + ms / 1000.0;
    var last = 1;
    while out.len < n && clock() < deadline {
        let r = net::recv(p, c, buf[0..min(buf.len, n - out.len)]);
        if r == net::AGAIN { net::wait(p, 10, hs, evs); continue; }
        last = r;
        if r <= 0 { break; }
        if check && wire_at < 0 {
            let k = first_wrong(buf[0..r], out.len);
            if k >= 0 {
                wire_at = out.len;
                wire_len = r;
                wire_k = k;
                wire_got.append(buf[k..min(r, k + 16)]);
            }
        }
        if log { starts.push(out.len); }
        out.append(buf[0..r]);
    }
    return last;
}

fn ask_line(p: net::Poller, c: net::Conn, line: const u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    assert(send_all(p, c, line));
    while out.len == 0 || out[out.len - 1] != '\n' {
        if read_n(p, c, out, out.len + 1, 5000, false, false) <= 0 { break; }
    }
    return out;
}

fn open(p: net::Poller, port: i64) -> net::Conn {
    let c, status = net::connect(p, "127.0.0.1", port, 2000);
    assert(status == 0);
    return c;
}

// The wrong stretch of bytes from at: how long it is, and whether what
// follows it is the pattern again at the same offsets (bytes overwritten in
// place) or not.
fn report(name: const u8[:], got: const u8[:], at: i64) {
    var run = 0;
    var zeros = true;
    while at + run < got.len && got[at + run] != pattern(at + run) {
        if got[at + run] != 0 { zeros = false; }
        run++;
    }
    let after = if at + run < got.len { first_wrong(got[at + run..], at + run) } else { -1 };
    print("  ", name, ": first wrong byte ", at, ", a run of ", run, " wrong bytes, all zero ", zeros,
          ", the rest right ", after < 0);
    if after >= 0 { print("  ", name, ": wrong again at ", at + run + after); }
}

fn main() {
    hs.resize(64, 0);
    evs.resize(64, 0);
    let av = args();
    var rounds = 20;
    if av.len > 1 {
        let v, ok = parse_int(av[1]);
        if ok { rounds = v; }
    }
    let l3, ls3 = net::listen(0, 16, true);
    assert(ls3 == 0);
    let srv3 = thread_spawn(server, l3, net::Limits { out_queue: 16384, out_total: 16384, write_ms: 20000,
                                                      grace_ms: 1000, .. });
    let p, ps = net::poller();
    assert(ps == 0);
    let huge = 33554432;
    let second = 1000000;
    for round in rounds {
        let m1 = open(p, net::port(l3));
        let m2 = open(p, net::port(l3));
        assert(send_all(p, m1, "big 33554432\n"));
        var room = ask_line(p, m2, "room\n");
        for tries in 1000 {
            if room == "0\n" { break; }
            sleep(0.01);
            room = ask_line(p, m2, "room\n");
        }
        assert(send_all(p, m2, "big 1000000\n"));
        var got1: u8[>..] = [];
        var got2: u8[>..] = [];
        starts.resize(0, 0);
        var turns = 0;
        while (got1.len < huge || got2.len < second) && turns < 100000 {
            if got1.len < huge { read_n(p, m1, got1, min(got1.len + 262144, huge), 20, true, false); }
            if got2.len < second { read_n(p, m2, got2, min(got2.len + 8192, second), 20, true, true); }
            turns++;
        }
        let wrong1 = first_wrong(got1, 0);
        let wrong2 = first_wrong(got2, 0);
        print("round ", round, ": ", got1.len, " and ", got2.len, " bytes after ", turns, " turns, starved ",
              room == "0\n", ", intact ", wrong1 < 0 && wrong2 < 0, ", on the wire ", wire_at < 0);
        if wrong1 >= 0 || wrong2 >= 0 || wire_at >= 0 {
            if wire_at >= 0 {
                print("  on the wire: a piece of ", wire_len, " bytes from ", wire_at, " wrong at its byte ",
                      wire_k, " (stream offset ", wire_at + wire_k, "): ", wire_got);
            } else {
                print("  every piece was right as recv handed it over: the client's memory changed after");
            }
            if wrong1 >= 0 { report("first", got1, wrong1); }
            let a1 = starved_addr(got1);
            let a2 = starved_addr(got2);
            print("  first at ", a1, " (64 KB boundary + ", a1 % 65536, "), second at ", a2, " (+ ", a2 % 65536,
                  ")");
            if wrong1 >= 0 { dump(got1, wrong1 - wrong1 % 16, wrong1 - wrong1 % 16 + 96); }
            if wrong2 >= 0 {
                report("second", got2, wrong2);
                dump(got2, wrong2 - wrong2 % 16 - 16, wrong2 - wrong2 % 16 + 96);
                // The pieces around it.
                var j = 0;
                while j < starts.len && starts[j] <= wrong2 { j++; }
                let lo = max(0, j - 3);
                var line: u8[>..] = [];
                for q in lo..min(starts.len, j + 2) { format(line, " ", starts[q]); }
                print("  second's pieces began at", line);
            }
            exit(1);
        }
        net::close(p, m1);
        net::close(p, m2);
    }
    net::request_stop();
    print("served: ", qget<net::Stats>().bytes_out > 0);
}
