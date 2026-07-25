#pragma once

// One uniform view of "a thing the tunnel manager holds".
//
// Before this existed, TunnelManager kept two maps — active_tunnels (SSH) and
// mesh_tunnels — and only two of its eight methods knew about the second. So
// IsTunnelActive, ListTunnels, GetTunnelStatus, GetTunnelError,
// CleanupInactiveTunnels and RemoveTunnel all silently returned "not found" (or
// quietly did nothing) for a perfectly healthy mesh tunnel, and ListTunnels and
// ListTunnelsWithDetails disagreed with each other about what existed.
//
// Adding a third kind (exported listeners) to that shape would have tripled the
// problem, so the maps are collapsed into one keyed by this interface. Every
// manager method then handles every kind by construction rather than by whoever
// remembered to add the second branch.

#include "tunnel_connection.hpp"

#include <memory>
#include <string>
#include <utility>

namespace duckdb {

class TunnelHandle {
public:
    virtual ~TunnelHandle() = default;

    // Tear down. Must be idempotent and must not return until every thread the
    // handle owns has been joined.
    virtual void Close() = 0;

    // The row shown by tunnels(). Every kind renders through the same struct.
    virtual TunnelConnectionAttributes GetAttributes() const = 0;

    // Is this still doing its job? For a listener that means "still listening",
    // NOT "has live connections" — an idle export is perfectly healthy and must
    // not be reaped by CleanupInactiveTunnels.
    virtual bool IsActive() const = 0;

    std::string GetStatus() const { return GetAttributes().status; }
    std::string GetErrorMessage() const { return GetAttributes().error_message; }
};

// Adapts the existing SSH TunnelConnection, which is deliberately NOT made virtual:
// it has hand-written move construction/assignment, and giving it a vtable while it
// is still moved around would be a foot-gun for no benefit.
class SshTunnelHandle : public TunnelHandle {
public:
    explicit SshTunnelHandle(std::shared_ptr<TunnelConnection> conn) : conn_(std::move(conn)) {}

    void Close() override {
        if (conn_) {
            conn_->Close();
        }
    }
    TunnelConnectionAttributes GetAttributes() const override {
        return conn_ ? conn_->GetAttributes() : TunnelConnectionAttributes{};
    }
    bool IsActive() const override { return conn_ && conn_->IsConnected(); }

    const std::shared_ptr<TunnelConnection> &Connection() const { return conn_; }

private:
    std::shared_ptr<TunnelConnection> conn_;
};

} // namespace duckdb
