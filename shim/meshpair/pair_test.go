package meshpair

import (
	"bytes"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

// closeWriter is satisfied by *net.UnixConn (Unix) and *net.TCPConn (Windows).
type closeWriter interface{ CloseWrite() error }

func newPairT(t *testing.T) (uintptr, net.Conn) {
	t.Helper()
	raw, goSide, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	if raw == 0 {
		t.Fatal("New() returned a zero raw handle")
	}
	if goSide == nil {
		t.Fatal("New() returned a nil net.Conn")
	}
	return raw, goSide
}

// Go -> raw and raw -> Go both carry bytes intact.
func TestPairIsBidirectional(t *testing.T) {
	raw, goSide := newPairT(t)
	defer CloseRaw(raw)
	defer goSide.Close()

	want := []byte("hello from go")
	if _, err := goSide.Write(want); err != nil {
		t.Fatalf("write on go side: %v", err)
	}
	got := make([]byte, len(want))
	if _, err := io.ReadFull(rawReader{raw}, got); err != nil {
		t.Fatalf("read on raw side: %v", err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("raw side got %q, want %q", got, want)
	}

	want2 := []byte("hello from c")
	if _, err := rawWrite(raw, want2); err != nil {
		t.Fatalf("write on raw side: %v", err)
	}
	got2 := make([]byte, len(want2))
	if err := goSide.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("set read deadline: %v", err)
	}
	if _, err := io.ReadFull(goSide, got2); err != nil {
		t.Fatalf("read on go side: %v", err)
	}
	if !bytes.Equal(got2, want2) {
		t.Fatalf("go side got %q, want %q", got2, want2)
	}
}

// The half-close the shim's bridge() performs must be observable by C as recv()==0.
// This is what propagates mesh EOF to the local client.
func TestGoCloseWriteIsEOFOnRawSide(t *testing.T) {
	raw, goSide := newPairT(t)
	defer CloseRaw(raw)
	defer goSide.Close()

	cw, ok := goSide.(closeWriter)
	if !ok {
		t.Fatalf("go side %T does not support CloseWrite; bridge()'s half-close would silently no-op", goSide)
	}
	if _, err := goSide.Write([]byte("last")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := cw.CloseWrite(); err != nil {
		t.Fatalf("CloseWrite: %v", err)
	}

	buf := make([]byte, 4)
	if _, err := io.ReadFull(rawReader{raw}, buf); err != nil {
		t.Fatalf("read payload: %v", err)
	}
	n, err := rawRead(raw, buf)
	if n != 0 || (err != nil && err != io.EOF) {
		t.Fatalf("after CloseWrite, raw read = (%d, %v); want (0, nil/EOF)", n, err)
	}
}

// When C closes its handle, the Go bridge must observe the stream ending so it can
// tear the mesh conn down rather than leaking a goroutine.
func TestRawCloseIsEOFOnGoSide(t *testing.T) {
	raw, goSide := newPairT(t)
	defer goSide.Close()

	CloseRaw(raw)

	if err := goSide.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("set read deadline: %v", err)
	}
	buf := make([]byte, 16)
	if _, err := goSide.Read(buf); err == nil {
		t.Fatal("go side read succeeded after the raw end was closed; want an error/EOF")
	}
}

// Many pairs at once: on Windows this exercises the accept-matching loop under
// contention, where a wrong 4-tuple check would cross-wire two pairs.
func TestConcurrentPairsAreNotCrossWired(t *testing.T) {
	const n = 24
	var wg sync.WaitGroup
	errs := make(chan error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			raw, goSide, err := New()
			if err != nil {
				errs <- err
				return
			}
			defer CloseRaw(raw)
			defer goSide.Close()

			// A per-pair marker: if two pairs were cross-wired this comes back wrong.
			msg := []byte{byte(i), byte(i >> 8), 'x', 'y'}
			if _, err := goSide.Write(msg); err != nil {
				errs <- err
				return
			}
			got := make([]byte, len(msg))
			if _, err := io.ReadFull(rawReader{raw}, got); err != nil {
				errs <- err
				return
			}
			if !bytes.Equal(got, msg) {
				errs <- errCrossWired{want: msg, got: got}
			}
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Errorf("concurrent pair: %v", err)
	}
}

type errCrossWired struct{ want, got []byte }

func (e errCrossWired) Error() string {
	return "pair cross-wired: got " + string(e.got) + ", want " + string(e.want)
}

// Repeated create/destroy must not leak handles or wedge (the listener is closed
// each time, so a leak here shows up fast as EMFILE / WSAEMFILE).
func TestRepeatedPairsDoNotLeak(t *testing.T) {
	for i := 0; i < 200; i++ {
		raw, goSide, err := New()
		if err != nil {
			t.Fatalf("iteration %d: %v", i, err)
		}
		CloseRaw(raw)
		goSide.Close()
	}
}

// rawReader adapts the raw handle to io.Reader so io.ReadFull can be used.
type rawReader struct{ h uintptr }

func (r rawReader) Read(p []byte) (int, error) { return rawRead(r.h, p) }
