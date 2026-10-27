// Gust: freeze-then-share (§11.2). Data built at run time is frozen into a
// read-only view of static data, which workers are handed and read in
// place, as they read a string literal: no copy per worker, and no writes.
struct Body { x: f64, y: f64, mass: f64 }
struct Part { from: i64, sum: i64 }

thread_fn summer(table: const i64[:], from: i64, to: i64) {
    var sum = 0;
    for i in from..to { sum += table[i]; }
    qput(Part { from: from, sum: sum });
}

thread_fn weigher(bodies: const Body[:]) {
    var mass = 0.0;
    for b in bodies { mass += b.mass; }
    qput(mass);
}

fn main() {
    // Built at run time, then frozen: the copy is the program's to the end.
    var squares: i64[>..] = [];
    for i in 100000 { squares.push(i * i % 1000); }
    let table = freeze(squares);
    squares[0] = 999;
    squares.clear();
    print(table.len, " ", table[0], " ", table[99999]);       // 100000 0 1

    var ids: i64[>..] = [];
    for w in 4 { ids.push(thread_spawn(summer, table, w * 25000, (w + 1) * 25000)); }
    var total = 0;
    var parts = 0;
    for w in 4 {
        let p = qget<Part>();
        total += p.sum;
        parts += p.from;
    }
    for id in ids { thread_wait(id); }
    var expected = 0;
    for v in table { expected += v; }
    print(total, " ", total == expected, " ", parts);      // 46150000 true 150000

    // A struct's elements, from a fixed array; and one frozen from a view.
    let bodies: Body[3] = [Body { 0.0, 0.0, 1.5 }, Body { 1.0, 0.0, 2.5 }, Body { 0.0, 1.0, 4.0 }];
    let frozen = bodies.freeze();
    thread_wait(thread_spawn(weigher, frozen));
    print(qget<f64>(), " ", frozen[1].x);                     // 8.0 1.0
    let part = freeze(table[10..20]);
    print(part.len, " ", part[0] == table[10]);               // 10 true

    // Nothing to freeze is a view of nothing.
    let none: i64[>..] = [];
    print(freeze(none).len);                                  // 0
}
