package meshpair

// Inbound port-forwarding: accept connections arriving on a mesh listener and
// proxy each one to a local service.
//
// This lives here, next to the stream pair, because both shims need exactly the
// same logic and only differ in how they produce the net.Listener — tsnet's
// Server.Listen versus NetBird's Client.ListenTCP. The interesting property is
// what is NOT here: no fd is handed to C. An accepted connection originates inside
// Go, so exposing it across the C ABI would need the socketpair + SCM_RIGHTS dance
// libtailscale performs, and SCM_RIGHTS does not exist on Windows. Since the whole
// operation is a port-forward, Go can simply dial the local service itself and copy
// bytes, which is both simpler and identical on all three platforms.

import (
	"fmt"
	"io"
	"net"
	"sync"
)

// Exporter owns one mesh listener and the goroutine accepting on it.
type Exporter struct {
	ln        net.Listener
	localAddr string

	mu      sync.Mutex
	conns   map[net.Conn]struct{} // in-flight proxied connections, so Close can end them
	closed  bool
	lastErr error
}

// StartExport begins accepting on ln and proxying to localHost:localPort.
// It takes ownership of ln: Close() closes it.
func StartExport(ln net.Listener, localHost string, localPort int) *Exporter {
	if localHost == "" {
		localHost = "127.0.0.1"
	}
	e := &Exporter{
		ln:        ln,
		localAddr: net.JoinHostPort(localHost, fmt.Sprintf("%d", localPort)),
		conns:     make(map[net.Conn]struct{}),
	}
	go e.acceptLoop()
	return e
}

func (e *Exporter) acceptLoop() {
	for {
		c, err := e.ln.Accept()
		if err != nil {
			e.mu.Lock()
			// A closed listener is the normal shutdown path, not an error worth
			// reporting back to the user.
			if !e.closed {
				e.lastErr = err
			}
			e.mu.Unlock()
			return
		}
		go e.serve(c)
	}
}

func (e *Exporter) serve(meshConn net.Conn) {
	e.track(meshConn)
	defer e.untrack(meshConn)

	local, err := net.Dial("tcp", e.localAddr)
	if err != nil {
		// One dead backend must not kill the export: record why and drop this
		// connection, but keep listening. The service may simply be restarting.
		e.mu.Lock()
		e.lastErr = fmt.Errorf("inbound connection arrived but %s refused it: %w", e.localAddr, err)
		e.mu.Unlock()
		meshConn.Close()
		return
	}
	e.track(local)
	defer e.untrack(local)

	// Same shape as the outbound bridge: full duplex, half-close propagated so the
	// peer sees EOF on one direction without the whole connection being torn down.
	done := make(chan struct{}, 2)
	cp := func(dst, src net.Conn) {
		io.Copy(dst, src)
		if cw, ok := dst.(interface{ CloseWrite() error }); ok {
			cw.CloseWrite()
		}
		done <- struct{}{}
	}
	go cp(local, meshConn)
	go cp(meshConn, local)
	<-done
	<-done
	local.Close()
	meshConn.Close()
}

func (e *Exporter) track(c net.Conn) {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		c.Close()
		return
	}
	e.conns[c] = struct{}{}
	e.mu.Unlock()
}

func (e *Exporter) untrack(c net.Conn) {
	e.mu.Lock()
	delete(e.conns, c)
	e.mu.Unlock()
}

// LastError reports the most recent problem, or "" if there has been none. Used to
// surface a failing export through mesh_errmsg rather than leaving it silently dead.
func (e *Exporter) LastError() string {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.lastErr == nil {
		return ""
	}
	return e.lastErr.Error()
}

// Addr is the mesh address actually being listened on, which is what a peer must
// connect to. Worth surfacing: for NetBird the host part of the requested address
// is discarded and replaced with the node's own mesh IP.
func (e *Exporter) Addr() string {
	if e.ln == nil {
		return ""
	}
	return e.ln.Addr().String()
}

// Close stops accepting and ends every in-flight proxied connection. Idempotent.
func (e *Exporter) Close() {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.closed = true
	ln := e.ln
	conns := make([]net.Conn, 0, len(e.conns))
	for c := range e.conns {
		conns = append(conns, c)
	}
	e.conns = map[net.Conn]struct{}{}
	e.mu.Unlock()

	if ln != nil {
		ln.Close() // unblocks acceptLoop
	}
	// Closing both ends makes the io.Copy pairs return, so serve() unwinds.
	for _, c := range conns {
		c.Close()
	}
}
