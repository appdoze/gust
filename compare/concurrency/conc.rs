use std::sync::{mpsc, Arc, Mutex};
use std::thread;
use std::time::Instant;

const SPAWNS: i64 = 2000;
const ROUNDS: i64 = 100000;
const JOBS: i64 = 1000000;

fn spawn_join() -> f64 {
    let mut sum = 0i64;
    for i in 0..SPAWNS {
        thread::spawn(move || { let _ = i; }).join().unwrap();
        sum += 1;
    }
    sum as f64
}

fn spawn_many() -> f64 {
    let mut sum = 0i64;
    for _ in 0..20 {
        let ids: Vec<_> = (0..100).map(|i| thread::spawn(move || { let _ = i; })).collect();
        for id in ids { id.join().unwrap(); sum += 1; }
    }
    sum as f64
}

fn spawn_work() -> f64 {
    let mut sum = 0i64;
    for x in 0..SPAWNS {
        sum += thread::spawn(move || {
            let mut a = Vec::new();
            for i in 0..1000 { a.push(i + x); }
            a.iter().sum::<i64>()
        }).join().unwrap();
    }
    sum as f64
}

fn pingpong() -> f64 {
    let (to, from_main) = mpsc::channel::<i64>();
    let (back, from_w) = mpsc::channel::<i64>();
    let h = thread::spawn(move || {
        for _ in 0..ROUNDS {
            let p = from_main.recv().unwrap();
            back.send(p + 1).unwrap();
        }
    });
    let mut sum = 0i64;
    for i in 0..ROUNDS {
        to.send(i).unwrap();
        sum += from_w.recv().unwrap();
    }
    h.join().unwrap();
    sum as f64
}

fn fan_out() -> f64 {
    // std's channel has one receiver: shared behind a mutex, as the Rust book does.
    let (tx, rx) = mpsc::channel::<i64>();
    let rx = Arc::new(Mutex::new(rx));
    let (dtx, drx) = mpsc::channel::<i64>();
    let mut ids = Vec::new();
    for _ in 0..3 {
        let rx = rx.clone();
        let dtx = dtx.clone();
        ids.push(thread::spawn(move || {
            let mut sum = 0i64;
            loop {
                let j = rx.lock().unwrap().recv().unwrap();
                if j < 0 { break; }
                sum += j;
            }
            dtx.send(sum).unwrap();
        }));
    }
    for i in 0..JOBS { tx.send(i).unwrap(); }
    for _ in 0..3 { tx.send(-1).unwrap(); }
    let mut sum = 0i64;
    for _ in 0..3 { sum += drx.recv().unwrap(); }
    for id in ids { id.join().unwrap(); }
    sum as f64
}

fn main() {
    let only = std::env::args().nth(2).unwrap_or_default();
    let benches: [(&str, fn() -> f64); 5] = [("spawnjoin", spawn_join), ("spawnmany", spawn_many), ("spawnwork", spawn_work), ("pingpong", pingpong), ("fanout", fan_out)];
    for (name, f) in benches {
        if !only.is_empty() && only != name { continue; }
        let t0 = Instant::now();
        let ck = f();
        println!("bench\t{}\t{}\t{}", name, t0.elapsed().as_secs_f64(), ck);
    }
}
