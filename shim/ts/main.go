// ts_shim — a Go c-shared library exposing the erpl-tunnel mesh C ABI
// (../mesh_shim.h) over Tailscale's in-process userspace node, tsnet.
//
// Built with `go build -buildmode=c-shared`. The extension embeds the resulting
// .so/.dylib/.dll as a byte blob and loads it lazily on first Tailscale activation
// (HLD §6.5). Handles are opaque ints into a Go-side registry; dialled streams are
// handed to C as OS socket handles via a local stream pair (see shim/meshpair) so
// the C++ pump never touches Go memory.
package main

/*
#include <stdlib.h>
#include <stdint.h>

// Must match `mesh_stream` in ../mesh_shim.h: fd on Unix, Winsock SOCKET on Windows.
typedef uintptr_t mesh_stream;
*/
import "C"

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"time"
	"unsafe"

	"github.com/DataZooDE/erpl-tunnel/shim/meshpair"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/tsnet"
)

type node struct {
	mu         sync.Mutex
	srv        *tsnet.Server
	authKey    string
	hostname   string
	tags       string
	controlURL string
	stateDir   string
	ephemeral  bool
	up         bool
	lastErr    string
	exports    map[C.long]*meshpair.Exporter
	nextExport C.long
}

// --- exported listeners (mesh_export) --------------------------------------
// Kept per node so mesh_close can tear them all down; handles are opaque longs,
// same convention as node handles, because Go pointers must never cross into C.
func (n *node) addExport(e *meshpair.Exporter) C.long {
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.exports == nil {
		n.exports = map[C.long]*meshpair.Exporter{}
	}
	n.nextExport++
	h := n.nextExport
	n.exports[h] = e
	return h
}

func (n *node) takeExport(h C.long) *meshpair.Exporter {
	n.mu.Lock()
	defer n.mu.Unlock()
	e := n.exports[h]
	delete(n.exports, h)
	return e
}

func (n *node) closeAllExports() {
	n.mu.Lock()
	all := make([]*meshpair.Exporter, 0, len(n.exports))
	for _, e := range n.exports {
		all = append(all, e)
	}
	n.exports = nil
	n.mu.Unlock()
	for _, e := range all {
		e.Close()
	}
}

func (n *node) setErr(err error) {
	n.mu.Lock()
	n.lastErr = err.Error()
	n.mu.Unlock()
}

var (
	regMu sync.Mutex
	reg          = map[C.long]*node{}
	nextH C.long = 1
)

func get(h C.long) *node {
	regMu.Lock()
	defer regMu.Unlock()
	return reg[h]
}

//export mesh_kind
func mesh_kind() C.int { return C.int(1) } // MESH_KIND_TAILSCALE

//export mesh_new
func mesh_new() C.long {
	regMu.Lock()
	defer regMu.Unlock()
	h := nextH
	nextH++
	reg[h] = &node{}
	return h
}

//export mesh_set_str
func mesh_set_str(h C.long, key *C.char, val *C.char) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	k := C.GoString(key)
	v := C.GoString(val)
	n.mu.Lock()
	defer n.mu.Unlock()
	switch k {
	case "auth_key":
		n.authKey = v
	case "hostname":
		n.hostname = v
	case "tags":
		n.tags = v
	case "control_url":
		n.controlURL = v
	case "state_dir":
		n.stateDir = v
	}
	return 0
}

//export mesh_set_bool
func mesh_set_bool(h C.long, key *C.char, val C.int) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	k := C.GoString(key)
	n.mu.Lock()
	defer n.mu.Unlock()
	if k == "ephemeral" {
		n.ephemeral = val != 0
	}
	return 0
}

//export mesh_up
func mesh_up(h C.long) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	if n.up {
		n.mu.Unlock()
		return 0 // idempotent
	}
	if n.authKey == "" {
		n.lastErr = "Tailscale: no auth key set. Generate a reusable, tagged key at " +
			"https://login.tailscale.com/admin/settings/keys and set auth_key on the secret."
		n.mu.Unlock()
		return 1
	}
	dir := n.stateDir
	if dir == "" {
		d, err := os.MkdirTemp("", "erpl-tunnel-ts-")
		if err != nil {
			n.lastErr = fmt.Sprintf("Tailscale: cannot create state dir: %v", err)
			n.mu.Unlock()
			return 1
		}
		dir = d
	}
	srv := &tsnet.Server{
		Hostname:   n.hostname,
		AuthKey:    n.authKey,
		Dir:        dir,
		Ephemeral:  n.ephemeral,
		ControlURL: n.controlURL, // empty => Tailscale cloud
	}
	n.srv = srv
	n.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	if _, err := srv.Up(ctx); err != nil {
		n.setErr(fmt.Errorf("Tailscale: node did not come up: %w", err))
		return 1
	}
	n.mu.Lock()
	n.up = true
	n.mu.Unlock()
	return 0
}

// bridge copies bytes both ways between the Go-side socketpair end and the mesh
// connection, then closes both. This is what turns a userspace (netstack) conn
// into something C reads/writes as a plain fd.
func bridge(local net.Conn, mesh net.Conn) {
	done := make(chan struct{}, 2)
	cp := func(dst, src net.Conn) {
		io.Copy(dst, src)
		// half-close the write side so the peer sees EOF
		if cw, ok := dst.(interface{ CloseWrite() error }); ok {
			cw.CloseWrite()
		}
		done <- struct{}{}
	}
	go cp(mesh, local)
	go cp(local, mesh)
	<-done
	<-done
	local.Close()
	mesh.Close()
}

//export mesh_export
func mesh_export(h C.long, meshPort C.int, localHost *C.char, localPort C.int, out *C.long) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	up, srv := n.up, n.srv
	n.mu.Unlock()
	if !up || srv == nil {
		return 1
	}
	// ":port" binds this node's own tailnet addresses, which is what a peer dials.
	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", int(meshPort)))
	if err != nil {
		n.setErr(fmt.Errorf("Tailscale: could not listen on mesh port %d: %w", int(meshPort), err))
		return 1
	}
	*out = n.addExport(meshpair.StartExport(ln, C.GoString(localHost), int(localPort)))
	return 0
}

//export mesh_unexport
func mesh_unexport(h C.long, exportHandle C.long) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	if e := n.takeExport(exportHandle); e != nil {
		e.Close()
	}
	return 0 // idempotent: an unknown handle is not an error
}

//export mesh_dial
func mesh_dial(h C.long, host *C.char, port C.int, streamOut *C.mesh_stream) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	up := n.up
	srv := n.srv
	n.mu.Unlock()
	if !up || srv == nil {
		return 1
	}

	// One end goes to C, the other is bridged in Go. See shim/meshpair.
	raw, goConn, err := meshpair.New()
	if err != nil {
		n.setErr(fmt.Errorf("Tailscale: stream pair failed: %w", err))
		return 1
	}

	addr := fmt.Sprintf("%s:%d", C.GoString(host), int(port))
	dctx, dcancel := context.WithTimeout(context.Background(), 30*time.Second)
	meshConn, err := srv.Dial(dctx, "tcp", addr)
	dcancel()
	if err != nil {
		meshpair.CloseRaw(raw)
		goConn.Close()
		n.setErr(fmt.Errorf("Tailscale: dial %s failed: %w", addr, err))
		return 1
	}

	go bridge(goConn, meshConn)
	*streamOut = C.mesh_stream(raw)
	return 0
}

type peerJSON struct {
	Backend  string   `json:"backend"`
	HostName string   `json:"host_name"`
	DNSName  string   `json:"dns_name"`
	MeshIP   string   `json:"mesh_ip"`
	Tags     []string `json:"tags"`
	Online   bool     `json:"online"`
}

func peersFromStatus(st *ipnstate.Status) []peerJSON {
	out := []peerJSON{}
	if st == nil {
		return out
	}
	for _, p := range st.Peer {
		pj := peerJSON{
			Backend:  "tailscale",
			HostName: p.HostName,
			DNSName:  strings.TrimSuffix(p.DNSName, "."),
			Online:   p.Online,
			Tags:     []string{},
		}
		if len(p.TailscaleIPs) > 0 {
			pj.MeshIP = p.TailscaleIPs[0].String()
		}
		if p.Tags != nil {
			for i := 0; i < p.Tags.Len(); i++ {
				pj.Tags = append(pj.Tags, p.Tags.At(i))
			}
		}
		out = append(out, pj)
	}
	return out
}

func writeJSON(v interface{}, buf *C.char, length C.size_t, need *C.size_t) C.int {
	b, err := json.Marshal(v)
	if err != nil {
		return 1
	}
	if need != nil {
		*need = C.size_t(len(b) + 1)
	}
	if int(length) < len(b)+1 {
		return 2 // buffer too small; caller retries with *need bytes
	}
	dst := unsafe.Slice((*byte)(unsafe.Pointer(buf)), int(length))
	copy(dst, b)
	dst[len(b)] = 0
	return 0
}

//export mesh_peers_json
func mesh_peers_json(h C.long, buf *C.char, length C.size_t, need *C.size_t) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	srv := n.srv
	up := n.up
	n.mu.Unlock()
	if !up || srv == nil {
		return 1
	}
	lc, err := srv.LocalClient()
	if err != nil {
		n.setErr(err)
		return 1
	}
	st, err := lc.Status(context.Background())
	if err != nil {
		n.setErr(err)
		return 1
	}
	return writeJSON(peersFromStatus(st), buf, length, need)
}

//export mesh_self_json
func mesh_self_json(h C.long, buf *C.char, length C.size_t, need *C.size_t) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	srv := n.srv
	up := n.up
	n.mu.Unlock()
	if !up || srv == nil {
		return 1
	}
	lc, err := srv.LocalClient()
	if err != nil {
		n.setErr(err)
		return 1
	}
	st, err := lc.Status(context.Background())
	if err != nil {
		n.setErr(err)
		return 1
	}
	self := peerJSON{Backend: "tailscale", Tags: []string{}}
	if st.Self != nil {
		self.HostName = st.Self.HostName
		self.DNSName = strings.TrimSuffix(st.Self.DNSName, ".")
		self.Online = st.Self.Online
		if len(st.Self.TailscaleIPs) > 0 {
			self.MeshIP = st.Self.TailscaleIPs[0].String()
		}
		if st.Self.Tags != nil {
			for i := 0; i < st.Self.Tags.Len(); i++ {
				self.Tags = append(self.Tags, st.Self.Tags.At(i))
			}
		}
	}
	return writeJSON(self, buf, length, need)
}

//export mesh_errmsg
func mesh_errmsg(h C.long, buf *C.char, length C.size_t) C.int {
	n := get(h)
	if n == nil || length == 0 {
		return 0
	}
	n.mu.Lock()
	msg := n.lastErr
	n.mu.Unlock()
	if len(msg) >= int(length) {
		msg = msg[:int(length)-1]
	}
	dst := unsafe.Slice((*byte)(unsafe.Pointer(buf)), int(length))
	copy(dst, msg)
	dst[len(msg)] = 0
	return C.int(len(msg))
}

//export mesh_close
func mesh_close(h C.long) C.int {
	n := get(h)
	if n == nil {
		return 0
	}
	n.mu.Lock()
	srv := n.srv
	n.srv = nil
	n.up = false
	n.mu.Unlock()
	if srv != nil {
		n.closeAllExports()
		srv.Close()
	}
	regMu.Lock()
	delete(reg, h)
	regMu.Unlock()
	return 0
}

func main() {} // required for c-shared
