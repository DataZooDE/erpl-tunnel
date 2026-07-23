// ts_shim — a Go c-shared library exposing the erpl-tunnel mesh C ABI
// (../mesh_shim.h) over Tailscale's in-process userspace node, tsnet.
//
// Built with `go build -buildmode=c-shared`. The extension embeds the resulting
// .so/.dylib as a byte blob and dlopen's it lazily on first Tailscale activation
// (HLD §6.5). Handles are opaque ints into a Go-side registry; dialled streams
// are handed to C as OS file descriptors via a socketpair bridge (the
// libtailscale fd trick) so the C++ pump never touches Go memory.
package main

/*
#include <stdlib.h>
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

	"golang.org/x/sys/unix"
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
}

func (n *node) setErr(err error) {
	n.mu.Lock()
	n.lastErr = err.Error()
	n.mu.Unlock()
}

var (
	regMu sync.Mutex
	reg   = map[C.long]*node{}
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

//export mesh_dial
func mesh_dial(h C.long, host *C.char, port C.int, fdOut *C.int) C.int {
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

	// AF_UNIX socketpair: fds[0] handed to C, fds[1] bridged in Go.
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM, 0)
	if err != nil {
		n.setErr(fmt.Errorf("Tailscale: socketpair failed: %w", err))
		return 1
	}
	goFile := os.NewFile(uintptr(fds[1]), "mesh-bridge")
	goConn, err := net.FileConn(goFile)
	goFile.Close() // net.FileConn dup'd the fd
	if err != nil {
		unix.Close(fds[0])
		n.setErr(fmt.Errorf("Tailscale: FileConn failed: %w", err))
		return 1
	}

	addr := fmt.Sprintf("%s:%d", C.GoString(host), int(port))
	dctx, dcancel := context.WithTimeout(context.Background(), 30*time.Second)
	meshConn, err := srv.Dial(dctx, "tcp", addr)
	dcancel()
	if err != nil {
		unix.Close(fds[0])
		goConn.Close()
		n.setErr(fmt.Errorf("Tailscale: dial %s failed: %w", addr, err))
		return 1
	}

	go bridge(goConn, meshConn)
	*fdOut = C.int(fds[0])
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
		srv.Close()
	}
	regMu.Lock()
	delete(reg, h)
	regMu.Unlock()
	return 0
}

func main() {} // required for c-shared
