//go:build windows

package main

import "syscall"

// soReuseUnicastPort lets Windows hand an idle connection bound to its own
// loopback address an ephemeral port another address already uses: without
// it, every such bind draws from one pool of about 16,000 ports, which ends
// the -idle connections there whatever the server does.
const soReuseUnicastPort = 0x3007

func idleControl(network, address string, c syscall.RawConn) error {
	var serr error
	err := c.Control(func(fd uintptr) {
		serr = syscall.SetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, soReuseUnicastPort, 1)
	})
	if err != nil {
		return err
	}
	return serr
}
