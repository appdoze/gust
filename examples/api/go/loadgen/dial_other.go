//go:build !windows

package main

import "syscall"

// Elsewhere each source address has ports of its own.
func idleControl(network, address string, c syscall.RawConn) error { return nil }
