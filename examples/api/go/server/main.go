// The Go twin of examples/api/api.gust: the same routes, JSON, validation
// errors and limits, written as idiomatic net/http and encoding/json, so
// that bench.py compares the languages and not two different servers.
//
//	go build -o server.exe ./server && server.exe [port]
//
// Differences that are Go's own and kept: net/http adds a Date header, and
// supports chunked request bodies (Gust's server refuses them with 501).
// /debug/vars (expvar) reports the runtime's memory statistics, which the
// benchmark reads before and after a run for allocations and collections.
package main

import (
	"context"
	"encoding/json"
	"errors"
	_ "expvar"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	maxHead = 8192
	maxBody = 8192
	idle    = 5 * time.Second
)

type Item struct {
	ID         int64   `json:"id"`
	Name       string  `json:"name"`
	PriceCents int64   `json:"price_cents"`
	Rating     float64 `json:"rating"`
	InStock    bool    `json:"in_stock"`
}

// Pointers tell a field the body left out from one it gave as zero.
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

var nextOrder atomic.Int64

type problem struct {
	Loc  []any  `json:"loc"`
	Msg  string `json:"msg"`
	Type string `json:"type"`
}

func write(w http.ResponseWriter, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		status, b = 500, []byte(`{"detail":"Internal Server Error"}`)
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	w.WriteHeader(status)
	w.Write(b)
}

func detail(w http.ResponseWriter, status int, msg string) {
	write(w, status, map[string]string{"detail": msg})
}

func method(w http.ResponseWriter, r *http.Request, m string) bool {
	if r.Method == m {
		return true
	}
	w.Header().Set("Allow", m)
	detail(w, 405, "Method Not Allowed")
	return false
}

func health(w http.ResponseWriter, r *http.Request) {
	if method(w, r, "GET") {
		write(w, 200, map[string]string{"status": "ok"})
	}
}

func items(w http.ResponseWriter, r *http.Request) {
	if method(w, r, "GET") {
		write(w, 200, map[string][]Item{"items": catalog})
	}
}

func item(w http.ResponseWriter, r *http.Request) {
	if !method(w, r, "GET") {
		return
	}
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil || id < 1 || id > int64(len(catalog)) {
		detail(w, 404, "Not Found")
		return
	}
	write(w, 200, catalog[id-1])
}

func orders(w http.ResponseWriter, r *http.Request) {
	if !method(w, r, "POST") {
		return
	}
	body, err := readBody(w, r)
	if err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			detail(w, 413, "Content Too Large")
		} else {
			detail(w, 400, "Bad Request")
		}
		return
	}
	var in OrderIn
	var ps []problem
	if err := json.Unmarshal(body, &in); err != nil {
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
		write(w, 422, map[string][]problem{"detail": ps})
		return
	}
	it := catalog[*in.ItemID-1]
	if !it.InStock {
		detail(w, 409, "Out of stock")
		return
	}
	note := ""
	if in.Note != nil {
		note = *in.Note
	}
	write(w, 201, Order{nextOrder.Add(1), it.ID, *in.Quantity, it.PriceCents * *in.Quantity, note})
}

func readBody(w http.ResponseWriter, r *http.Request) ([]byte, error) {
	return io.ReadAll(http.MaxBytesReader(w, r.Body, maxBody))
}

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/health", health)
	mux.HandleFunc("/v1/items", items)
	mux.HandleFunc("/v1/items/{id}", item)
	mux.HandleFunc("/v1/orders", orders)
	mux.Handle("/debug/vars", http.DefaultServeMux)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { detail(w, 404, "Not Found") })

	srv := &http.Server{
		Handler:           mux,
		MaxHeaderBytes:    maxHead,
		ReadHeaderTimeout: idle,
		IdleTimeout:       idle,
	}
	ln, err := net.Listen("tcp", ":"+port)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println("listening on :" + port)

	var served atomic.Int64
	srv.ConnState = func(c net.Conn, s http.ConnState) {
		if s == http.StateNew {
			served.Add(1)
		}
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	done := make(chan struct{})
	go func() {
		<-stop
		fmt.Println("stopping")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
		close(done)
	}()
	if err := srv.Serve(ln); err != http.ErrServerClosed {
		log.Fatal(err)
	}
	<-done
	fmt.Printf("served %d connection(s)\n", served.Load())
}
