// nb_shim — a Go c-shared library exposing the erpl-tunnel mesh C ABI
// (../mesh_shim.h) over NetBird's in-process userspace node, client/embed.
//
// EXPORTS THE IDENTICAL C ABI AS ts_shim (mesh_kind/new/set_str/set_bool/up/dial/
// peers_json/self_json/errmsg/close) so the C++ MeshBackend binds one symbol set
// regardless of which shim the loader dlopen'd (HLD §5.2). mesh_kind() reports 2.
//
// Streams are handed to C as OS fds via the same socketpair bridge as ts_shim.
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
	"sync"
	"time"
	"unsafe"

	nbembed "github.com/netbirdio/netbird/client/embed"
	"golang.org/x/sys/unix"
)

type node struct {
	mu        sync.Mutex
	client    *nbembed.Client
	setupKey  string
	hostname  string
	groups    string
	mgmtURL   string
	statePath string
	ephemeral bool
	up        bool
	lastErr   string
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
func mesh_kind() C.int { return C.int(2) } // MESH_KIND_NETBIRD

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
	case "setup_key":
		n.setupKey = v
	case "hostname":
		n.hostname = v
	case "groups":
		n.groups = v
	case "mgmt_url", "management_url":
		n.mgmtURL = v
	case "state_dir":
		n.statePath = v
	}
	return 0
}

//export mesh_set_bool
func mesh_set_bool(h C.long, key *C.char, val C.int) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	if C.GoString(key) == "ephemeral" {
		n.mu.Lock()
		n.ephemeral = val != 0
		n.mu.Unlock()
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
		return 0
	}
	if n.setupKey == "" {
		n.lastErr = "NetBird: no setup key set. Create one in the NetBird dashboard " +
			"(Setup Keys) and set setup_key on the secret."
		n.mu.Unlock()
		return 1
	}
	statePath := n.statePath
	if statePath == "" {
		d, err := os.MkdirTemp("", "erpl-tunnel-nb-")
		if err != nil {
			n.lastErr = fmt.Sprintf("NetBird: cannot create state dir: %v", err)
			n.mu.Unlock()
			return 1
		}
		statePath = d + "/state.json"
	}
	opts := nbembed.Options{
		DeviceName:    n.hostname,
		SetupKey:      n.setupKey,
		ManagementURL: n.mgmtURL, // empty => NetBird cloud
		StatePath:     statePath,
	}
	n.mu.Unlock()

	client, err := nbembed.New(opts)
	if err != nil {
		n.setErr(fmt.Errorf("NetBird: client init failed: %w", err))
		return 1
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	if err := client.Start(ctx); err != nil {
		n.setErr(fmt.Errorf("NetBird: node did not come up: %w", err))
		return 1
	}
	n.mu.Lock()
	n.client = client
	n.up = true
	n.mu.Unlock()
	return 0
}

func bridge(local net.Conn, mesh net.Conn) {
	done := make(chan struct{}, 2)
	cp := func(dst, src net.Conn) {
		io.Copy(dst, src)
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
	client := n.client
	up := n.up
	n.mu.Unlock()
	if !up || client == nil {
		return 1
	}
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM, 0)
	if err != nil {
		n.setErr(fmt.Errorf("NetBird: socketpair failed: %w", err))
		return 1
	}
	goFile := os.NewFile(uintptr(fds[1]), "mesh-bridge")
	goConn, err := net.FileConn(goFile)
	goFile.Close()
	if err != nil {
		unix.Close(fds[0])
		n.setErr(fmt.Errorf("NetBird: FileConn failed: %w", err))
		return 1
	}
	addr := fmt.Sprintf("%s:%d", C.GoString(host), int(port))
	dctx, dcancel := context.WithTimeout(context.Background(), 30*time.Second)
	meshConn, err := client.Dial(dctx, "tcp", addr)
	dcancel()
	if err != nil {
		unix.Close(fds[0])
		goConn.Close()
		n.setErr(fmt.Errorf("NetBird: dial %s failed: %w", addr, err))
		return 1
	}
	go bridge(goConn, meshConn)
	*fdOut = C.int(fds[0])
	return 0
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
		return 2
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
	// NetBird's embed surface does not currently expose a stable peer-status
	// accessor; peer enumeration is best-effort empty for now (documented, R3).
	// Self identity is still available via mesh_self_json.
	return writeJSON([]map[string]interface{}{}, buf, length, need)
}

//export mesh_self_json
func mesh_self_json(h C.long, buf *C.char, length C.size_t, need *C.size_t) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	up := n.up
	host := n.hostname
	n.mu.Unlock()
	self := map[string]interface{}{
		"backend":   "netbird",
		"host_name": host,
		"dns_name":  "",
		"mesh_ip":   "",
		"tags":      []string{},
		"online":    up,
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
	client := n.client
	n.client = nil
	n.up = false
	n.mu.Unlock()
	if client != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		client.Stop(ctx)
		cancel()
	}
	regMu.Lock()
	delete(reg, h)
	regMu.Unlock()
	return 0
}

func main() {}
