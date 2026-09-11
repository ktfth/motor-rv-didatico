#pragma once
// metrics_server.hpp — Servidor HTTP/1.1 ultraleve para /metrics, /healthz, /ready e /status.
//
// Projetado para isolamento total em relação ao hot path do motor:
//   - Sockets não-bloqueantes com poll
//   - Zero alocação dinâmica durante o ciclo de requisição
//   - Suporte a execução em background thread ou por poll_once() síncrono em testes
//   - Limite estrito de leitura de cabeçalhos (CODING_RULES §14)

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "edge/observability.hpp"

namespace rv::edge {

class MetricsServer {
 public:
  MetricsServer() noexcept;
  ~MetricsServer() noexcept;

  // Não copiável nem movível
  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  // Inicializa o socket na porta especificada (ou 0 para porta efêmera aleatória do SO).
  // Retorna true se abriu com sucesso.
  bool bind_and_listen(uint16_t port) noexcept;

  // Inicia thread de atendimento em segundo plano com a porta e coletor especificados.
  bool start(uint16_t port, const ObservabilityCollector& collector) noexcept;

  // Encerra a thread e fecha os sockets de forma síncrona e graciosa.
  void stop() noexcept;

  // Processa conexões pendentes por um período máximo de timeout_ms.
  // Útil para testes unitários ou loops integrados.
  void poll_once(int timeout_ms) noexcept;

  [[nodiscard]] bool is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  // Retorna a porta efetiva em que o socket está ouvindo (útil com porta 0).
  [[nodiscard]] uint16_t bound_port() const noexcept { return bound_port_; }

  void set_collector(const ObservabilityCollector* collector) noexcept { collector_ = collector; }

 private:
  void handle_connection(int client_fd) noexcept;
  void run_loop() noexcept;

  int listen_fd_{-1};
  int stop_pipe_[2]{-1, -1};
  uint16_t bound_port_{0};
  std::atomic<bool> running_{false};
  const ObservabilityCollector* collector_{nullptr};
  std::thread worker_thread_{};
};

}  // namespace rv::edge
