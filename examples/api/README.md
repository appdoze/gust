# A JSON API in Gust, and its Go twins

A small HTTP/1.1 service, a product catalog and orders, written the way a
backend in Gust is meant to look, beside the same service in Go twice: on
idiomatic `net/http`, and with the same minimal HTTP as the Gust server, so
the language and the library can be compared apart.

| file | what |
|---|---|
| `api.gust` | the service: routes, models, validation, the worker pool or a thread per connection, containment, graceful shutdown |
| `http.gust` | HTTP/1.1 server side as a protocol over the standard library's event loop (`net::serve`, `stdlib/net.goose`): in-place request parsing, keep-alive, pipelining, limits, timeouts, error responses, responses built whole up to 1 MB and streamed (chunked) past that |
| `jsonx.gust` | JSON for a struct through `const for` (compile-time field iteration), and FastAPI-shaped `422` errors |
| `go/server` | the Go twin on `net/http` |
| `go/rawserver` | the Go twin with the same minimal HTTP as `http.gust` |
| `go/loadgen` | the load generator all of them are measured with |
| `test_api.py` | the behaviour tests, run against any of the servers |
| `fuzz_http.py` | mutated requests against a server: every answer well formed, no worker aborted, still serving |
| `bench.py`, `tables.py` | the benchmark and its tables |
| `memory_profile.py` | reserved, committed and resident memory per idle connection, at several counts |
| `scale.py` | the pool beside thousands of idle connections, with the platform's readiness backend and with `poll`/`WSAPoll` |
| `switches.py`, `loadgen_check.py`, `contention.py` | context switches per request; whether the load generator is the bottleneck; tails under CPU contention from another process |
| `results/` | raw results of the runs `docs/gust/backend_api.md` reports |

```bash
gust build examples/api/api.gust -o api.exe
```

```bash
api.exe 8080 --workers 8
```

```bash
python examples/api/test_api.py --exe api.exe --crash-route
```

```bash
python examples/api/bench.py
```

Two ways to serve, the same HTTP:

- **A worker pool** (the default; `--workers N`, by default one per
  hardware thread): each worker an event loop over the connections it
  accepts. An idle connection costs about 2 KB. Handlers must not block.
- **A thread per connection** (`--thread-per-connection`): the acceptor
  detaches each connection and a thread of its own adopts it and runs the
  same loop over it alone; handlers may block; each connection costs two
  threads and about 130 KB resident.

The sockets, the poller and the loop are the standard library's `net`
(`docs/stdlib.md`); `examples/echo/` is the same loop with no HTTP.

The routes:

```
GET  /health                     {"status":"ok"}
GET  /v1/items[?repeat=N]        {"items":[...]}: the catalog N times (1-1000), built whole
GET  /v1/items/export?repeat=N   the same, N up to 1,000,000, streamed in chunks
GET  /v1/items/{id}              an item, or 404
POST /v1/orders                  {"item_id":1,"quantity":2,"note":"..."} -> 201, or 422 / 409
```

A validation error reads as FastAPI's do:

```json
{"detail":[{"loc":["body","quantity"],"msg":"Input should be less than or equal to 100","type":"less_than_equal"}]}
```

Design, measurements against Go, the gaps found and the roadmap are in
[`docs/gust/backend_api.md`](../../docs/gust/backend_api.md). The test
suite builds the example and runs `test_api.py` against both ways to
serve, normally and with `--checked`, and in its sanitizer profile under
ASan and UBSan (`test/api_example.py`).
