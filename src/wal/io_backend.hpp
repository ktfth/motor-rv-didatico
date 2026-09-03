#pragma once
// A interface estreita entre o WAL e o mundo que grava bytes (ADR-0023).
//
// ------------------------------------------------------------------------------- por que virtual
// O projeto proíbe indireção no caminho quente, e com razão: `append` roda milhões de vezes por
// segundo. Mas `submit` e `reap` NÃO rodam por evento — rodam por GRUPO. Com janela de 100 µs e
// grupos de 64 KiB, é uma chamada a cada ~10 mil eventos; a chamada indireta some no ruído do
// próprio `io_uring_enter`. `core/journal.hpp` já registra essa assimetria como regra do projeto:
// `concept` onde a chamada é por evento, função virtual onde é por grupo.
//
// O que a vtable compra em troca: trocar io_uring por pwrite numa linha da composição, e um
// `FaultBackend` que injeta escrita curta, erro e reordenação de completion — sem o qual I9 e I10
// dependem de sorte com o dispositivo. ADR-0023 previa parâmetro de template; a decisão que ficou
// de pé no código é a virtual, pelo motivo acima, e o texto do ADR precisa acompanhar.
//
// A interface é um COMPROMISSO PÚBLICO: cinco operações, nenhuma delas conhecendo LSN, grupo,
// segmento ou CRC. O backend transporta bytes; a política de durabilidade é do WAL.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "base/bytes.hpp"
#include "base/status.hpp"

namespace rv::wal {

// Uma escrita alinhada, já pronta. Quem monta o pedido garantiu buffer, offset e tamanho
// múltiplos do bloco (`block_align.hpp`); o backend não corrige nada — corrigir aqui esconderia
// exatamente o bug que `O_DIRECT` existe para expor.
struct WriteRequest {
  const std::byte* buf;  // dentro do buffer registrado em `buf_idx`
  uint64_t offset;       // offset absoluto no arquivo, múltiplo do bloco
  uint64_t token;        // `last_lsn` do grupo: é o que volta na Completion
  uint32_t len;          // múltiplo do bloco
  uint32_t buf_idx;      // índice do buffer registrado
  uint32_t file_idx;     // índice do arquivo registrado
  uint32_t pad_ = 0;     // buraco explícito; a struct viaja em fila, não em disco
};

static_assert(sizeof(WriteRequest) == 40);
static_assert(std::is_trivially_copyable_v<WriteRequest>);

// O que uma completion diz, e nada além: qual pedido terminou e como.
//
// `res` segue a convenção do CQE — bytes escritos, ou `-errno`. `expected` vem junto porque só o
// backend sabe quanto foi pedido, e sem isso o WAL manteria uma tabela paralela de pedidos em voo
// só para detectar escrita curta. Detectar escrita curta é obrigação de I9; facilitar é barato.
struct Completion {
  uint64_t token;
  int32_t res;
  uint32_t expected;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return res >= 0 && static_cast<uint32_t>(res) == expected;
  }
  [[nodiscard]] constexpr bool short_write() const noexcept {
    return res >= 0 && static_cast<uint32_t>(res) < expected;
  }
};

static_assert(sizeof(Completion) == 16);
static_assert(std::is_trivially_copyable_v<Completion>);

// Teto de grupos em voo (docs/wal.md: 8 por padrão). O backend dimensiona a tabela por este valor
// e recusa o excedente com `WalFull` — falha TRANSITÓRIA: o loop da partição tenta na volta
// seguinte, depois de colher. Estourar silenciosamente seria perder a ordem FIFO de I9.
inline constexpr uint32_t kMaxInflight = 32;

class IoBackend {
 public:
  IoBackend() noexcept = default;
  virtual ~IoBackend() = default;

  IoBackend(const IoBackend&) = delete;
  IoBackend& operator=(const IoBackend&) = delete;
  IoBackend(IoBackend&&) = delete;
  IoBackend& operator=(IoBackend&&) = delete;

  // Nome curto para log de fail-stop e para o relatório de teste. Saber QUAL backend gravou é a
  // primeira pergunta de qualquer investigação de durabilidade.
  [[nodiscard]] virtual const char* name() const noexcept = 0;

  // Registro de arquivos e buffers acontece UMA vez, no warm-up. Existe na interface — e não só
  // no backend de io_uring — porque é o registro que dá sentido a `file_idx` e `buf_idx`; um
  // backend que os ignorasse deixaria passar em teste o índice trocado que o io_uring rejeita.
  [[nodiscard]] virtual Status register_files(std::span<const int> fds) noexcept = 0;
  [[nodiscard]] virtual Status register_buffers(std::span<const MutBytes> bufs) noexcept = 0;

  // Enfileira (ou executa, se o backend for síncrono) uma escrita. `WalFull` quando não há
  // espaço em voo.
  [[nodiscard]] virtual Status submit(const WriteRequest& req) noexcept = 0;

  // Colhe até `out.size()` completions e devolve quantas escreveu. NÃO decide nada: erro e
  // escrita curta voltam como dados, e é o WAL quem chama `fail_stop`. Um backend que decidisse
  // parar a partição misturaria transporte com política de durabilidade — e o `FaultBackend`,
  // cuja razão de existir é justamente entregar falha, não teria como.
  [[nodiscard]] virtual uint32_t reap(std::span<Completion> out) noexcept = 0;

  // Quantos pedidos foram submetidos e ainda não voltaram por `reap`.
  [[nodiscard]] virtual uint32_t inflight() const noexcept = 0;
};

}  // namespace rv::wal
