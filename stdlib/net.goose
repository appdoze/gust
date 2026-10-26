// The Gust standard library: `net` (docs/stdlib.md): TCP over IPv4, every
// socket non-blocking and waited on with a poller, over the C in
// src/runtime/runtime_net.h (spec §7.10); and `serve`, an event loop for a
// server, which knows bytes and connections and nothing of any protocol.
//
// Handles are checked: a Listener, Poller, Conn or Detached that does not
// name a live object of this thread (closed, reused, from another thread,
// made up) gets BAD_HANDLE from every call, which then touches nothing.
// Ownership: a poller owns the connections in it, and closing it closes
// them; when the thread program that made a listener, a poller or a
// detached connection ends, by returning or by a contained abort, the
// runtime closes it. A poller and its connections belong to one thread;
// a listener can be accepted from on any.
//
// Results: a count or 0 means success; a negative result is one of the
// errors below. recv's 0 means the peer closed its side.
namespace net;

import std;
import os;

const ERROR = -1;          // the OS failed it (os_error() says how); the connection is no good
const AGAIN = -2;          // it would have to wait
const BAD_HANDLE = -3;     // not a live handle of this thread
const LIMIT = -4;          // a table or memory ran out
const ADDRESS = -5;        // not an IPv4 address, or the port is taken
const TIMEOUT = -6;        // a connect did not finish in time
const UNAVAILABLE = -7;    // the platform's sockets could not be started
const INVALID = -8;        // an argument the call does not take

// What a poller waits for and reports.
const READ = 1;
const WRITE = 2;
const HUP = 4;             // reported only: the peer hung up, or the socket failed

struct Listener { handle: i64 = 0 }
struct Poller { handle: i64 = 0 }
struct Conn { handle: i64 = 0 }
struct Detached { handle: i64 = 0 }

// The runtime's calls. Every one checks its handles and takes slices whole,
// so none can reach memory or a socket it was not given.
extern fn rt_listen(port: i64, backlog: i64, local_only: bool) -> i64 = "gs_net_listen";
extern fn rt_listener_port(l: i64) -> i64 = "gs_net_listener_port";
extern fn rt_close_listener(l: i64) -> i64 = "gs_net_close_listener";
extern fn rt_poller() -> i64 = "gs_net_poller";
extern fn rt_close_poller(p: i64) -> i64 = "gs_net_close_poller";
extern fn rt_count(p: i64) -> i64 = "gs_net_count";
extern fn rt_watch(p: i64, l: i64, on: bool) -> i64 = "gs_net_watch";
extern fn rt_accept(p: i64, l: i64) -> i64 = "gs_net_accept";
extern fn rt_listener_open() -> i64 = "gs_net_listener_open";
extern fn rt_connect(p: i64, host: const u8[:], port: i64, timeout_ms: i64) -> i64 = "gs_net_connect";
extern fn rt_recv(p: i64, c: i64, buf: u8[:]) -> i64 = "gs_net_recv";
extern fn rt_send(p: i64, c: i64, bytes: const u8[:]) -> i64 = "gs_net_send";
extern fn rt_want(p: i64, c: i64, events: i64) -> i64 = "gs_net_want";
extern fn rt_shutdown(p: i64, c: i64) -> i64 = "gs_net_shutdown";
extern fn rt_close(p: i64, c: i64) -> i64 = "gs_net_close";
extern fn rt_wait(p: i64, ms: i64, handles: i64[:], events: i64[:]) -> i64 = "gs_net_wait";
extern fn rt_detach(p: i64, c: i64) -> i64 = "gs_net_detach";
extern fn rt_adopt(p: i64, d: i64) -> i64 = "gs_net_adopt";
extern fn os_error() -> i64 = "gs_net_os_error";
// The server's stop flag: set by Ctrl-C, SIGINT or SIGTERM once
// catch_signals has been called, or by request_stop; one flag for the
// whole process, read by every thread.
extern fn catch_signals() = "gs_net_catch_signals";
extern fn request_stop() = "gs_net_request_stop";
extern fn stop_requested() -> bool = "gs_net_stop_requested";

fn error_text(code: i64) -> const u8[:] {
    return match code {
        ERROR => "the operating system failed the call",
        AGAIN => "it would have to wait",
        BAD_HANDLE => "not a live handle of this thread",
        LIMIT => "a table or memory ran out",
        ADDRESS => "not an IPv4 address, or the port is in use",
        TIMEOUT => "timed out",
        UNAVAILABLE => "the platform's sockets could not be started",
        INVALID => "an argument the call does not take",
        _ => if code < 0 { "not an error of net" } else { "no error" },
    };
}

// A listening socket on port (0: one the system picks; port() says which),
// on every interface or, local_only, on 127.0.0.1: the listener, and 0 or
// an error.
fn listen(port: i64, backlog: i64 = 1024, local_only: bool = false) -> Listener, i64 {
    let h = rt_listen(port, backlog, local_only);
    if h < 0 { return Listener { .. }, h; }
    return Listener { handle: h }, 0;
}
fn port(l: Listener) -> i64 { rt_listener_port(l.handle) }
// Only the thread that made a listener closes it; its socket goes when no
// accept and no watching poller still uses it.
fn close(l: Listener) -> i64 { rt_close_listener(l.handle) }

fn poller() -> Poller, i64 {
    let h = rt_poller();
    if h < 0 { return Poller { .. }, h; }
    return Poller { handle: h }, 0;
}
// Closes the poller and every connection in it.
fn close(p: Poller) -> i64 { rt_close_poller(p.handle) }
// How many connections the poller holds.
fn count(p: Poller) -> i64 { rt_count(p.handle) }
// Waits for connections on l too, or (on false) no longer.
fn watch(p: Poller, l: Listener, on: bool = true) -> i64 { rt_watch(p.handle, l.handle, on) }

// A connection from l, in p and waiting to read: the connection, and 0,
// AGAIN when none is waiting, LIMIT when the process is out of descriptors
// (on Linux and macOS the waiting connection is then dropped, so that its
// client hears at once), or an error.
fn accept(p: Poller, l: Listener) -> Conn, i64 {
    let h = rt_accept(p.handle, l.handle);
    if h < 0 { return Conn { .. }, h; }
    return Conn { handle: h }, 0;
}
// After this thread's last accept that returned a connection: how many
// connections accepted from that listener are open, on every thread, the
// new one among them. What workers sharing a listener hold to one limit.
fn accepted_open() -> i64 { rt_listener_open() }
// A connection to host (an IPv4 address such as 127.0.0.1, or localhost)
// and port, in p: waits up to timeout_ms for it to be made.
fn connect(p: Poller, host: const u8[:], port: i64, timeout_ms: i64 = 5000) -> Conn, i64 {
    let h = rt_connect(p.handle, host, port, timeout_ms);
    if h < 0 { return Conn { .. }, h; }
    return Conn { handle: h }, 0;
}
// Bytes into buf: the count, 0 when the peer closed, AGAIN, or an error.
fn recv(p: Poller, c: Conn, buf: u8[:]) -> i64 { rt_recv(p.handle, c.handle, buf) }
// What the socket takes of bytes now: the count (at least 1), AGAIN, or an error.
fn send(p: Poller, c: Conn, bytes: const u8[:]) -> i64 { rt_send(p.handle, c.handle, bytes) }
// What p waits for on c: READ, WRITE, both, or neither (0), which leaves
// it out of every wait until it wants something again.
fn want(p: Poller, c: Conn, events: i64) -> i64 { rt_want(p.handle, c.handle, events) }
// Ends what c sends: the peer reads 0 once it has the rest, and c can
// still read.
fn shutdown(p: Poller, c: Conn) -> i64 { rt_shutdown(p.handle, c.handle) }
fn close(p: Poller, c: Conn) -> i64 { rt_close(p.handle, c.handle) }
// c's index in p: small and dense, for keeping state per connection in an
// array. A connection closed and another accepted may share an index, never
// a handle.
fn index(c: Conn) -> i64 { c.handle & 0xffff }

// Waits up to ms: how many of p's sockets are ready, with each one's handle
// (a connection's, or a watched listener's) and its events (READ, WRITE,
// HUP) in the first entries of handles and events, as many as both hold.
fn wait(p: Poller, ms: i64, handles: i64[:], events: i64[:]) -> i64 {
    return rt_wait(p.handle, ms, handles, events);
}

// Takes c out of p with its socket open, for another thread's poller to
// adopt (a Detached is a flat value: a thread argument or a queue message).
fn detach(p: Poller, c: Conn) -> Detached, i64 {
    let h = rt_detach(p.handle, c.handle);
    if h < 0 { return Detached { .. }, h; }
    return Detached { handle: h }, 0;
}
fn adopt(p: Poller, d: Detached) -> Conn, i64 {
    let h = rt_adopt(p.handle, d.handle);
    if h < 0 { return Conn { .. }, h; }
    return Conn { handle: h }, 0;
}

// --- serve: an event loop for a server -------------------------------------
//
// serve multiplexes connections over one thread and one poller: it accepts
// them, reads what they send, hands it to the protocol's D, sends what D and
// the producer P write, and enforces the limits below. Nothing in it knows a
// protocol. A server is a worker per CPU, each in serve over the same
// listener (examples/api/http.gust's serve_pool), or a thread per
// connection, each in serve over the one connection it adopted.
//
// D(state, ctl, input, out) -> consumed: input is what the connection has
// sent and D has not consumed yet; D appends any reply to out and returns
// how many bytes of input it consumed (0: it needs more). It may set
// ctl.close (end the connection once its output is sent) and ctl.produce
// (call P whenever there is room, until P clears it). D is called again
// while input remains and it consumes some, but not while output is waiting
// or P is producing, so replies go out in order and a client that does not
// read is sent nothing more (backpressure). A reply larger than ctl.room
// goes out only as far as the socket takes the excess at once; past that,
// the connection is closed.
//
// P(state, ctl, out, room) appends at least 1 and at most room bytes of the
// response it is producing, and clears ctl.produce once that is complete.
// It is called when none of the connection's output is waiting and the
// socket can take more, with room at least 16 KB (or out_queue, if that is
// less) and no more than the connection's queue and the worker's output
// memory allow: a response of any size goes out in bounded memory, as fast
// as the client takes it. When the worker has no output memory left, the
// connection waits, out of the poll set, until another frees some.
//
// D and P must not block: the thread is every connection's. state is the
// protocol's per-connection value, of any fixed-size type, a copy of proto
// for each new connection.

// What the loop tells the protocol about a connection, and what the
// protocol asks of the loop.
struct Ctl {
    conn: i64 = 0,           // the connection's number (read only)
    now: f64 = 0.0,          // clock() as the loop last read it (read only)
    room: i64 = 0,           // what the connection may queue now (read only)
    close: bool = false,     // end the connection once its output is sent
    produce: bool = false,   // call P when there is room, until P clears it
    stopping: bool = false,  // a stop was requested: finish what is under way (read only)
    // When the request should be done, its input's first byte plus
    // limits.request_ms, as clock() reads; 0.0 for no limit (read only).
    // The loop enforces nothing: D and P look at it, as a handler in Go
    // looks at its context's deadline.
    deadline: f64 = 0.0,
}

// One worker's limits, and what each costs when broken.
struct Limits {
    max_conns: i64 = 4096,       // open at once; past it, a new one gets replies.busy and is closed
    pool_conns: i64 = 0,         // the same over every worker on the listener (0: no such limit)
    in_size: i64 = 16384,        // a connection's input D has not consumed yet; past it, closed
    out_queue: i64 = 262144,     // a connection's output waiting to be sent; past it, closed
    out_total: i64 = 67108864,   // all of the worker's waiting output, in 16 KB chunks
    idle_ms: i64 = 5000,         // no input pending and nothing to send; closed
    read_ms: i64 = 5000,         // from the first byte of input D has not consumed; replies.timeout, closed
    write_ms: i64 = 5000,        // output waiting and none of it taken; closed
    grace_ms: i64 = 10000,       // after a stop, for connections still busy
    request_ms: i64 = 0,         // what a request may take from its first byte: ctl.deadline (0: none)
    wait_ms: i64 = 50,           // the longest wait, so timeouts and the stop flag are looked at
}

// What the loop sends by itself before it closes a connection, as much as
// the socket takes at once: to one past max_conns or pool_conns, and to one whose input
// timed out. Empty: nothing.
struct Replies { busy: const u8[:] = "", timeout: const u8[:] = "" }

// What the loop did, for the server's totals.
struct Stats {
    accepted: i64 = 0,
    busy: i64 = 0,               // refused past max_conns or pool_conns
    idle_timeouts: i64 = 0,
    read_timeouts: i64 = 0,
    write_timeouts: i64 = 0,
    overflows: i64 = 0,          // input or output past its limit
    protocol: i64 = 0,           // D or P broke its contract
    errors: i64 = 0,             // the OS failed a connection (resets included)
    closed: i64 = 0,             // all of them, however they ended
    bytes_in: i64 = 0,
    bytes_out: i64 = 0,
    peak_out: i64 = 0,           // the most output memory held at once, in whole chunks
    accept_failures: i64 = 0,    // accepts that failed (out of descriptors, say): the listener rested
    parks: i64 = 0,              // times a producer waited for output memory
}

// The loop's own record of a connection.
struct Rec<S> {
    conn: Conn = Conn { .. },     // handle 0: a free entry
    in_slot: i64 = -1,            // input D has not consumed, when more than the read held
    have: i64 = 0,
    started: f64 = 0.0,           // when that input began
    last: f64 = 0.0,              // the last time any byte went either way
    head: i64 = -1,               // the output queue: chunks, oldest first
    tail: i64 = -1,
    head_off: i64 = 0,            // bytes of the head chunk already sent
    tail_len: i64 = 0,            // bytes in the tail chunk
    queued: i64 = 0,
    parked: bool = false,         // producing, out of the poll set until output memory is freed
    ctl: Ctl = Ctl { .. },
    state: S,
}

const CHUNK = 16384;              // an output chunk
const READY = 512;                // events taken from the poller at once

// A slot of size bytes: a free one, or a new one at the end of bytes.
fn take_slot(bytes: u8[>..]&, free: i64[>..<]&, size: i64) -> i64 {
    if free.len > 0 {
        let slot = free[free.len - 1];
        free.pop();
        return slot;
    }
    bytes.resize(bytes.len + size, 0);
    return bytes.len / size - 1;
}

// Serves connections from listener (none: Listener { }) and the one in
// `first` (none: Conn { }), already in poller, until the server stops, or,
// with no listener, until that connection ends. Connections are numbered
// first_id, first_id + id_step, ... Every connection it served is closed
// when it returns.
fn serve<D, P, S>(listener: Listener, poller: Poller, first: Conn, proto: S, limits: Limits,
                  replies: Replies, first_id: i64, id_step: i64) -> Stats {
    var stats = Stats { .. };
    var recs: Rec<S>[>..] = [];
    // The slabs only grow, which lets D's input be a view of a slot: a
    // reference into a grow-shrink array may never be stored (spec §5.2).
    var ins: u8[>..] = [];
    var free_ins: i64[>..<] = [];
    var chunks: u8[>..] = [];
    var chunk_next: i64[>..] = [];
    var free_chunks: i64[>..<] = [];
    var used_chunks = 0;
    let max_chunks = max(1, limits.out_total / CHUNK);
    var scratch: u8[>..] = [];
    scratch.resize(limits.in_size, 0);
    var handles: i64[>..] = [];
    handles.resize(READY, 0);
    var events: i64[>..] = [];
    events.resize(READY, 0);
    var live = 0;
    var parked = 0;
    var next_id = first_id;
    var listening = listener.handle != 0 && watch(poller, listener) == 0;
    var resume_at = 0.0;          // when a resting listener is watched again
    var stop_at = 0.0;

    fn open(c: Conn, now: f64) {
        let i = index(c);
        while recs.len <= i { recs.push(Rec { state: proto, .. }); }
        recs[i] = Rec { conn: c, last: now, state: proto, .. };
        recs[i].ctl.conn = next_id;
        next_id += id_step;
        live += 1;
        stats.accepted += 1;
    }

    fn end(i: i64) {
        close(poller, recs[i].conn);
        if recs[i].in_slot >= 0 { free_ins.push(recs[i].in_slot); }
        var k = recs[i].head;
        while k >= 0 {
            free_chunks.push(k);
            used_chunks -= 1;
            k = chunk_next[k];
        }
        if recs[i].parked { parked -= 1; }
        recs[i].conn = Conn { .. };
        recs[i].in_slot = -1;
        recs[i].head = -1;
        recs[i].tail = -1;
        recs[i].queued = 0;
        recs[i].parked = false;
        live -= 1;
        stats.closed += 1;
    }

    // Bytes the connection may still queue: its own limit and, counting
    // the room in its tail chunk, the chunks the worker has left.
    fn room(i: i64) -> i64 {
        let own = limits.out_queue - recs[i].queued;
        let tail_room = if recs[i].tail >= 0 { CHUNK - recs[i].tail_len } else { 0 };
        return max(0, min(own, tail_room + (max_chunks - used_chunks) * CHUNK));
    }

    fn enqueue(i: i64, bytes: const u8[:]) -> bool {
        if bytes.len > room(i) { return false; }
        var at = 0;
        while at < bytes.len {
            if recs[i].tail < 0 || recs[i].tail_len == CHUNK {
                let k = take_slot(chunks, free_chunks, CHUNK);
                while chunk_next.len <= k { chunk_next.push(-1); }
                chunk_next[k] = -1;
                used_chunks += 1;
                stats.peak_out = max(stats.peak_out, used_chunks * CHUNK);
                if recs[i].tail >= 0 { chunk_next[recs[i].tail] = k; } else { recs[i].head = k; recs[i].head_off = 0; }
                recs[i].tail = k;
                recs[i].tail_len = 0;
            }
            let n = min(bytes.len - at, CHUNK - recs[i].tail_len);
            let base = recs[i].tail * CHUNK + recs[i].tail_len;
            for k in n { chunks[base + k] = bytes[at + k]; }
            recs[i].tail_len += n;
            recs[i].queued += n;
            at += n;
        }
        return true;
    }

    // Sends what the socket takes of the queue: false when the connection ended.
    fn flush(i: i64, now: f64) -> bool {
        while recs[i].head >= 0 {
            let k = recs[i].head;
            let stop = if k == recs[i].tail { recs[i].tail_len } else { CHUNK };
            let n = send(poller, recs[i].conn, chunks[k * CHUNK + recs[i].head_off..k * CHUNK + stop]);
            if n == AGAIN { return true; }
            if n < 0 { stats.errors += 1; end(i); return false; }
            stats.bytes_out += n;
            recs[i].last = now;
            recs[i].head_off += n;
            recs[i].queued -= n;
            if recs[i].head_off == stop {
                recs[i].head = chunk_next[k];
                free_chunks.push(k);
                used_chunks -= 1;
                recs[i].head_off = 0;
                if recs[i].head < 0 { recs[i].tail = -1; recs[i].tail_len = 0; }
            }
        }
        return true;
    }

    // Sends out now what the socket takes, queues the rest: false when the
    // connection ended (an error, or output past its limit).
    fn emit(i: i64, out: const u8[:], now: f64) -> bool {
        var sent = 0;
        if recs[i].queued == 0 {
            while sent < out.len {
                let n = send(poller, recs[i].conn, out[sent..]);
                if n == AGAIN { break; }
                if n < 0 { stats.errors += 1; end(i); return false; }
                sent += n;
                stats.bytes_out += n;
                recs[i].last = now;
            }
        }
        if sent < out.len && !enqueue(i, out[sent..]) {
            stats.overflows += 1;
            end(i);
            return false;
        }
        return true;
    }

    // What the connection waits for now: to send while output is waiting or
    // P is producing, else to read; nothing while it is parked.
    fn rearm(i: i64) {
        if recs[i].parked { return; }
        let busy = recs[i].queued > 0 || recs[i].ctl.produce;
        want(poller, recs[i].conn, if busy { WRITE } else { READ });
    }

    // P's next output, while there is room: false when the connection ended.
    fn produce(i: i64, now: f64) -> bool {
        while recs[i].ctl.produce {
            // The next piece once the last has gone to the socket: a stream
            // holds no more than what the socket did not take of one.
            if recs[i].queued > 0 { return true; }
            let r = room(i);
            if r == 0 {
                // Other connections hold all of the worker's output memory.
                recs[i].parked = true;
                stats.parks += 1;
                parked += 1;
                want(poller, recs[i].conn, 0);
                return true;
            }
            var out: u8[>..] = [];
            recs[i].ctl.now = now;
            recs[i].ctl.room = r;
            recs[i].ctl.stopping = stop_at > 0.0;
            P(recs[i].state, recs[i].ctl, out, r);
            if out.len > r || (out.len == 0 && recs[i].ctl.produce) {
                stats.protocol += 1;
                end(i);
                return false;
            }
            if !emit(i, out, now) { return false; }
        }
        return true;
    }

    // Hands D the input waiting in buf, while it consumes some and nothing
    // holds it back: the bytes consumed, or -1 when the connection ended.
    // `since` is when that input began to arrive, which a request's deadline
    // counts from.
    fn feed(i: i64, buf: const u8[:], now: f64, since: f64) -> i64 {
        var used = 0;
        while used < buf.len && recs[i].queued == 0 && !recs[i].ctl.produce && !recs[i].ctl.close {
            var out: u8[>..] = [];
            recs[i].ctl.now = now;
            recs[i].ctl.room = room(i);
            recs[i].ctl.stopping = stop_at > 0.0;
            recs[i].ctl.deadline = if limits.request_ms > 0 { since + limits.request_ms / 1000.0 } else { 0.0 };
            let n = D(recs[i].state, recs[i].ctl, buf[used..], out);
            if n < 0 || n > buf.len - used { stats.protocol += 1; end(i); return -1; }
            if out.len > 0 && !emit(i, out, now) { return -1; }
            if recs[i].ctl.produce && !produce(i, now) { return -1; }
            if n == 0 { break; }
            used += n;
        }
        return used;
    }

    // After any change: the connection's next state. False when it ended.
    fn settle(i: i64) -> bool {
        if recs[i].ctl.close && recs[i].queued == 0 && !recs[i].ctl.produce {
            end(i);
            return false;
        }
        rearm(i);
        return true;
    }

    // Input waiting in the slot, once nothing holds D back.
    fn drain_slot(i: i64, now: f64) {
        let slot = recs[i].in_slot;
        let base = slot * limits.in_size;
        let used = feed(i, ins[base..base + recs[i].have], now, recs[i].started);
        if used < 0 { return; }
        let left = recs[i].have - used;
        for k in left { ins[base + k] = ins[base + used + k]; }
        recs[i].have = left;
        if left == 0 {
            free_ins.push(slot);
            recs[i].in_slot = -1;
        } else if used > 0 {
            recs[i].started = now;
        }
        settle(i);
    }

    fn readable(i: i64, now: f64) {
        if recs[i].in_slot >= 0 {
            let base = recs[i].in_slot * limits.in_size;
            if recs[i].have == limits.in_size { stats.overflows += 1; end(i); return; }
            let n = recv(poller, recs[i].conn, ins[base + recs[i].have..base + limits.in_size]);
            if n == AGAIN { return; }
            if n <= 0 {
                if n < 0 { stats.errors += 1; }
                end(i);
                return;
            }
            stats.bytes_in += n;
            recs[i].have += n;
            recs[i].last = now;
            drain_slot(i, now);
            return;
        }
        // Nothing pending: read into the scratch buffer, and keep only what
        // D has not consumed.
        let n = recv(poller, recs[i].conn, scratch);
        if n == AGAIN { return; }
        if n <= 0 {
            if n < 0 { stats.errors += 1; }
            end(i);
            return;
        }
        stats.bytes_in += n;
        recs[i].last = now;
        let used = feed(i, scratch[0..n], now, now);
        if used < 0 { return; }
        if used < n {
            let slot = take_slot(ins, free_ins, limits.in_size);
            let base = slot * limits.in_size;
            for k in n - used { ins[base + k] = scratch[used + k]; }
            recs[i].in_slot = slot;
            recs[i].have = n - used;
            recs[i].started = now;
        }
        settle(i);
    }

    fn writable(i: i64, now: f64) {
        if !flush(i, now) { return; }
        if recs[i].ctl.produce && !produce(i, now) { return; }
        if recs[i].queued == 0 && !recs[i].ctl.produce && recs[i].in_slot >= 0 && !recs[i].ctl.close {
            drain_slot(i, now);
            return;
        }
        settle(i);
    }

    fn rest(now: f64, secs: f64) {
        watch(poller, listener, false);
        listening = false;
        resume_at = now + secs;
    }

    // At most 32 a turn, so the other workers on a shared listener, woken
    // by the same readiness, take their share. They are seldom even, since
    // the busier a worker the less often it gets to the listener: hence
    // pool_conns, a limit over all of them.
    fn accept_all(now: f64) {
        var failed = 0;
        var taken = 0;
        while taken < 32 {
            let c, status = accept(poller, listener);
            if status == AGAIN || status == BAD_HANDLE { return; }    // none left, or no listener
            if status != 0 {
                // Out of descriptors or memory, or the OS failed it. On Linux
                // and macOS each such accept has dropped a waiting connection,
                // so a few more drain the queue; then the listener rests a
                // moment, rather than wake the loop again at once, ready with
                // nothing it can take.
                stats.accept_failures += 1;
                failed += 1;
                if failed < 64 { continue; }
                rest(now, 0.1);
                return;
            }
            taken += 1;
            if live >= limits.max_conns || (limits.pool_conns > 0 && accepted_open() > limits.pool_conns) {
                send(poller, c, replies.busy);    // what fits; it is closed either way
                close(poller, c);
                stats.busy += 1;
                continue;
            }
            open(c, now);
        }
    }

    // Timeouts: output the client does not take, input it does not finish,
    // a connection it leaves idle; and, once stopping, every idle connection.
    fn sweep(now: f64) {
        for i in recs.len {
            if recs[i].conn.handle == 0 { continue; }
            if recs[i].queued > 0 {
                if now - recs[i].last > limits.write_ms / 1000.0 { stats.write_timeouts += 1; end(i); }
            } else if recs[i].ctl.produce {
                // Producing as the socket takes it, or parked.
            } else if recs[i].in_slot >= 0 {
                if now - recs[i].started > limits.read_ms / 1000.0 {
                    send(poller, recs[i].conn, replies.timeout);
                    stats.read_timeouts += 1;
                    end(i);
                }
            } else if stop_at > 0.0 {
                end(i);
            } else if now - recs[i].last > limits.idle_ms / 1000.0 {
                stats.idle_timeouts += 1;
                end(i);
            }
        }
    }

    if first.handle != 0 { open(first, clock()); }
    var last_sweep = clock();
    loop {
        let n = wait(poller, limits.wait_ms, handles, events);
        let now = clock();
        if n < 0 { stop_at = now; }    // the poller failed: everything ends
        for k in n {
            let h = handles[k];
            if h == listener.handle {
                if listening && (events[k] & HUP) == 0 { accept_all(now); }
                else { listening = false; }
                continue;
            }
            let i = index(Conn { handle: h });
            // An event for a connection closed earlier in this batch has a
            // handle no record holds any more.
            if i >= recs.len || recs[i].conn.handle != h { continue; }
            if recs[i].queued > 0 || recs[i].ctl.produce { writable(i, now); } else { readable(i, now); }
        }
        // Output memory freed: the parked producers try again.
        if parked > 0 && used_chunks < max_chunks {
            for i in recs.len {
                if recs[i].conn.handle != 0 && recs[i].parked {
                    recs[i].parked = false;
                    parked -= 1;
                    rearm(i);
                }
            }
        }
        if !listening && resume_at > 0.0 && now >= resume_at && stop_at == 0.0 {
            listening = watch(poller, listener) == 0;
            resume_at = 0.0;
        }
        if stop_at == 0.0 && stop_requested() { stop_at = now; }
        if stop_at > 0.0 && listening {
            watch(poller, listener, false);
            listening = false;
        }
        if stop_at > 0.0 || now - last_sweep >= 0.1 {
            sweep(now);
            last_sweep = now;
        }
        let alone = listener.handle == 0 && live == 0;
        if alone || n < 0 || (stop_at > 0.0 && (live == 0 || now - stop_at > limits.grace_ms / 1000.0)) {
            for i in recs.len {
                if recs[i].conn.handle != 0 { end(i); }
            }
            break;
        }
    }
    if listening { watch(poller, listener, false); }
    return stats;
}
