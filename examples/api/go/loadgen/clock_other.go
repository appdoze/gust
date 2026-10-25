//go:build !windows

package main

import "time"

var epoch = time.Now()

// nanos is a monotonic time in nanoseconds.
func nanos() int64 { return int64(time.Since(epoch)) }
