#pragma once

#include "datazoo_banner_duckdb.hpp"

// Shared identity for the load banner and the issue-link error footer.
//
// External linkage on purpose: DATAZOO_GUARD takes the address as a non-type
// template argument, and every guarded translation unit should annotate errors
// against the same object rather than a per-TU copy.
//
// Defined in tunnel_extension.cpp, next to ERPL_TUNNEL_VERSION — the version is
// bumped there on release and the banner picks it up automatically.
extern const datazoo::BannerInfo ERPL_TUNNEL_BANNER;
