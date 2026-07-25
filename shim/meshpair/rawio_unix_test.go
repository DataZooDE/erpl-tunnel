//go:build !windows

package meshpair

// Test-only I/O against the raw handle, standing in for what the C++ side does
// (POSIX read/write on the fd).

import (
	"io"

	"golang.org/x/sys/unix"
)

func rawRead(h uintptr, p []byte) (int, error) {
	n, err := unix.Read(int(h), p)
	if err != nil {
		return 0, err
	}
	if n == 0 {
		return 0, io.EOF
	}
	return n, nil
}

func rawWrite(h uintptr, p []byte) (int, error) { return unix.Write(int(h), p) }
