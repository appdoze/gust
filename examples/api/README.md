# A JSON API in Gust, and its Go twin

A small HTTP/1.1 service, a product catalog and orders, written the way a
backend in Gust is meant to look, beside the same service in idiomatic Go
(`net/http`, `encoding/json`), so the two can be compared on equal terms.

| file | what |
|---|---|
| `api.gust` | the service: routes, models, validation, a thread per connection, containment, graceful shutdown |
| `http.gust` | HTTP/1.1 server side: in-place request parsing, keep-alive, pipelining, limits, error responses |
| `jsonx.gust` | JSON for any struct through `const for` (compile-time field iteration), and FastAPI-shaped `422` errors |
| `net.gust`, `net.h`, `net.c` | sockets (Winsock and POSIX) behind a few `extern fn`s, checked against `net.h` (`c_headers`) |
| `go/server` | the Go twin |
| `go/loadgen` | the load generator both are measured with |
| `test_api.py` | the behaviour tests, run against either server |
| `bench.py` | the benchmark |

```bash
gust build examples/api/api.gust -o api.exe
```

```bash
python examples/api/test_api.py --exe api.exe --crash-route
```

```bash
python examples/api/bench.py
```

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
suite builds the example and runs `test_api.py` on every platform CI covers
(`test/api_example.py`).
