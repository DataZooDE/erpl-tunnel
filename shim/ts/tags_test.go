package main

// Tags used to be a silent no-op: the `tags` secret field was stored and never
// read, so nodes registered untagged however carefully it was set. These tests
// pin the replacement, including the always-on tag:erpl-tunnel that makes every
// node identifiable in the admin console.

import (
	"errors"
	"strings"
	"testing"
)

func TestParseTagsAlwaysIncludesErplTunnel(t *testing.T) {
	cases := []struct {
		in   string
		want []string
	}{
		{"", []string{"tag:erpl-tunnel"}},
		{"   ", []string{"tag:erpl-tunnel"}},
		{"duckdb", []string{"tag:erpl-tunnel", "tag:duckdb"}},       // prefix added
		{"tag:duckdb", []string{"tag:erpl-tunnel", "tag:duckdb"}},
		{"erpl-tunnel", []string{"tag:erpl-tunnel"}},                // not duplicated
		{"tag:erpl-tunnel, prod", []string{"tag:erpl-tunnel", "tag:prod"}},
		{"a, a; b", []string{"tag:erpl-tunnel", "tag:a", "tag:b"}},  // separators + dedupe
	}
	for _, c := range cases {
		got := parseTags(c.in)
		if strings.Join(got, ",") != strings.Join(c.want, ",") {
			t.Fatalf("parseTags(%q) = %v, want %v", c.in, got, c.want)
		}
	}
}

func TestIsTagRejection(t *testing.T) {
	// The control server's wording differs between Tailscale and Headscale and has
	// changed across versions, so the matcher keys on the stable parts.
	for _, m := range []string{
		"requested tags [tag:erpl-tunnel] are invalid or not permitted",
		"tags are not allowed for this node",
		"Tag is unauthorized",
	} {
		if !isTagRejection(errors.New(m)) {
			t.Fatalf("expected tag rejection for %q", m)
		}
	}
	// Must not swallow unrelated failures behind a tagOwners hint.
	for _, m := range []string{"context deadline exceeded", "dial tcp: connection refused", ""} {
		if isTagRejection(errors.New(m)) {
			t.Fatalf("did not expect tag rejection for %q", m)
		}
	}
	if isTagRejection(nil) {
		t.Fatal("nil must not be a tag rejection")
	}
}
