// nb_shim — a Go c-shared library exposing the erpl-tunnel mesh C ABI
// (../mesh_shim.h) over NetBird's in-process userspace node, client/embed.
//
// EXPORTS THE IDENTICAL C ABI AS ts_shim (mesh_kind/new/set_str/set_bool/up/dial/
// peers_json/self_json/errmsg/close) so the C++ MeshBackend binds one symbol set
// regardless of which shim the loader dlopen'd (HLD §5.2). mesh_kind() reports 2.
//
// Streams are handed to C as OS socket handles via the same local stream pair as
// ts_shim (see shim/meshpair).
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
	"path/filepath"
	"sync"
	"time"
	"unsafe"

	"github.com/DataZooDE/erpl-tunnel/shim/meshpair"
	nbembed "github.com/netbirdio/netbird/client/embed"
)

type node struct {
	mu         sync.Mutex
	client     *nbembed.Client
	setupKey   string
	hostname   string
	groups     string
	mgmtURL    string
	statePath  string
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
		statePath = filepath.Join(d, "state.json")
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

//export mesh_export
func mesh_export(h C.long, meshPort C.int, localHost *C.char, localPort C.int, out *C.long) C.int {
	n := get(h)
	if n == nil {
		return 1
	}
	n.mu.Lock()
	up, client := n.up, n.client
	n.mu.Unlock()
	if !up || client == nil {
		return 1
	}
	// ListenTCP discards the host part and always binds this node's own NetBird
	// address, but it still requires a parseable host:port, so ":port" it is.
	ln, err := client.ListenTCP(fmt.Sprintf(":%d", int(meshPort)))
	if err != nil {
		n.setErr(fmt.Errorf("NetBird: could not listen on mesh port %d: %w", int(meshPort), err))
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
	client := n.client
	up := n.up
	n.mu.Unlock()
	if !up || client == nil {
		return 1
	}
	// One end goes to C, the other is bridged in Go. See shim/meshpair.
	raw, goConn, err := meshpair.New()
	if err != nil {
		n.setErr(fmt.Errorf("NetBird: stream pair failed: %w", err))
		return 1
	}
	addr := fmt.Sprintf("%s:%d", C.GoString(host), int(port))
	dctx, dcancel := context.WithTimeout(context.Background(), 30*time.Second)
	meshConn, err := client.Dial(dctx, "tcp", addr)
	dcancel()
	if err != nil {
		meshpair.CloseRaw(raw)
		goConn.Close()
		n.setErr(fmt.Errorf("NetBird: dial %s failed: %w", addr, err))
		return 1
	}
	go bridge(goConn, meshConn)
	*streamOut = C.mesh_stream(raw)
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
		n.closeAllExports()
		client.Stop(ctx)
		cancel()
	}
	regMu.Lock()
	delete(reg, h)
	regMu.Unlock()
	return 0
}

func main() {}
