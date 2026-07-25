//go:build windows

package meshpair

// Test-only I/O against the raw handle, standing in for what the C++ side does.
// Winsock recv/send — NOT read/write, which only work on CRT file descriptors.

import (
	"io"

	"golang.org/x/sys/windows"
)

func rawRead(h uintptr, p []byte) (int, error) {
	var buf windows.WSABuf
	if len(p) > 0 {
		buf.Buf = &p[0]
		buf.Len = uint32(len(p))
	}
	var received, flags uint32
	err := windows.WSARecv(windows.Handle(h), &buf, 1, &received, &flags, nil, nil)
	if err != nil {
		return 0, err
	}
	if received == 0 {
		return 0, io.EOF
	}
	return int(received), nil
}

func rawWrite(h uintptr, p []byte) (int, error) {
	var buf windows.WSABuf
	if len(p) > 0 {
		buf.Buf = &p[0]
		buf.Len = uint32(len(p))
	}
	var sent uint32
	if err := windows.WSASend(windows.Handle(h), &buf, 1, &sent, 0, nil, nil); err != nil {
		return 0, err
	}
	return int(sent), nil
}
