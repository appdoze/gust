// A load generator for the API example, the same for both servers: keep-alive
// connections, each sending a fixed rotation of requests and checking every
// response's status.
//
//	loadgen -addr 127.0.0.1:8080 -c 64 -d 10s [-rate 20000] [-mix mixed]
//
// Closed loop (no -rate): each connection sends its next request as soon as
// the last is answered, which measures the most a server sustains; its
// latencies understate the tail (coordinated omission: a stalled server
// delays the requests that would have seen the stall). With -rate, the
// connections send on a fixed schedule, rate per second in all, and each
// latency is also measured from the time the request was due, not when it
// went out, as wrk2 measures (due_*); that one includes the client's timer
// slack, the same for any server.
//
// With -churn, every request goes on a connection of its own, which is then
// reset (SO_LINGER 0, so that no TIME_WAIT is left to exhaust the ports):
// the cost of accepting and setting up a connection, for clients that do
// not keep theirs.
//
// Prints one JSON object: requests, rps, latency percentiles in
// microseconds, and errors.
package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

type req struct {
	raw    []byte
	status int
}

func get(path string, status int) req {
	return req{[]byte("GET " + path + " HTTP/1.1\r\nHost: api\r\n\r\n"), status}
}

func post(path, body string, status int) req {
	return req{[]byte(fmt.Sprintf("POST %s HTTP/1.1\r\nHost: api\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
		path, len(body), body)), status}
}

// The rotations, ten requests each.
func mix(name string) []req {
	switch name {
	case "health":
		return []req{get("/health", 200)}
	case "item":
		var rs []req
		for i := 1; i <= 8; i++ {
			rs = append(rs, get("/v1/items/"+strconv.Itoa(i), 200))
		}
		return rs
	case "order":
		return []req{post("/v1/orders", `{"item_id":2,"quantity":3,"note":"oat milk, extra hot"}`, 201)}
	case "mixed":
		return []req{
			get("/v1/items/1", 200),
			post("/v1/orders", `{"item_id":2,"quantity":3,"note":"oat milk, extra hot"}`, 201),
			get("/v1/items/5", 200),
			get("/v1/items", 200),
			post("/v1/orders", `{"item_id":7,"quantity":1}`, 201),
			get("/v1/items/3", 200),
			post("/v1/orders", `{"item_id":1,"quantity":0}`, 422),
			get("/health", 200),
			post("/v1/orders", `{"item_id":5,"quantity":12,"note":"for the office"}`, 201),
			get("/v1/items/99", 404),
		}
	}
	fmt.Fprintln(os.Stderr, "unknown mix", name)
	os.Exit(2)
	return nil
}

// Reads one response: its status; the body is read and dropped.
func readResponse(r *bufio.Reader) (int, error) {
	line, err := r.ReadSlice('\n')
	if err != nil {
		return 0, err
	}
	if len(line) < 12 || !bytes.HasPrefix(line, []byte("HTTP/1.")) {
		return 0, fmt.Errorf("bad status line %q", line)
	}
	status, err := strconv.Atoi(string(line[9:12]))
	if err != nil {
		return 0, err
	}
	length := -1
	for {
		h, err := r.ReadSlice('\n')
		if err != nil {
			return 0, err
		}
		if len(h) <= 2 {
			break
		}
		if len(h) > 15 && bytes.EqualFold(h[:15], []byte("content-length:")) {
			length, err = strconv.Atoi(string(bytes.TrimSpace(h[15:])))
			if err != nil {
				return 0, err
			}
		}
	}
	if length < 0 {
		return 0, fmt.Errorf("no Content-Length")
	}
	if _, err := r.Discard(length); err != nil {
		return 0, err
	}
	return status, nil
}

type result struct {
	Mix         string  `json:"mix"`
	Connections int     `json:"connections"`
	Rate        int     `json:"rate"`
	Seconds     float64 `json:"seconds"`
	Churn       bool    `json:"churn"`
	Requests    int     `json:"requests"`
	Total       int64   `json:"total_with_warmup"`
	RPS         float64 `json:"rps"`
	P50         float64 `json:"p50_us"`
	P90         float64 `json:"p90_us"`
	P99         float64 `json:"p99_us"`
	P999        float64 `json:"p999_us"`
	Max         float64 `json:"max_us"`
	// With -rate: from when each request was due, not when it went out.
	DueP50     float64 `json:"due_p50_us,omitempty"`
	DueP99     float64 `json:"due_p99_us,omitempty"`
	DueP999    float64 `json:"due_p999_us,omitempty"`
	DueMax     float64 `json:"due_max_us,omitempty"`
	Errors     int64   `json:"errors"`
	Mismatches int64   `json:"status_mismatches"`
	FirstError string  `json:"first_error,omitempty"`
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "server address")
	conns := flag.Int("c", 64, "connections")
	dur := flag.Duration("d", 10*time.Second, "measured duration")
	warm := flag.Duration("warmup", 2*time.Second, "unmeasured warm-up before it")
	rate := flag.Int("rate", 0, "requests per second in all (0: closed loop)")
	mixName := flag.String("mix", "mixed", "health, item, order or mixed")
	churn := flag.Bool("churn", false, "a new connection for every request")
	flag.Parse()
	rot := mix(*mixName)

	var recording, stopping atomic.Bool
	var errs, mismatches, total atomic.Int64
	var firstErr atomic.Value
	lats := make([][]int64, *conns)
	dueLats := make([][]int64, *conns)
	var wg sync.WaitGroup
	start := nanos()
	for c := 0; c < *conns; c++ {
		wg.Add(1)
		go func(c int) {
			defer wg.Done()
			fail := func(err error) {
				errs.Add(1)
				firstErr.CompareAndSwap(nil, err.Error())
			}
			var conn net.Conn
			var br *bufio.Reader
			dial := func() bool {
				var err error
				conn, err = net.Dial("tcp", *addr)
				if err != nil {
					fail(err)
					return false
				}
				conn.(*net.TCPConn).SetNoDelay(true)
				if *churn {
					conn.(*net.TCPConn).SetLinger(0)
				}
				br = bufio.NewReaderSize(conn, 16384)
				return true
			}
			if !*churn {
				if !dial() {
					return
				}
				defer conn.Close()
			}
			var interval int64
			if *rate > 0 {
				interval = int64(float64(time.Second) * float64(*conns) / float64(*rate))
			}
			// Connections start spread over one interval, not all at once.
			due := start + interval*int64(c)/int64(*conns)
			local := make([]int64, 0, 1<<16)
			var localDue []int64
			for i := c; !stopping.Load(); i++ {
				r := rot[i%len(rot)]
				if interval > 0 {
					if d := due - nanos(); d > 0 {
						time.Sleep(time.Duration(d))
					}
				}
				if *churn && !dial() {
					return
				}
				sent := nanos()
				if _, err := conn.Write(r.raw); err != nil {
					fail(err)
					return
				}
				status, err := readResponse(br)
				doneAt := nanos()
				if *churn {
					conn.Close()
				}
				if err != nil {
					fail(err)
					return
				}
				total.Add(1)
				if recording.Load() {
					local = append(local, doneAt-sent)
					if interval > 0 {
						localDue = append(localDue, doneAt-due)
					}
					if status != r.status {
						mismatches.Add(1)
					}
				}
				due += interval
			}
			lats[c] = local
			dueLats[c] = localDue
		}(c)
	}
	time.Sleep(*warm)
	recording.Store(true)
	t0 := nanos()
	time.Sleep(*dur)
	recording.Store(false)
	elapsed := float64(nanos()-t0) / 1e9
	stopping.Store(true)
	wg.Wait()

	sorted := func(ls [][]int64) []int64 {
		var all []int64
		for _, l := range ls {
			all = append(all, l...)
		}
		sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
		return all
	}
	pct := func(all []int64, p float64) float64 {
		if len(all) == 0 {
			return 0
		}
		i := int(p * float64(len(all)))
		if i >= len(all) {
			i = len(all) - 1
		}
		return float64(all[i]) / 1e3
	}
	all := sorted(lats)
	res := result{
		Mix: *mixName, Connections: *conns, Rate: *rate, Seconds: elapsed, Churn: *churn,
		Requests: len(all), Total: total.Load(), RPS: float64(len(all)) / elapsed,
		P50: pct(all, 0.50), P90: pct(all, 0.90), P99: pct(all, 0.99), P999: pct(all, 0.999), Max: pct(all, 1),
		Errors: errs.Load(), Mismatches: mismatches.Load(),
	}
	if *rate > 0 {
		d := sorted(dueLats)
		res.DueP50, res.DueP99, res.DueP999, res.DueMax = pct(d, 0.50), pct(d, 0.99), pct(d, 0.999), pct(d, 1)
	}
	if e, ok := firstErr.Load().(string); ok {
		res.FirstError = e
	}
	out, _ := json.Marshal(res)
	fmt.Println(string(out))
}
