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
