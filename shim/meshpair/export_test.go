package meshpair

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

// A stand-in for the local service being exported: echoes a fixed line, then a
// copy of whatever it was sent.
func startLocalService(t *testing.T, payload string) (host string, port int, stop func()) {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("local service listen: %v", err)
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				fmt.Fprintf(c, "%s\n", payload)
				io.Copy(c, c) // echo, so the client->service direction is proven too
			}(c)
		}
	}()
	addr := ln.Addr().(*net.TCPAddr)
	return "127.0.0.1", addr.Port, func() { ln.Close() }
}

// The "mesh" listener is a plain loopback listener here — StartExport does not care
// how the listener was produced, which is exactly why this logic can be tested
// without tsnet, NetBird, cgo or a network.
func TestExportProxiesBothDirections(t *testing.T) {
	host, port, stop := startLocalService(t, "hello-from-local-service")
	defer stop()

	meshLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("mesh listen: %v", err)
	}
	exp := StartExport(meshLn, host, port)
	defer exp.Close()

	c, err := net.DialTimeout("tcp", exp.Addr(), 5*time.Second)
	if err != nil {
		t.Fatalf("peer dial: %v", err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(5 * time.Second))

	r := bufio.NewReader(c)
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read greeting through the export: %v", err)
	}
	if strings.TrimSpace(line) != "hello-from-local-service" {
		t.Fatalf("greeting = %q, want hello-from-local-service", strings.TrimSpace(line))
	}

	// service -> peer proven above; now peer -> service via the echo.
	if _, err := c.Write([]byte("ping\n")); err != nil {
		t.Fatalf("write: %v", err)
	}
	echo, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read echo: %v", err)
	}
	if strings.TrimSpace(echo) != "ping" {
		t.Fatalf("echo = %q, want ping", strings.TrimSpace(echo))
	}
	if e := exp.LastError(); e != "" {
		t.Fatalf("unexpected export error: %s", e)
	}
}

// A dead local service must not kill the export — the listener stays up and the
// reason is retrievable, because the service may just be restarting.
func TestExportSurvivesDeadLocalService(t *testing.T) {
	// Bind then immediately release a port, so nothing is listening on it.
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("probe listen: %v", err)
	}
	deadPort := probe.Addr().(*net.TCPAddr).Port
	probe.Close()

	meshLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("mesh listen: %v", err)
	}
	exp := StartExport(meshLn, "127.0.0.1", deadPort)
	defer exp.Close()

	c, err := net.DialTimeout("tcp", exp.Addr(), 5*time.Second)
	if err != nil {
		t.Fatalf("peer dial: %v", err)
	}
	c.SetDeadline(time.Now().Add(5 * time.Second))
	io.ReadAll(c) // the proxy drops us because the backend refused
	c.Close()

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) && exp.LastError() == "" {
		time.Sleep(50 * time.Millisecond)
	}
	if e := exp.LastError(); e == "" {
		t.Fatal("a refused backend should be reported via LastError")
	}

	// Crucially, the export is still accepting.
	c2, err := net.DialTimeout("tcp", exp.Addr(), 5*time.Second)
	if err != nil {
		t.Fatalf("export stopped accepting after a backend failure: %v", err)
	}
	c2.Close()
}

func TestExportCloseIsIdempotentAndStopsAccepting(t *testing.T) {
	host, port, stop := startLocalService(t, "x")
	defer stop()

	meshLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("mesh listen: %v", err)
	}
	exp := StartExport(meshLn, host, port)
	addr := exp.Addr()

	exp.Close()
	exp.Close() // must not panic or block

	if c, err := net.DialTimeout("tcp", addr, 1*time.Second); err == nil {
		c.Close()
		t.Fatal("export still accepting after Close")
	}
}

// Many concurrent connections through one export must not be cross-wired.
func TestExportHandlesConcurrentConnections(t *testing.T) {
	const n = 16
	host, port, stop := startLocalService(t, "concurrent")
	defer stop()

	meshLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("mesh listen: %v", err)
	}
	exp := StartExport(meshLn, host, port)
	defer exp.Close()

	errs := make(chan error, n)
	for i := 0; i < n; i++ {
		go func(i int) {
			c, err := net.DialTimeout("tcp", exp.Addr(), 5*time.Second)
			if err != nil {
				errs <- err
				return
			}
			defer c.Close()
			c.SetDeadline(time.Now().Add(5 * time.Second))
			r := bufio.NewReader(c)
			if _, err := r.ReadString('\n'); err != nil { // greeting
				errs <- err
				return
			}
			want := fmt.Sprintf("m%d\n", i)
			if _, err := c.Write([]byte(want)); err != nil {
				errs <- err
				return
			}
			got, err := r.ReadString('\n')
			if err != nil {
				errs <- err
				return
			}
			if got != want {
				errs <- fmt.Errorf("cross-wired: got %q want %q", got, want)
				return
			}
			errs <- nil
		}(i)
	}
	for i := 0; i < n; i++ {
		if err := <-errs; err != nil {
			t.Errorf("connection %d: %v", i, err)
		}
	}
}
