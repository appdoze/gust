// The Go twin without net/http: the same minimal HTTP/1.1 as
// examples/api/http.gust (a goroutine per connection, the request parsed in
// place in one buffer, keep-alive and pipelining, the same limits and
// refusals, one write per response), with the same encoding/json handlers
// as ../server. It separates what the language and its runtime cost from
// what net/http does beyond what the Gust server does.
//
//	go build -o rawserver.exe ./rawserver && rawserver.exe [port]
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	maxHead = 8192
	maxBody = 8192
	idle    = 5 * time.Second
	poll    = 100 * time.Millisecond
)

type Item struct {
	ID         int64   `json:"id"`
	Name       string  `json:"name"`
	PriceCents int64   `json:"price_cents"`
	Rating     float64 `json:"rating"`
	InStock    bool    `json:"in_stock"`
}

type OrderIn struct {
	ItemID   *int64  `json:"item_id"`
	Quantity *int64  `json:"quantity"`
	Note     *string `json:"note"`
}

type Order struct {
	ID         int64  `json:"id"`
	ItemID     int64  `json:"item_id"`
	Quantity   int64  `json:"quantity"`
	TotalCents int64  `json:"total_cents"`
	Note       string `json:"note"`
}

var catalog = []Item{
	{1, "Espresso", 250, 4.6, true},
	{2, "Cappuccino", 380, 4.4, true},
	{3, "Flat white", 390, 4.7, true},
	{4, "Cold brew", 420, 4.1, false},
	{5, "Croissant", 310, 4.5, true},
	{6, "Banana bread", 295, 3.9, true},
	{7, "Oat milk latte", 450, 4.3, true},
	{8, "Matcha", 470, 4.0, false},
}

var (
	nextOrder atomic.Int64
	stopping  atomic.Bool
)

type problem struct {
	Loc  []any  `json:"loc"`
	Msg  string `json:"msg"`
	Type string `json:"type"`
}

type request struct {
	method, path []byte
	body         []byte
	keepAlive    bool
}

type response struct {
	status int
	allow  string
	body   []byte
}

var reasons = map[int]string{
	200: "OK", 201: "Created", 400: "Bad Request", 404: "Not Found", 405: "Method Not Allowed",
	409: "Conflict", 413: "Content Too Large", 422: "Unprocessable Content",
	431: "Request Header Fields Too Large", 500: "Internal Server Error", 501: "Not Implemented",
}

func jsonBody(r *response, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		status, b = 500, []byte(`{"detail":"Internal Server Error"}`)
	}
	r.status, r.body = status, b
}

func detail(r *response, status int, msg string) {
	jsonBody(r, status, map[string]string{"detail": msg})
}

func notAllowed(r *response, allow string) {
	r.allow = allow
	detail(r, 405, "Method Not Allowed")
}

func handle(req *request, res *response) {
	path := string(req.path)
	method := string(req.method)
	switch {
	case path == "/health":
		if method != "GET" {
			notAllowed(res, "GET")
			return
		}
		jsonBody(res, 200, map[string]string{"status": "ok"})
	case path == "/v1/items":
		if method != "GET" {
			notAllowed(res, "GET")
			return
		}
		jsonBody(res, 200, map[string][]Item{"items": catalog})
	case len(path) > 10 && path[:10] == "/v1/items/":
		if method != "GET" {
			notAllowed(res, "GET")
			return
		}
		id, err := strconv.ParseInt(path[10:], 10, 64)
		if err != nil || id < 1 || id > int64(len(catalog)) {
			detail(res, 404, "Not Found")
			return
		}
		jsonBody(res, 200, catalog[id-1])
	case path == "/v1/orders":
		if method != "POST" {
			notAllowed(res, "POST")
			return
		}
		order(req, res)
	case path == "/debug/vars":
		var ms runtime.MemStats
		runtime.ReadMemStats(&ms)
		jsonBody(res, 200, map[string]any{"memstats": map[string]uint64{
			"Mallocs": ms.Mallocs, "Frees": ms.Frees, "TotalAlloc": ms.TotalAlloc, "HeapInuse": ms.HeapInuse,
			"Sys": ms.Sys, "NumGC": uint64(ms.NumGC), "PauseTotalNs": ms.PauseTotalNs}})
	default:
		detail(res, 404, "Not Found")
	}
}

func order(req *request, res *response) {
	var in OrderIn
	var ps []problem
	if err := json.Unmarshal(req.body, &in); err != nil {
		var te *json.UnmarshalTypeError
		if errors.As(err, &te) {
			ps = append(ps, problem{[]any{"body", te.Field}, "expected " + te.Type.String(), "value_error"})
		} else {
			ps = append(ps, problem{[]any{"body"}, "JSON decode error: " + err.Error(), "json_invalid"})
		}
	} else {
		if in.ItemID == nil {
			ps = append(ps, problem{[]any{"body", "item_id"}, "Field required", "missing"})
		} else if *in.ItemID < 1 || *in.ItemID > int64(len(catalog)) {
			ps = append(ps, problem{[]any{"body", "item_id"}, "No item has this id", "value_error"})
		}
		if in.Quantity == nil {
			ps = append(ps, problem{[]any{"body", "quantity"}, "Field required", "missing"})
		} else if *in.Quantity < 1 {
			ps = append(ps, problem{[]any{"body", "quantity"}, "Input should be greater than or equal to 1", "greater_than_equal"})
		} else if *in.Quantity > 100 {
			ps = append(ps, problem{[]any{"body", "quantity"}, "Input should be less than or equal to 100", "less_than_equal"})
		}
		if in.Note != nil && len(*in.Note) > 64 {
			ps = append(ps, problem{[]any{"body", "note"}, "a string longer than its field", "value_error"})
		}
	}
	if len(ps) > 0 {
		jsonBody(res, 422, map[string][]problem{"detail": ps})
		return
	}
	it := catalog[*in.ItemID-1]
	if !it.InStock {
		detail(res, 409, "Out of stock")
		return
	}
	note := ""
	if in.Note != nil {
		note = *in.Note
	}
	jsonBody(res, 201, Order{nextOrder.Add(1), it.ID, *in.Quantity, it.PriceCents * *in.Quantity, note})
}

func equalFold(s []byte, word string) bool {
	return len(s) == len(word) && bytes.EqualFold(s, []byte(word))
}

func hasToken(v []byte, tok string) bool {
	for _, part := range bytes.Split(v, []byte(",")) {
		if equalFold(bytes.TrimSpace(part), tok) {
			return true
		}
	}
	return false
}

// parseHead: the request (body still empty), the body's length, and a
// status to refuse it with (0 when fine), as http.gust's parse_head.
func parseHead(head []byte) (request, int, int) {
	var req request
	eol := bytes.IndexByte(head, '\r')
	line := head[:eol]
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return req, 0, 400
	}
	sp2 := bytes.IndexByte(line[sp1+1:], ' ')
	if sp2 <= 0 {
		return req, 0, 400
	}
	sp2 += sp1 + 1
	req.method = line[:sp1]
	target := line[sp1+1 : sp2]
	version := string(line[sp2+1:])
	if target[0] != '/' {
		return req, 0, 400
	}
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	req.path = target
	switch version {
	case "HTTP/1.1":
		req.keepAlive = true
	case "HTTP/1.0":
	default:
		return req, 0, 400
	}
	length, seenLength, hosts := 0, false, 0
	for at := eol + 2; at < len(head)-2; {
		end := at + bytes.IndexByte(head[at:], '\r')
		h := head[at:end]
		at = end + 2
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return req, 0, 400
		}
		name, value := h[:colon], bytes.TrimSpace(h[colon+1:])
		switch {
		case equalFold(name, "content-length"):
			n := 0
			if len(value) == 0 {
				return req, 0, 400
			}
			for _, c := range value {
				if c < '0' || c > '9' {
					return req, 0, 400
				}
				n = min(n*10+int(c-'0'), maxBody+1)
			}
			if seenLength && n != length {
				return req, 0, 400
			}
			length, seenLength = n, true
		case equalFold(name, "transfer-encoding"):
			return req, 0, 501
		case equalFold(name, "connection"):
			if hasToken(value, "close") {
				req.keepAlive = false
			} else if hasToken(value, "keep-alive") {
				req.keepAlive = true
			}
		case equalFold(name, "host"):
			hosts++
		}
	}
	if hosts > 1 || (hosts == 0 && version == "HTTP/1.1") {
		return req, 0, 400
	}
	if length > maxBody {
		return req, length, 413
	}
	return req, length, 0
}

func send(c net.Conn, out []byte, res *response, keep bool) ([]byte, bool) {
	out = append(out[:0], "HTTP/1.1 "...)
	out = strconv.AppendInt(out, int64(res.status), 10)
	out = append(out, ' ')
	out = append(out, reasons[res.status]...)
	out = append(out, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	out = strconv.AppendInt(out, int64(len(res.body)), 10)
	out = append(out, "\r\n"...)
	if res.allow != "" {
		out = append(out, "Allow: "...)
		out = append(out, res.allow...)
		out = append(out, "\r\n"...)
	}
	if !keep {
		out = append(out, "Connection: close\r\n"...)
	}
	out = append(out, "\r\n"...)
	out = append(out, res.body...)
	_, err := c.Write(out)
	return out, err == nil
}

// fill: more bytes into buf[have:limit], or 0 when the connection should
// end, as http.gust's fill (closed, failed, idle, or stopping with nothing read).
func fill(c net.Conn, buf []byte, have, limit int) int {
	deadline := time.Now().Add(idle)
	for {
		if have == 0 && stopping.Load() {
			return 0
		}
		c.SetReadDeadline(time.Now().Add(poll))
		n, err := c.Read(buf[have:limit])
		if n > 0 {
			return n
		}
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() && time.Now().Before(deadline) {
			continue
		}
		return 0
	}
}

func serve(c net.Conn) {
	defer c.Close()
	c.(*net.TCPConn).SetNoDelay(true)
	buf := make([]byte, maxHead+maxBody)
	out := make([]byte, 0, 1024)
	have := 0
	for {
		end := -1
		for {
			if i := bytes.Index(buf[:have], []byte("\r\n\r\n")); i >= 0 {
				end = i + 4
				break
			}
			if have >= maxHead {
				refuse(c, out, 431)
				return
			}
			n := fill(c, buf, have, len(buf))
			if n == 0 {
				return
			}
			have += n
		}
		if end > maxHead {
			refuse(c, out, 431)
			return
		}
		req, length, status := parseHead(buf[:end])
		if status != 0 {
			refuse(c, out, status)
			return
		}
		for have < end+length {
			n := fill(c, buf, have, len(buf))
			if n == 0 {
				return
			}
			have += n
		}
		done := end + length
		req.body = buf[end:done]
		var res response
		handle(&req, &res)
		keep := req.keepAlive && !stopping.Load()
		var ok bool
		if out, ok = send(c, out, &res, keep); !ok || !keep {
			return
		}
		copy(buf, buf[done:have])
		have -= done
	}
}

func refuse(c net.Conn, out []byte, status int) {
	res := response{status: status, body: []byte(fmt.Sprintf(`{"detail":"%s"}`, reasons[status]))}
	send(c, out, &res, false)
}

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	ln, err := net.Listen("tcp", ":"+port)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("listening on :" + port)
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		stopping.Store(true)
		ln.Close()
	}()
	var conns sync.WaitGroup
	served := 0
	for {
		c, err := ln.Accept()
		if err != nil {
			if stopping.Load() {
				break
			}
			continue
		}
		served++
		conns.Add(1)
		go func() {
			defer conns.Done()
			serve(c)
		}()
	}
	fmt.Println("stopping")
	waited := make(chan struct{})
	go func() { conns.Wait(); close(waited) }()
	select {
	case <-waited:
	case <-time.After(10 * time.Second):
	}
	fmt.Printf("served %d connection(s)\n", served)
}
