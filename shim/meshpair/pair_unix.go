//go:build !windows

package meshpair

// Unix: an AF_UNIX socketpair. fds[0] goes to C, fds[1] stays in Go.
//
// This is a verbatim lift of the original, proven implementation from the ts/nb
// shims — the Linux/macOS data plane must not change behaviour. Do not "improve"
// it without re-running the Headscale and NetBird data-plane tests.

import (
	"fmt"
	"net"
	"os"

	"golang.org/x/sys/unix"
)

func newPair() (uintptr, net.Conn, error) {
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM, 0)
	if err != nil {
		return 0, nil, fmt.Errorf("socketpair failed: %w", err)
	}
	goFile := os.NewFile(uintptr(fds[1]), "mesh-bridge")
	goConn, err := net.FileConn(goFile)
	goFile.Close() // net.FileConn dup'd the fd
	if err != nil {
		unix.Close(fds[0])
		return 0, nil, fmt.Errorf("FileConn failed: %w", err)
	}
	return uintptr(fds[0]), goConn, nil
}

func closeRaw(raw uintptr) { unix.Close(int(raw)) }
