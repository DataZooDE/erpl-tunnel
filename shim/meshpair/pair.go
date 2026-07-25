// Package meshpair creates a connected, bidirectional byte-stream pair where one
// end is a RAW OS handle suitable for handing across a C ABI, and the other is a
// Go net.Conn.
//
// This is the transport seam of the mesh shims: a userspace (netstack) mesh
// connection lives inside Go, but the C++ port-forward engine wants a plain OS
// socket it can select/recv/send/close. New() supplies exactly that.
//
// The package is deliberately free of cgo, of the mesh libraries, and of DuckDB,
// so it can be unit-tested with plain `go test` on every target OS — which is the
// cheapest possible signal for the trickiest platform-specific logic here.
//
// Ownership contract (identical on every platform):
//
//	raw (uintptr)   -> the CALLER's, ultimately C++. Released with close()/closesocket().
//	                   The Go runtime never registers, polls, or closes it.
//	goSide net.Conn -> Go's. The bridge goroutine owns it and closes it.
//
// On error New() has released everything it created and returns (0, nil, err).
package meshpair

import "net"

// New returns one end of a connected stream pair as a raw OS handle (a file
// descriptor on Unix, a Winsock SOCKET on Windows) plus the other end as a
// net.Conn.
//
// The raw end is blocking and has no pending buffered data, so the caller may
// hand it straight to C without draining a preamble.
func New() (raw uintptr, goSide net.Conn, err error) { return newPair() }

// CloseRaw releases a handle returned by New when the caller decides not to hand
// it to C after all — e.g. the mesh dial failed after the pair was created.
// After the handle has been handed over it belongs to C and must not be passed
// here.
func CloseRaw(raw uintptr) { closeRaw(raw) }
