// net (docs/stdlib.md) on one thread over loopback: a listener, a poller
// holding both ends of each connection, bytes both ways through partial
// reads and writes, a peer's close, detach and adopt, and every handle that
// does not name a live object answering BAD_HANDLE.

import net;

// Waits until an accept on l succeeds: the connect's other end.
fn accept_one(p: net::Poller, l: net::Listener) -> net::Conn {
    var hs: i64[>..] = [];
    hs.resize(4, 0);
    var evs: i64[>..] = [];
    evs.resize(4, 0);
    for tries in 200 {
        let c, status = net::accept(p, l);
        if status == 0 { return c; }
        assert(status == net::AGAIN);
        net::wait(p, 10, hs, evs);
    }
    abort("no connection arrived");
}

fn pattern(i: i64) -> u8 { ((i * 7 + i / 251) & 255) as! u8 }

fn main() {
    let l, le = net::listen(0, 16, true);
    let port = net::port(l);
    print("listen ", le, ", port picked ", port > 0);
    let p, pe = net::poller();
    print("poller ", pe, ", count ", net::count(p));
    print("watch ", net::watch(p, l), ", again ", net::watch(p, l));

    let c, ce = net::connect(p, "127.0.0.1", port, 2000);
    print("connect ", ce);
    // The watched listener reports the waiting connection.
    var hs: i64[>..] = [];
    hs.resize(8, 0);
    var evs: i64[>..] = [];
    evs.resize(8, 0);
    var seen = false;
    for tries in 100 {
        let n = net::wait(p, 20, hs, evs);
        for k in n {
            if hs[k] == l.handle && (evs[k] & net::READ) != 0 { seen = true; }
        }
        if seen { break; }
    }
    print("listener ready ", seen);
    let s = accept_one(p, l);
    print("accepted, count ", net::count(p), ", distinct ", s.handle != c.handle, ", open from the listener ",
          net::accepted_open());
    print("unwatch ", net::watch(p, l, false));

    // Four megabytes one way: the client sends what the socket takes, the
    // server reads what has come, each when the poller says it may.
    let total = 4194304;
    var data: u8[>..] = [];
    for i in total { data.push(pattern(i)); }
    var buf: u8[>..] = [];
    buf.resize(65536, 0);
    var sent = 0;
    var got = 0;
    var bad = 0;
    net::want(p, c, net::READ | net::WRITE);
    while got < total {
        let n = net::wait(p, 1000, hs, evs);
        assert(n > 0);
        for k in n {
            if hs[k] == c.handle && (evs[k] & net::WRITE) != 0 && sent < total {
                let w = net::send(p, c, data[sent..]);
                if w != net::AGAIN {
                    assert(w > 0);
                    sent += w;
                    if sent == total { net::want(p, c, net::READ); }
                }
            }
            if hs[k] == s.handle && (evs[k] & net::READ) != 0 {
                let r = net::recv(p, s, buf);
                if r != net::AGAIN {
                    assert(r > 0);
                    for i in r {
                        if buf[i] != pattern(got + i) { bad++; }
                    }
                    got += r;
                }
            }
        }
    }
    print("four megabytes across: ", got == total, ", mismatched bytes ", bad);

    // And a reply the other way.
    print("reply sent ", net::send(p, s, "done"));
    var reply = 0;
    for tries in 100 {
        let r = net::recv(p, c, buf);
        if r > 0 { reply = r; break; }
        net::wait(p, 20, hs, evs);
    }
    print("reply ", buf[0..reply]);

    // The client ends what it sends: the server reads 0, and can still
    // answer.
    print("shutdown ", net::shutdown(p, c));
    var end = net::AGAIN;
    for tries in 100 {
        end = net::recv(p, s, buf);
        if end != net::AGAIN { break; }
        net::wait(p, 20, hs, evs);
    }
    print("server reads ", end, " once the peer shut down; answers ", net::send(p, s, "late"));
    var late = 0;
    for tries in 100 {
        late = net::recv(p, c, buf);
        if late > 0 { break; }
        net::wait(p, 20, hs, evs);
    }
    print("the client still reads: ", buf[0..late]);

    // The client hangs up: its handle is dead.
    print("close ", net::close(p, c), ", again ", net::close(p, c), ", shutdown ",
          net::shutdown(p, c));
    print("closed handle: send ", net::send(p, c, "x"), ", recv ", net::recv(p, c, buf),
          ", want ", net::want(p, c, net::READ));
    // Waiting for neither takes a connection out of the waits, input
    // pending or not, until it waits for something again.
    print("want neither ", net::want(p, s, 0), ", something else ", net::want(p, s, 4));
    let c3, ce3 = net::connect(p, "127.0.0.1", port, 2000);
    let s4 = accept_one(p, l);
    net::send(p, c3, "ping");
    net::want(p, s4, 0);
    var reported = false;
    for tries in 5 {
        let n = net::wait(p, 20, hs, evs);
        for k in n {
            if hs[k] == s4.handle { reported = true; }
        }
    }
    print("a connection waiting for nothing is reported ", reported);
    net::want(p, s4, net::READ);
    for tries in 100 {
        let n = net::wait(p, 20, hs, evs);
        for k in n {
            if hs[k] == s4.handle { reported = true; }
        }
        if reported { break; }
    }
    let pinged = net::recv(p, s4, buf);
    print("again once it waits to read ", reported, ", reads ", pinged);
    net::close(p, c3);
    net::close(p, s4);
    print("count ", net::count(p));

    // detach and adopt: the connection leaves one poller for another.
    let c2, ce2 = net::connect(p, "localhost", port, 2000);
    let s2 = accept_one(p, l);
    let q, qe = net::poller();
    let d, de = net::detach(p, s2);
    print("connect ", ce2, ", poller ", qe, ", detach ", de, ", count ", net::count(p));
    print("detached conn: send ", net::send(p, s2, "x"));
    let s3, ae = net::adopt(q, d);
    let d2, ae2 = net::adopt(q, d);
    print("adopt ", ae, ", adopt twice ", ae2, ", count ", net::count(q));
    print("sent through the adopted end ", net::send(q, s3, "moved"));
    var moved = 0;
    for tries in 100 {
        moved = net::recv(p, c2, buf);
        if moved > 0 { break; }
        net::wait(p, 20, hs, evs);
    }
    print("received ", buf[0..moved]);
    print("a connection of another poller: send ", net::send(p, s3, "x"));

    // Addresses and ports.
    let x1, ae3 = net::connect(p, "1.2.3", port, 100);
    let x2, ae4 = net::connect(p, "example.com", port, 100);
    let x3, ae5 = net::connect(p, "127.0.0.1", 70000, 100);
    print("bad addresses ", ae3, " ", ae4, " ", ae5);
    let l2, le2 = net::listen(port, 16, true);
    print("a port in use ", le2);
    let l3, le3 = net::listen(-1, 16, true);
    print("a negative port ", le3);

    // Handles that name nothing.
    let none = net::Poller { .. };
    let made_up = net::Poller { handle: p.handle ^ 65536 };    // another generation
    print("a default poller ", net::count(none), ", a made-up one ", net::count(made_up));
    print("a listener as a poller ", net::count(net::Poller { handle: l.handle }));
    print("a poller as a listener ", net::port(net::Listener { handle: p.handle }));
    print("a default conn ", net::recv(p, net::Conn { .. }, buf));

    // Closing the poller closes its connections; its handles die with it.
    print("close poller ", net::close(p), ", again ", net::close(p));
    print("its conn ", net::recv(p, s, buf), ", count ", net::count(p));
    var ended = net::AGAIN;
    for tries in 100 {
        ended = net::recv(q, s3, buf);
        if ended != net::AGAIN { break; }
        var qh: i64[>..] = [];
        qh.resize(2, 0);
        var qe2: i64[>..] = [];
        qe2.resize(2, 0);
        net::wait(q, 20, qh, qe2);
    }
    print("the adopted end sees its peer gone: ", ended == 0 || ended == net::ERROR);
    print("an empty wait slice ", net::wait(q, 0, hs[0..0], evs));
    print("close q ", net::close(q));
    print("close listener ", net::close(l), ", again ", net::close(l), ", port ", net::port(l));
    print("wait on a closed poller ", net::wait(q, 0, hs, evs));
    print(net::error_text(net::BAD_HANDLE), "; ", net::error_text(net::AGAIN), "; ",
          net::error_text(0));
}
