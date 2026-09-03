#pragma once
// O backend de produção: um ring de io_uring por thread de partição.
//
// Este é o único cabeçalho do WAL que arrasta `<liburing.h>`, e é de propósito: quem compõe o
// programa inclui; o resto do motor fala com `IoBackend` e não sabe que io_uring existe. Foi o
// que permitiu escrever `PwriteBackend` e `FaultBackend` sem tocar em nada do WAL.
//
// Configuração fixada em docs/wal.md e medida em docs/ambiente.md:
//   IORING_SETUP_SINGLE_ISSUER  — só a thread da partição submete; o kernel dispensa travas
//                                 internas que existem para o caso geral de vários submissores.
//   IORING_SETUP_DEFER_TASKRUN  — o trabalho de completion roda quando ESTA thread entra no
//                                 kernel, e não por IPI no meio do `apply`. É a diferença entre
//                                 uma cauda de latência previsível e uma interrupção no pior
//                                 momento possível. Exige SINGLE_ISSUER e kernel ≥ 6.1.
// A máquina de referência aceita as duas (probe registrado em docs/ambiente.md), mas o fallback
// para ring simples fica: um binário que não sobe num kernel antigo é um binário que ninguém
// consegue depurar em outra máquina.

#include <liburing.h>

#include <cstdint>
#include <span>

#include "wal/io_backend.hpp"

namespace rv::wal {

class UringBackend final : public IoBackend {
 public:
  UringBackend() noexcept = default;
  ~UringBackend() override;

  [[nodiscard]] const char* name() const noexcept override { return "io_uring"; }

  // Cria o ring. `entries` é o tamanho da SQ; `kMaxInflight` já é o teto lógico de grupos em voo,
  // então pedir mais só desperdiça memória fixada.
  [[nodiscard]] Status open(uint32_t entries = kMaxInflight) noexcept;
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // O que foi EFETIVAMENTE conseguido, não o que foi pedido. Vai para o log de abertura: um
  // relatório de latência sem esta linha é um relatório sobre uma configuração desconhecida.
  [[nodiscard]] bool defer_taskrun() const noexcept { return defer_taskrun_; }
  [[nodiscard]] bool single_issuer() const noexcept { return single_issuer_; }

  // Registro de arquivos: elimina a busca na tabela de descritores a cada submissão, e é o que
  // torna `IOSQE_FIXED_FILE` legal. Registro de buffers: fixa as páginas uma vez, para o kernel
  // não repetir `get_user_pages` por escrita — é o outro metade do `write_fixed`.
  [[nodiscard]] Status register_files(std::span<const int> fds) noexcept override;
  [[nodiscard]] Status register_buffers(std::span<const MutBytes> bufs) noexcept override;

  [[nodiscard]] Status submit(const WriteRequest& req) noexcept override;
  [[nodiscard]] uint32_t reap(std::span<Completion> out) noexcept override;
  [[nodiscard]] uint32_t inflight() const noexcept override { return inflight_; }

  static constexpr uint32_t kMaxBuffers = 16;

 private:
  // Tabela de pedidos em voo. O CQE só devolve 64 bits de `user_data`, e esses 64 bits já são o
  // `token` (o `last_lsn` do grupo, que o WAL precisa inteiro). O tamanho pedido, portanto, tem
  // de morar do lado de cá — sem ele não há como distinguir escrita curta de escrita completa,
  // que é a checagem obrigatória de docs/wal.md ("Erro ou short write em CQE é fail-stop").
  struct Slot {
    uint64_t token = 0;
    uint32_t expected = 0;
    bool used = false;
  };

  [[nodiscard]] uint32_t take_expected(uint64_t token) noexcept;

  io_uring ring_{};
  Slot slots_[kMaxInflight]{};
  uint32_t inflight_ = 0;
  bool open_ = false;
  bool defer_taskrun_ = false;
  bool single_issuer_ = false;
  bool files_registered_ = false;
  bool buffers_registered_ = false;
};

}  // namespace rv::wal
