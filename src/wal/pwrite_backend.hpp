#pragma once
// Backend síncrono, com ORDEM DE COMPLETION PROGRAMÁVEL.
//
// Duas razões para existir, e as duas valem o arquivo:
//
// 1. Portabilidade e depuração. `pwrite` funciona em qualquer kernel, em qualquer filesystem,
//    dentro de qualquer contêiner de CI. Um teste de invariante que só roda com kernel ≥ 6.1 e
//    NVMe específico não roda onde mais importa (ADR-0023).
// 2. I9 — `durable_lsn` avança apenas em ordem FIFO de grupos. Com io_uring de verdade, a ordem
//    das completions é do dispositivo: para observar uma inversão é preciso ter sorte. Aqui a
//    ordem é um botão. `Order::Lifo` devolve o grupo mais novo primeiro, que é o cenário
//    "grupo N+1 completo, grupo N ainda não" — e é exatamente onde `durable_lsn` erraria se
//    avançasse pelo maior LSN visto em vez de pelo prefixo completo.
//
// A escrita em si é imediata (é `pwrite`); o que fica pendurado é apenas a NOTIFICAÇÃO. Isso é o
// suficiente para exercitar a lógica de ordenação do WAL, e é honesto quanto ao que não simula:
// aqui os bytes já estão no disco quando a completion é entregue, então este backend não
// reproduz "grupo N falhou depois que N+1 completou" com dados de verdade — para isso existe o
// `FaultBackend`.

#include <cstdint>
#include <span>

#include "wal/io_backend.hpp"

namespace rv::wal {

class PwriteBackend final : public IoBackend {
 public:
  // A ordem em que `reap` entrega o que já foi escrito.
  enum class Order : uint8_t {
    Fifo = 0,  // ordem de submissão: o caso bem comportado
    Lifo = 1,  // o mais novo primeiro: dispositivo reordenando, o cenário de I9
  };

  PwriteBackend() noexcept = default;
  ~PwriteBackend() override = default;

  [[nodiscard]] const char* name() const noexcept override { return "pwrite"; }

  // Os fds NÃO são adotados: quem abriu o segmento continua dono dele. O backend só memoriza a
  // tradução `file_idx -> fd`, que é o mesmo contrato do registro de arquivos do io_uring.
  [[nodiscard]] Status register_files(std::span<const int> fds) noexcept override;
  [[nodiscard]] Status register_buffers(std::span<const MutBytes> bufs) noexcept override;

  [[nodiscard]] Status submit(const WriteRequest& req) noexcept override;
  [[nodiscard]] uint32_t reap(std::span<Completion> out) noexcept override;
  [[nodiscard]] uint32_t inflight() const noexcept override { return count_; }

  void set_completion_order(Order o) noexcept { order_ = o; }
  [[nodiscard]] Order completion_order() const noexcept { return order_; }

  // Quantos bytes este backend já entregou ao kernel. Só para métrica e teste.
  [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_; }

  static constexpr uint32_t kMaxFiles = 8;
  static constexpr uint32_t kMaxBuffers = 16;

 private:
  struct RegBuf {
    const std::byte* base = nullptr;
    size_t len = 0;
  };

  // Fila circular de completions prontas. `kMaxInflight` é o teto porque o WAL não submete mais
  // que isso sem colher; passar disso é `WalFull`, não crescimento.
  Completion ring_[kMaxInflight]{};
  uint32_t head_ = 0;
  uint32_t count_ = 0;

  int fds_[kMaxFiles]{};
  uint32_t n_files_ = 0;
  RegBuf bufs_[kMaxBuffers]{};
  uint32_t n_bufs_ = 0;

  Order order_ = Order::Fifo;
  uint64_t bytes_written_ = 0;
};

}  // namespace rv::wal
