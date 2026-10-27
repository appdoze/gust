"""Gust: `gust cimport` on real libraries, where their headers are installed
(Linux, with the -dev packages): SQLite, through prepared statements and
through sqlite3_exec's row callback; libcurl, fetching from a local HTTP
server through its write callback and its variadic curl_easy_setopt; and
OpenSSL's libcrypto, SHA-256 and HMAC as Python's hashlib computes them. No
program here has a hand-written extern fn. A library whose header is not
installed is skipped; check_cimport_libs returns the ones that ran."""
import hashlib
import hmac
import http.server
import subprocess
import sys
import threading
from pathlib import Path

import toolchain as tc

INCLUDE = ["/usr/include", "/usr/include/x86_64-linux-gnu", "/usr/include/aarch64-linux-gnu"]

SQLITE = r'''
import sqlite3 from "./sqlite3";
import ffi;

var rows: u8[>..] = [];

// The i-th pointer of a C array of them.
fn nth(p: u64, i: i64) -> u64 {
    let b = ffi::bytes(p + (i * 8) as u64, 8);
    var v: u64 = 0;
    for k in 8 { v |= (b[k] as u64) << (8 * k); }
    return v;
}

// sqlite3_exec's row callback: each column's name and its value as text.
export fn row(user: u64, n: i32, values: u64, names: u64) -> i32 {
    for i in n {
        if i > 0 { rows.append(" "); }
        rows.append(ffi::cstr(nth(names, i)));
        rows.append("=");
        rows.append(ffi::cstr(nth(values, i)));
    }
    rows.append(";");
    return 0;
}

fn add(stmt: u64, name: const u8[:], year: i64) {
    // SQLITE_TRANSIENT: SQLite copies the text, which the glue hands it
    // only for the call.
    sqlite3::sqlite3_bind_text(stmt, 1, name, -1, sqlite3::SQLITE_TRANSIENT);
    sqlite3::sqlite3_bind_int64(stmt, 2, year);
    print(sqlite3::sqlite3_step(stmt) == sqlite3::SQLITE_DONE);
    sqlite3::sqlite3_reset(stmt);
}

fn main() {
    print(sqlite3::sqlite3_libversion_str() == sqlite3::SQLITE_VERSION);
    var db: u64 = 0;
    print(sqlite3::sqlite3_open(":memory:", db) == sqlite3::SQLITE_OK);
    var err: u64 = 0;
    print(sqlite3::sqlite3_exec(db, "create table langs (name text, year integer)", 0, 0, err) == sqlite3::SQLITE_OK);
    var stmt: u64 = 0;
    var tail: u64 = 0;
    sqlite3::sqlite3_prepare_v2(db, "insert into langs values (?, ?)", -1, stmt, tail);
    add(stmt, "Goose", 2024);
    add(stmt, "Gust", 2026);
    add(stmt, "Rust", 2015);
    sqlite3::sqlite3_finalize(stmt);
    sqlite3::sqlite3_prepare_v2(db, "select name, year from langs where year > ? order by year", -1, stmt, tail);
    sqlite3::sqlite3_bind_int64(stmt, 1, 2020);
    while sqlite3::sqlite3_step(stmt) == sqlite3::SQLITE_ROW {
        print(ffi::cstr(sqlite3::sqlite3_column_text(stmt, 0)), " ", sqlite3::sqlite3_column_int64(stmt, 1));
    }
    sqlite3::sqlite3_finalize(stmt);
    sqlite3::sqlite3_exec(db, "select name, year * 2 as twice from langs order by name", c_callback(row), 0, err);
    print(rows);
    // A mistake, which SQLite reports with a message it allocated.
    print(sqlite3::sqlite3_exec(db, "select name from nowhere", 0, 0, err) == sqlite3::SQLITE_ERROR);
    print(ffi::cstr(err));
    sqlite3::sqlite3_free(err);
    print(sqlite3::sqlite3_close(db) == sqlite3::SQLITE_OK);
}
'''

SQLITE_OUT = '''true
true
true
true
true
true
Goose 2024
Gust 2026
name=Goose twice=4048;name=Gust twice=4052;name=Rust twice=4030;
true
no such table: nowhere
true
'''

CURL = r'''
import curl from "./curl";
import ffi;
import os;
import std;

var body: u8[>..] = [];
var calls = 0;

// libcurl's write callback: size * n bytes at p, handed over as they come.
export fn take(p: u64, size: u64, n: u64, user: u64) -> u64 {
    body.append(ffi::bytes(p, (size * n) as i64));
    calls += 1;
    return size * n;
}

fn main() {
    let a = args();
    print(curl::curl_global_init(curl::CURL_GLOBAL_DEFAULT) == curl::CURLE_OK);
    let h = curl::curl_easy_init();
    print(h != 0);
    curl::curl_easy_setopt_str(h, curl::CURLOPT_URL, a[1]);
    curl::curl_easy_setopt_ptr(h, curl::CURLOPT_WRITEFUNCTION, c_callback(take));
    curl::curl_easy_setopt_int(h, curl::CURLOPT_TIMEOUT, 30);
    let rc = curl::curl_easy_perform(h);
    print(rc == curl::CURLE_OK, " ", curl::curl_easy_strerror_str(rc));
    var code: i64[1] = [0];
    curl::curl_easy_getinfo_out(h, curl::CURLINFO_RESPONSE_CODE, code);
    print(code[0], " ", body.len, " ", calls > 1, " ", body[..19]);
    curl::curl_easy_cleanup(h);
    curl::curl_global_cleanup();
}
'''

CURL_BODY = b"gust " * 40000
CURL_OUT = f'''true
true
true No error
200 {len(CURL_BODY)} true gust gust gust gust
'''

CRYPTO_H = '''#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
'''

CRYPTO = r'''
import crypto from "./crypto";
import std;

fn hex(b: const u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    let digits = "0123456789abcdef";
    for x in b {
        out.push(digits[x >> 4]);
        out.push(digits[x & 15]);
    }
    return out;
}

fn main() {
    let text = "the quick brown fox jumps over the lazy dog";
    var md: u8[32] = [0; 32];
    crypto::SHA256(text, text.len as u64, md);
    print(hex(md));
    // The same through EVP, in two updates.
    let ctx = crypto::EVP_MD_CTX_new();
    var md2: u8[32] = [0; 32];
    var n: u32[1] = [0];
    print(crypto::EVP_DigestInit_ex(ctx, crypto::EVP_sha256(), 0) == 1);
    crypto::EVP_DigestUpdate(ctx, text[..10], 10);
    crypto::EVP_DigestUpdate(ctx, text[10..], (text.len - 10) as u64);
    crypto::EVP_DigestFinal_ex(ctx, md2, n);
    crypto::EVP_MD_CTX_free(ctx);
    print(n[0], " ", hex(md2) == hex(md));
    var mac: u8[32] = [0; 32];
    var maclen: u32[1] = [0];
    crypto::HMAC(crypto::EVP_sha256(), "key", 3, text, text.len as u64, mac, maclen);
    print(maclen[0], " ", hex(mac));
    var rnd: u8[16] = [0; 16];
    print(crypto::RAND_bytes(rnd, 16) == 1);
    print(starts_with(crypto::OpenSSL_version_str(crypto::OPENSSL_VERSION), "OpenSSL "));
}
'''

TEXT = b"the quick brown fox jumps over the lazy dog"
CRYPTO_OUT = f'''{hashlib.sha256(TEXT).hexdigest()}
true
32 true
32 {hmac.new(b"key", TEXT, hashlib.sha256).hexdigest()}
true
true
'''


def find(rel):
    for d in INCLUDE:
        p = Path(d) / rel
        if p.exists():
            return p
    return None


class Reply(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(CURL_BODY)))
        self.end_headers()
        self.wfile.write(CURL_BODY)

    def log_message(self, *args):
        pass


def check_cimport_libs(exe, cc, base):
    """The libraries that ran; raises RuntimeError on a failure. `cc` None
    skips them all: the glue has to be built by something."""
    if not cc or not sys.platform.startswith("linux"):
        return []
    base = (Path(base) / "cimport_libs").resolve()
    base.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Reply)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/body.txt"
    tc.write_text(base / "ossl.h", CRYPTO_H)
    libs = [
        ("sqlite3", find("sqlite3.h"), ["--lib", "sqlite3"], SQLITE, SQLITE_OUT, []),
        ("curl", find("curl/curl.h"),
         ["--name", "curl", "--also", "easy.h,multi.h,urlapi.h,options.h,header.h,system.h,curlver.h",
          "--lib", "curl"], CURL, CURL_OUT, [url]),
        ("crypto", base / "ossl.h" if find("openssl/evp.h") else None,
         ["--name", "crypto", "--also", "evp.h,sha.h,hmac.h,rand.h,crypto.h,opensslv.h", "--lib", "crypto"],
         CRYPTO, CRYPTO_OUT, []),
    ]
    ran = []
    try:
        for name, header, opts, program, expected, args in libs:
            if not header:
                continue
            r = subprocess.run([str(exe), "cimport", str(header), *opts, "--out", str(base)], capture_output=True,
                               text=True, errors="replace", timeout=600)
            if r.returncode:
                raise RuntimeError(f"gust cimport {header}:\n{r.stdout}{r.stderr}")
            prog = base / f"{name}_prog.goose"
            tc.write_text(prog, program)

            def run(argv):
                p = subprocess.run([str(a) for a in argv], cwd=base, capture_output=True, text=True,
                                   errors="replace", timeout=600)
                if tc.sanitizer_failure(p.stderr):
                    raise RuntimeError(p.stdout + p.stderr)
                return p
            p = run([exe, "run", "-q", prog, "--", *args])
            if p.returncode or p.stdout.replace("\r\n", "\n") != expected:
                raise RuntimeError(f"gust run {prog.name}, exit {p.returncode}:\n{p.stdout}{p.stderr}\n"
                                   f"wanted:\n{expected}")
            out = base / (f"{name}_prog" + tc.EXE_SUFFIX)
            p = run([exe, "build", "-q", prog, "-o", out])
            if p.returncode:
                raise RuntimeError(f"gust build {prog.name}:\n{p.stdout}{p.stderr}")
            p = run([out, *args])
            if p.returncode or p.stdout.replace("\r\n", "\n") != expected:
                raise RuntimeError(f"{out.name}, exit {p.returncode}:\n{p.stdout}{p.stderr}\nwanted:\n{expected}")
            ran.append(name)
    finally:
        server.shutdown()
    return ran
