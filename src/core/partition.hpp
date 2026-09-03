#pragma once
// O loop da partição — o motor inteiro, em dezoito linhas.
//
// Uma volta por chamada. O `while (true)` mora em `app/`, não aqui, e essa é a razão de o motor
// ser testável: o teste chama `poll` com o tempo que quiser, na ordem que quiser, e o replay
// chama com zero. É a MESMA função nos três regimes — produção, teste e recuperação.
//
// A ordem das quatro etapas não é arbitrária:
//   1. drena e aplica   — o estado anda na frente da durabilidade (estado otimista)
//   2. maybe_submit     — o ÚNICO ponto que recebe o relógio; fecha o grupo se a janela venceu
//   3. reap             — colhe completions e avança `durable_lsn` em ordem FIFO (I9)
//   4. libera o outbox  — só até `durable_lsn` (I10)
//
// Aplicar antes de ser durável é o que dá latência baixa; liberar só depois é o que dá correção.
// As duas coisas juntas são a ideia central do group commit, e elas cabem nesta ordem.

#include <cstdint>

#include "base/metrics.hpp"
#include "base/spsc_ring.hpp"
#include "core/apply.hpp"
#include "core/journal.hpp"
#include "core/outbox.hpp"
#include "core/partition_state.hpp"

namespace rv::core {

// 512 bytes: o slot do ring de ingresso. O maior evento do catálogo é `CustodyReconciled` com
// dezesseis divergências — 16 + 4 + 16×24 = 404 bytes — e é por isso que ele é FATIADO em
// dezesseis. Um slot de tamanho fixo mantém o ring livre de indireção e o ingresso livre de
// alocação; fatiar o único evento grande é mais barato que ter dois caminhos de entrada.
inline constexpr uint32_t kMaxIngressPayload = 496;

struct alignas(64) IngressFrame {
  uint64_t arrival_ts_ns;  // auditoria; `apply` só copia (D2)
  uint16_t tmpl;
  uint16_t len;
  uint32_t reserved;
  std::byte payload[kMaxIngressPayload];

  [[nodiscard]] ByteSpan bytes() const noexcept { return ByteSpan{payload, len}; }
};
static_assert(sizeof(IngressFrame) == 512);
static_assert(alignof(IngressFrame) == 64);

using Inbox = SpscRing<IngressFrame, 4096>;

template <Journal J>
class Partition {
 public:
  Partition(PartitionState& estado, J& diario, Inbox& entrada, Outbox& saida,
            Metrics& metricas) noexcept
      : estado_(estado), diario_(diario), entrada_(entrada), saida_(saida), metricas_(metricas),
        ctx_{&saida, &metricas} {}

  // Drenagem em lote: amortiza a leitura do cursor remoto do ring por 256 eventos em vez de por
  // evento. Sob carga, é a diferença entre uma transferência de linha de cache por mensagem e
  // uma por lote.
  static constexpr uint32_t kBatch = 256;

  uint32_t poll(uint64_t now_ns) noexcept {
    uint32_t aplicados = 0;

    if (!parado_) {
      while (aplicados < kBatch) {
        const IngressFrame* f = entrada_.peek();
        if (f == nullptr) break;

        // `append` ANTES de `apply`: o log é a verdade do que CHEGOU, não do que foi aceito.
        const auto a = diario_.append(f->tmpl, f->bytes(), f->arrival_ts_ns);
        if (!a) {
          // `WalFull` é contrapressão, não erro: o evento continua no ring e volta na próxima
          // volta. Nada foi consumido, nada foi aplicado — a falha é SEM EFEITO.
          ++metricas_.wal_full;
          break;
        }

        const EventView ev{a->lsn, f->arrival_ts_ns, a->payload, f->tmpl, f->len, 0};
        const Status st = apply(estado_, ev, ctx_);
        if (classify(st) == ApplyClass::Fatal) {
          parar_(st, a->lsn);
          break;
        }
        entrada_.pop();
        ++aplicados;
      }
    }

    (void)diario_.maybe_submit(now_ns);
    (void)diario_.reap();
    publicar_();
    return aplicados;
  }

  // Fecha o grupo agora, sem esperar a janela. É o que o `EodMark` usa: o snapshot precisa ser
  // exato num LSN durável, e esperar 100 µs por educação não faz sentido uma vez por dia.
  [[nodiscard]] Status force_commit(uint64_t now_ns) noexcept {
    const Status st = diario_.maybe_submit(now_ns);
    (void)diario_.reap();
    publicar_();
    return st;
  }

  [[nodiscard]] bool halted() const noexcept { return parado_ || diario_.halted(); }
  [[nodiscard]] Lsn applied_lsn() const noexcept { return estado_.applied_lsn; }
  [[nodiscard]] Status stop_reason() const noexcept { return motivo_; }
  [[nodiscard]] uint64_t published() const noexcept { return publicados_; }

 private:
  void parar_(Status st, Lsn lsn) noexcept {
    parado_ = true;
    motivo_ = st;
    lsn_da_parada_ = lsn;
    ++metricas_.apply_fatal;
    // Congelar o outbox é o que impede que uma saída produzida por um evento que corrompeu o
    // estado seja externalizada. Depois disto, nada mais sai — nem o que já estava empilhado.
    saida_.freeze();
  }

  void publicar_() noexcept {
    const Lsn duravel = diario_.durable_lsn();
    const uint32_t n = saida_.ready(duravel);
    if (n == 0) return;
    // Em produção, aqui os payloads seguem para os canais internos. O contrato é o que importa:
    // NADA sai por outro caminho, e nada sai com `lsn > durable_lsn` (I10).
    publicados_ += n;
    metricas_.outbox_released += n;
    saida_.commit(n);
  }

  PartitionState& estado_;
  J& diario_;
  Inbox& entrada_;
  Outbox& saida_;
  Metrics& metricas_;
  ApplyContext ctx_;
  Status motivo_{};
  Lsn lsn_da_parada_{};
  uint64_t publicados_ = 0;
  bool parado_ = false;
};

}  // namespace rv::core
