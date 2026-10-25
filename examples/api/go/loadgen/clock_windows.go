//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

// On Windows, Go's monotonic clock advances only with the timer interrupt
// (half a millisecond to a millisecond), too coarse for latencies of tens of
// microseconds; QueryPerformanceCounter resolves 100 ns or better.
var (
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	qpc      = kernel32.NewProc("QueryPerformanceCounter")
	qpcFreq  int64
)

func init() {
	kernel32.NewProc("QueryPerformanceFrequency").Call(uintptr(unsafe.Pointer(&qpcFreq)))
}

// nanos is a monotonic time in nanoseconds.
func nanos() int64 {
	var c int64
	qpc.Call(uintptr(unsafe.Pointer(&c)))
	return c/qpcFreq*1e9 + c%qpcFreq*1e9/qpcFreq
}
