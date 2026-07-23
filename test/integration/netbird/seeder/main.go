// seeder — produce a management-ready NetBird sqlite store (store.db) seeded from
// NetBird's own store.sql fixture (account + a reusable setup key), so the
// self-hosted management can enroll peers WITHOUT an IdP.
//
// The fixture's reusable setup key has plaintext A2C8E62B-38F5-4553-B31E-DD66C696CEBB
// (NetBird's peer_test.go enrolls with it). Its account carries a network, so
// enrolling peers get overlay IPs. Run: seeder <store.sql> <out-data-dir>.
package main

import (
	"context"
	"fmt"
	"os"

	"github.com/netbirdio/netbird/management/server/store"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: seeder <store.sql> <out-data-dir>")
		os.Exit(2)
	}
	sqlFile, dataDir := os.Args[1], os.Args[2]
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		panic(err)
	}
	ctx := context.Background()
	// Writes <dataDir>/store.db, runs migrations (hashes the setup key), adds the
	// "All" group — exactly what a fresh management expects.
	_, cleanup, err := store.NewTestStoreFromSQL(ctx, sqlFile, dataDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "seed failed: %v\n", err)
		os.Exit(1)
	}
	if cleanup != nil {
		cleanup()
	}
	fmt.Printf("seeded %s/store.db from %s\n", dataDir, sqlFile)
}
