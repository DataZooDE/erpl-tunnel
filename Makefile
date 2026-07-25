PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=erpl_tunnel
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# vcpkg provides dependencies (libssh2, openssl).
VCPKG_TOOLCHAIN_PATH?=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# Convenience targets
# ---------------------------------------------------------------------------

# Run every SQL test one-by-one against the debug unittest binary (the runner
# only accepts a single test path per invocation). Integration tests self-skip
# unless the docker sshd harness is up (see test/integration/).
UNITTEST=build/debug/test/unittest
.PHONY: sql_tests
sql_tests: debug
	@for t in test/sql/*.test; do echo "== $$t =="; $(UNITTEST) --test-dir . "$$t" || exit 1; done

# Bring up / tear down the dockerised sshd + private HTTP service used by the
# SSH integration tests (real services, no mocks).
.PHONY: test_up test_down
test_up:
	cd test/integration && docker compose up -d --wait
test_down:
	cd test/integration && docker compose down -v

# SSH bastion connection details for the live integration tests (real docker
# sshd, no mocks). These are scoped to the integration targets ONLY — they are
# deliberately NOT exported globally, so the standard `make test`/`test_release`
# (which the community-extensions CI runs, with no sshd) leaves ERPL_SSH_HOST
# unset and the live-SSH sqllogictests self-skip via `require-env`. Bringing the
# harness up (`make test_up`) and running `make e2e` / `make integration_tests`
# is what exercises them. Override any of these on the command line as needed.
e2e integration_tests: export ERPL_SSH_HOST ?= 127.0.0.1
e2e integration_tests: export ERPL_SSH_PORT ?= 2222
e2e integration_tests: export ERPL_SSH_USER ?= root
e2e integration_tests: export ERPL_SSH_PASSWORD ?= testpass

# Real end-to-end payload test: forward a local port through the docker sshd
# bastion to the private HTTP service and fetch a real response with curl.
# Requires `make test_up` first. Uses the debug build by default.
.PHONY: e2e
e2e: debug
	DUCKDB_BIN=$(PROJ_DIR)build/debug/duckdb \
	  test/integration/e2e_http_over_tunnel.sh \
	  $(PROJ_DIR)build/debug/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Run the SSH integration sqllogictests against the debug binary (services must be up).
.PHONY: integration_tests
integration_tests: debug
	@for t in test/sql/sap_tunnel_integration_*.test; do echo "== $$t =="; \
	  $(UNITTEST) --test-dir . "$$t" || exit 1; done

# --- Mesh backend spikes / proofs (need MESH_BACKEND != ssh) ----------------
# Standalone dlopen spike (BRD R1): build the Tailscale shim and prove a Go
# c-shared library loads and executes via dlopen. No DuckDB, no network.
.PHONY: spike
spike:
	cd shim/ts && CGO_ENABLED=1 GOFLAGS=-mod=mod GOTOOLCHAIN=local \
	  go build -buildmode=c-shared -o ts_shim.so .
	cc -o shim/spike_dlopen shim/spike_dlopen.c -ldl
	shim/spike_dlopen shim/ts/ts_shim.so

# Lazy-dlopen + zero-Go proof against the built extension (HLD §6.5/§8.8).
# Requires a mesh build, e.g.:  MESH_BACKEND=tailscale make debug lazy_load_test
.PHONY: lazy_load_test
lazy_load_test:
	DUCKDB_BIN=$(PROJ_DIR)build/debug/duckdb \
	  test/integration/lazy_mesh_load.sh \
	  $(PROJ_DIR)build/debug/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Real Tailscale enrollment against a hermetic Headscale (ADR-009). Needs a
# tailscale build:  MESH_BACKEND=tailscale make debug mesh_e2e
.PHONY: mesh_e2e mesh_up mesh_down
mesh_up:
	cd test/integration/headscale && docker compose up -d --wait
mesh_down:
	cd test/integration/headscale && docker compose down -v
mesh_e2e:
	DUCKDB_BIN=$(PROJ_DIR)build/debug/duckdb \
	  test/integration/mesh_e2e_headscale.sh \
	  $(PROJ_DIR)build/debug/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Single-mesh latch proof (ADR-011). Needs a 'both' build:
#   MESH_BACKEND=both make debug latch_test
.PHONY: latch_test
latch_test:
	DUCKDB_BIN=$(PROJ_DIR)build/debug/duckdb \
	  test/integration/single_mesh_latch.sh \
	  $(PROJ_DIR)build/debug/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Zero-dependency proof (NFR-1/M5): the loadable file links only glibc, and loads
# in a bare, network-isolated container. Needs a RELEASE build: `make release`.
.PHONY: zero_dep
zero_dep:
	DUCKDB_BIN=$(shell command -v duckdb) \
	  test/integration/zero_dependency.sh \
	  $(PROJ_DIR)build/release/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Tailscale data-plane test (Tier 2): real WireGuard traffic to an official
# kernel-TUN tailscale peer. Needs a RELEASE tailscale/both build + docker.
#   MESH_BACKEND=tailscale make release ts_dataplane
.PHONY: ts_dataplane
ts_dataplane:
	DUCKDB_BIN=$(shell command -v duckdb) \
	  test/integration/mesh_dataplane_tailscale.sh \
	  $(PROJ_DIR)build/release/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# NetBird data-plane test (Tier 2): real WireGuard to an official kernel-TUN peer,
# no-IdP self-hosted control plane (seeded store). Builds the seeder if needed.
# Needs a RELEASE netbird/both build + docker.
#   MESH_BACKEND=netbird make release nb_dataplane
.PHONY: nb_dataplane nb_seeder
nb_seeder:
	cd test/integration/netbird/seeder && \
	  CGO_ENABLED=1 GOFLAGS=-mod=mod GOTOOLCHAIN=local go build -o seeder .
nb_dataplane: nb_seeder
	DUCKDB_BIN=$(shell command -v duckdb) \
	  test/integration/mesh_dataplane_netbird.sh \
	  $(PROJ_DIR)build/release/repository/v1.5.4/linux_amd64/erpl_tunnel.duckdb_extension

# Fast, DuckDB-free C++ unit tests for the pure mesh-peers JSON parser (Catch2,
# bundled with duckdb). Seconds; no vcpkg, no docker — just needs the duckdb submodule.
.PHONY: core_tests
core_tests:
	mkdir -p build
	g++ -std=c++17 -O0 -g \
	    -Iduckdb/src/include -Isrc/include -Iduckdb/third_party/yyjson/include -Iduckdb/third_party/catch \
	    test/cpp/test_mesh_peers_json.cpp src/mesh_peers_json.cpp duckdb/third_party/yyjson/yyjson.cpp \
	    -o build/core_tests
	build/core_tests
