#pragma once
// O que o núcleo EXIGE de um log — e nada além disso.
//
// O núcleo declara a interface; quem a fornece é problema de quem compõe o programa. Inversão de
// dependência sem vtable: `Partition<J>` é template sobre o journal, então o compilador enxerga a
// implementação e embute `append`, que é chamado uma vez por evento — milhões por segundo.
//
// A assimetria vale registrar, porque ela se repete no WAL: `concept` onde a chamada é POR
// EVENTO; função virtual onde a chamada é POR GRUPO (`IoBackend`, ADR-0023). No segundo caso a
// indireção acontece a cada ~10 mil eventos e o custo some no ruído, enquanto o ganho — trocar
// io_uring por pwrite numa linha de configuração, e testar I8..I11 sem kernel especial — é grande.

#include <concepts>
#include <span>

#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"

namespace rv::core {

// O que `append` devolve: o LSN atribuído e a ÚNICA cópia do payload, já no buffer de commit.
// `apply` lê dali — não há segunda cópia entre o log e o estado.
struct Appended {
  Lsn lsn;
  const std::byte* payload;
  uint16_t len;
};
static_assert(sizeof(Appended) <= 24);

template <class J>
concept Journal = requires(J j, uint16_t tmpl, ByteSpan payload, uint64_t ts_ns, uint64_t now_ns) {
  // Copia o payload uma vez para o buffer de commit, escreve o cabeçalho, devolve o LSN.
  // Falha com `WalFull` quando não há buffer livre ou `kMaxInflight` está cheio — falha
  // TRANSITÓRIA e SEM EFEITO: o chamador tenta de novo na próxima volta do loop.
  { j.append(tmpl, payload, ts_ns) } noexcept -> std::same_as<Result<Appended>>;

  // O ÚNICO ponto do núcleo que recebe o relógio. Fecha o grupo se a janela venceu ou se ele
  // encheu. Nunca é chamado de dentro do `apply`.
  { j.maybe_submit(now_ns) } noexcept -> std::same_as<Status>;

  // Colhe completions e avança `durable_lsn` em ordem FIFO (I9).
  { j.reap() } noexcept -> std::same_as<Status>;

  // O maior LSN cujo grupo — E TODOS OS ANTERIORES — completou. Monótono; nunca maior que
  // `last_lsn` (I9). É o portão do outbox (I10).
  { j.durable_lsn() } noexcept -> std::same_as<Lsn>;

  // O maior LSN já entregue por `append`.
  { j.last_lsn() } noexcept -> std::same_as<Lsn>;

  // Uma vez verdadeiro, para sempre.
  { j.halted() } noexcept -> std::same_as<bool>;
};

}  // namespace rv::core
