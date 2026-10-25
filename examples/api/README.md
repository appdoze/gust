# A JSON API in Gust, and its Go twins

A small HTTP/1.1 service, a product catalog and orders, written the way a
backend in Gust is meant to look, beside the same service in Go twice: on
idiomatic `net/http`, and with the same minimal HTTP as the Gust server, so
the language and the library can be compared apart.

| file | what |
|---|---|
| `api.gust` | the service: routes, models, validation, the worker pool or a thread per connection, containment, graceful shutdown |
| `http.gust` | HTTP/1.1 server side: in-place request parsing, keep-alive, pipelining, limits, timeouts, error responses; the event loop (`serve_loop`) and the blocking per-connection server (`serve`) |
| `jsonx.gust` | JSON for a struct through `const for` (compile-time field iteration), and FastAPI-shaped `422` errors |
| `net.gust`, `net.h`, `net.c` | sockets (Winsock and POSIX), blocking and non-blocking, and a poller that owns its sockets, behind `extern fn`s checked against `net.h` (`c_headers`) |
| `go/server` | the Go twin on `net/http` |
| `go/rawserver` | the Go twin with the same minimal HTTP as `http.gust` |
| `go/loadgen` | the load generator all of them are measured with |
| `test_api.py` | the behaviour tests, run against any of the servers |
| `bench.py`, `tables.py` | the benchmark and its tables |
| `memory_profile.py` | reserved, committed and resident memory per idle connection, at several counts |
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
- **A thread per connection** (`--thread-per-connection`): handlers may
  block; each connection costs two threads and about 130 KB resident.

The routes:

```
GET  /health              {"status":"ok"}
GET  /v1/items            {"items":[...]}
GET  /v1/items/{id}       an item, or 404
POST /v1/orders           {"item_id":1,"quantity":2,"note":"..."} -> 201, or 422 / 409
```

A validation error reads as FastAPI's do:

```json
{"detail":[{"loc":["body","quantity"],"msg":"Input should be less than or equal to 100","type":"less_than_equal"}]}
```

Design, measurements against Go, the gaps found and the roadmap are in
[`docs/gust/backend_api.md`](../../docs/gust/backend_api.md). The test
suite builds the example and runs `test_api.py` against both ways to
serve, normally and with `--checked` (`test/api_example.py`).
