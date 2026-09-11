#pragma once
// Adapter TCP não-bloqueante. Cada conexão possui seu próprio estado de framing;
// compartilhar um IngressPipeline entre clientes misturaria fragmentos de streams distintos.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/partition.hpp"
#include "ingress/ingress_pipeline.hpp"

namespace rv::ingress {

struct TcpIngressStats {
  uint64_t connections_accepted{0};
  uint64_t connections_closed{0};
  uint64_t connection_errors{0};
  uint64_t active_connections{0};
  IngressStats ingress{};
};

class TcpIngressServer {
 public:
  static constexpr size_t kMaxClients = 128;

  TcpIngressServer() noexcept;
  ~TcpIngressServer() noexcept;
  TcpIngressServer(const TcpIngressServer&) = delete;
  TcpIngressServer& operator=(const TcpIngressServer&) = delete;

  [[nodiscard]] bool start(uint16_t port, std::span<core::Inbox*> inboxes) noexcept;
  void stop() noexcept;
  void poll_once(int timeout_ms, uint64_t now_ns) noexcept;

  [[nodiscard]] bool is_running() const noexcept { return listen_fd_ >= 0; }
  [[nodiscard]] uint16_t bound_port() const noexcept { return bound_port_; }
  [[nodiscard]] size_t client_count() const noexcept { return clients_.size(); }
  [[nodiscard]] TcpIngressStats stats() const noexcept;

 private:
  struct Client;
  void accept_ready() noexcept;
  void close_client(size_t index, bool error) noexcept;

  int listen_fd_{-1};
  uint16_t bound_port_{0};
  std::vector<core::Inbox*> inboxes_{};
  std::vector<std::unique_ptr<Client>> clients_{};
  TcpIngressStats retired_{};
};

}  // namespace rv::ingress
