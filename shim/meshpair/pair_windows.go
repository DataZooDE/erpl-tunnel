//go:build windows

package meshpair

// Windows has no AF_UNIX socketpair(), so the pair is built from a short-lived
// loopback TCP listener:
//
//	127.0.0.1:0 listener   <- we accept our own connection on it (Go's end)
//	raw SOCKET             -> handed to C
//
// Four details carry the design:
//
//  1. C's end is created with windows.Socket, NOT net.Dial. net.FileConn does not
//     accept a Windows SOCKET at all, and any socket the net package owns is
//     registered with the IOCP poller and closed when its net.Conn is closed or
//     collected — which would yank the handle out from under C. A raw SOCKET is
//     untouched by the runtime, so its lifetime belongs entirely to C.
//
//  2. SO_EXCLUSIVEADDRUSE on the listener. On Windows another local process can
//     otherwise bind the *same* 127.0.0.1:port with SO_REUSEADDR and steal inbound
//     connections — a hijack with no Unix analogue.
//
//  3. The accept is gated on the full TCP 4-tuple. Our raw socket already owns
//     (127.0.0.1:P -> 127.0.0.1:L), and no second socket in the system can form an
//     identical tuple, so requiring the accepted peer to be exactly 127.0.0.1:P is
//     airtight: an impostor necessarily arrives from a different source port and is
//     dropped. This needs no handshake bytes, which keeps the handle we give C free
//     of any preamble for C++ to drain.
//
//  4. TCP_NODELAY on both ends. Loopback TCP applies Nagle plus delayed ACK, which
//     adds ~40 ms stalls to request/response traffic — exactly the SAP/HTTP shape
//     this extension forwards. AF_UNIX has no such behaviour, so without this the
//     Windows path would be mysteriously slower than Unix.
//
// Winsock must be initialised before windows.Socket; net.Listen below does that
// (the net package calls WSAStartup lazily), which is why the listener comes first.

import (
	"context"
	"fmt"
	"net"
	"syscall"
	"time"

	"golang.org/x/sys/windows"
)

// SO_EXCLUSIVEADDRUSE is absent from x/sys/windows.
const soExclusiveAddrUse = ^int(0x0004) // -5

// acceptTimeout bounds the whole connect+accept dance. Loopback, so this is a
// generous ceiling rather than an expected wait.
const acceptTimeout = 10 * time.Second

func newPair() (uintptr, net.Conn, error) {
	lc := net.ListenConfig{Control: func(_, _ string, c syscall.RawConn) error {
		var soErr error
		if err := c.Control(func(fd uintptr) {
			soErr = windows.SetsockoptInt(windows.Handle(fd), windows.SOL_SOCKET, soExclusiveAddrUse, 1)
		}); err != nil {
			return err
		}
		return soErr
	}}

	ln, err := lc.Listen(context.Background(), "tcp4", "127.0.0.1:0")
	if err != nil {
		return 0, nil, fmt.Errorf("loopback listen failed: %w", err)
	}
	defer ln.Close() // the listener lives for microseconds

	tcpLn, ok := ln.(*net.TCPListener)
	if !ok {
		return 0, nil, fmt.Errorf("unexpected listener type %T", ln)
	}
	port := tcpLn.Addr().(*net.TCPAddr).Port

	s, err := windows.Socket(windows.AF_INET, windows.SOCK_STREAM, windows.IPPROTO_TCP)
	if err != nil {
		return 0, nil, fmt.Errorf("socket failed: %w", err)
	}
	fail := func(format string, args ...any) (uintptr, net.Conn, error) {
		windows.Closesocket(s)
		return 0, nil, fmt.Errorf(format, args...)
	}

	sa := &windows.SockaddrInet4{Port: port, Addr: [4]byte{127, 0, 0, 1}}
	if err := windows.Connect(s, sa); err != nil {
		return fail("loopback connect failed: %w", err)
	}
	if err := windows.SetsockoptInt(s, windows.IPPROTO_TCP, windows.TCP_NODELAY, 1); err != nil {
		return fail("set TCP_NODELAY on raw end failed: %w", err)
	}

	// Our own source port identifies the connection we just initiated.
	lsa, err := windows.Getsockname(s)
	if err != nil {
		return fail("getsockname failed: %w", err)
	}
	mine, ok := lsa.(*windows.SockaddrInet4)
	if !ok {
		return fail("unexpected local address type %T", lsa)
	}

	if err := tcpLn.SetDeadline(time.Now().Add(acceptTimeout)); err != nil {
		return fail("set accept deadline failed: %w", err)
	}
	for {
		c, err := tcpLn.Accept()
		if err != nil {
			return fail("loopback accept failed: %w", err)
		}
		ra, ok := c.RemoteAddr().(*net.TCPAddr)
		if ok && ra.IP.IsLoopback() && ra.Port == mine.Port {
			if tc, ok := c.(*net.TCPConn); ok {
				tc.SetNoDelay(true)
			}
			return uintptr(s), c, nil
		}
		// Someone else reached the port first — drop them, keep waiting for ours.
		c.Close()
	}
}

func closeRaw(raw uintptr) { windows.Closesocket(windows.Handle(raw)) }
