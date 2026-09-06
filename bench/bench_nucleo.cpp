// O núcleo: um pregão inteiro passando pelo motor, cronometrado.
//
// ---------------------------------------------------------------------------------------------
// TRÊS SÉRIES, E POR QUE NÃO UMA
//
//   nucleo.loop.eventos_por_s_por_core  — o motor COMPLETO: SPSC ring, `append`, `apply`, portão
//       de saída de I10. É a métrica obrigatória de `bench/baseline.json`, e é a única que mede
//       o que o processo de produção realmente executa.
//
//   nucleo.apply.eventos_por_s          — as mesmas quatro etapas, mas escritas AQUI, sem o ring.
//       Serve para separar o custo do transporte do custo do domínio: a diferença entre esta e a
//       de cima é o que o SPSC ring cobra por evento.
//
//   nucleo.apply.latencia_ns            — o mesmo laço de cima, com o relógio lido a cada evento.
//       É a única forma de ter P50/P99/P999 por evento sem instrumentar `Partition::poll` — e
//       instrumentar o `poll` seria medir um binário que a produção não roda. O preço é o custo
//       do próprio relógio, publicado em `base.relogio.custo_ns` e visível como a diferença de
//       vazão entre esta série e a anterior.
//
// O laço "espelhado" reproduz as quatro etapas de `core/partition.hpp` na ordem que aquele
// arquivo documenta: contrapressão do outbox, `append`, `apply`, publicação até `durable_lsn`.
// Ele é uma CÓPIA, e cópia diverge — por isso a série do loop de verdade fica ao lado, e uma
// diferença que não seja explicável pelo ring é sinal de que a cópia envelheceu.

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "base/metrics.hpp"
#include "bench/bench_journal.hpp"
#include "bench/contrato.hpp"
#include "bench/harness.hpp"
#include "bench/nucleo_bench.hpp"
#include "bench/suites.hpp"
#include "core/apply.hpp"
#include "core/partition.hpp"

namespace rv::bench {
namespace {

constexpr size_t kBufJournal = 64u << 20;

// Quantos appends `durable_lsn` fica atrás de `last_lsn`. Não é enfeite: com atraso zero o outbox
// libera uma entrada por evento e nunca acumula, que é um regime que o group commit não entrega.
// 4096 é a ordem de grandeza de um grupo de 64 KiB com eventos de ~100 bytes.
constexpr uint64_t kAtrasoDurabilidade = 4096;

struct Espelho {
  uint64_t aplicados = 0;
  uint64_t recusados_por_espaco = 0;
};

// As quatro etapas de `Partition::poll`, sem o ring. `hist`, quando presente, recebe a latência
// de UM evento — do `append` até a publicação.
Espelho roda_espelho(Nucleo& n, const Carga& carga, BenchJournal& diario, Histogram* hist) {
  Espelho e;
  core::Outbox& saida = n.outbox();
  for (const auto& ev : carga.eventos) {
    const uint64_t t0 = (hist != nullptr) ? agora_ns() : 0;

    if (!saida.has_room(core::Partition<BenchJournal>::kMaxOutputsPerEvent,
                        core::Partition<BenchJournal>::kMaxOutputBytesPerEvent)) {
      // Contrapressão: no motor de verdade o loop para e volta na próxima volta. Aqui a saída é
      // drenada na hora, porque não há consumidor externo para esperar.
      const uint32_t livres = saida.ready(diario.last_lsn());
      if (livres > 0) saida.commit(livres);
      if (!saida.has_room(core::Partition<BenchJournal>::kMaxOutputsPerEvent,
                          core::Partition<BenchJournal>::kMaxOutputBytesPerEvent)) {
        ++e.recusados_por_espaco;
        continue;
      }
    }

    const auto a = diario.append(ev.tmpl, ByteSpan{ev.bytes, ev.len}, 0);
    if (!a) continue;

    const core::EventView view{a->lsn, 0, a->payload, ev.tmpl, ev.len, 0};
    const Status st = core::apply(n.estado(), view, n.ctx());
    if (core::classify(st) == core::ApplyClass::Fatal) break;

    (void)diario.reap();
    const uint32_t prontos = saida.ready(diario.durable_lsn());
    if (prontos > 0) saida.commit(prontos);

    ++e.aplicados;
    if (hist != nullptr) hist->record(agora_ns() - t0);
  }
  return e;
}

}  // namespace

void registra_nucleo(Runner& r, const Carga& carga) {
  auto nucleo = std::make_shared<Nucleo>(kArenaDeMedicao);
  auto buf = std::make_shared<std::vector<std::byte>>(kBufJournal);
  const core::PartitionCapacity cap = capacidade_de_medicao();

  if (!nucleo->monta(cap)) {
    (void)r.pula("nucleo", kSerieNucleoLoop, Forma::Taxa,
                 "a partição não coube na arena de medição");
    return;
  }

  // ------------------------------------------------------------ o motor completo
  // O nome vem de bench/contrato.hpp: é ele que o comparador procura no baseline. Literal aqui
  // seria a quarta cópia da mesma verdade — e a que envelhece primeiro, porque é a que se lê ao
  // renomear a série.
  (void)r.medir("nucleo", kSerieNucleoLoop, Forma::Taxa, [&] {
    if (!nucleo->monta(cap)) return Amostra{0, 1};
    BenchJournal diario{buf->data(), buf->size()};
    diario.set_atraso(kAtrasoDurabilidade);
    core::Partition<BenchJournal> loop{nucleo->estado(), diario, nucleo->entrada(),
                                       nucleo->outbox(), nucleo->metricas()};
    core::Inbox& entrada = nucleo->entrada();

    uint64_t entregues = 0;
    uint64_t agora = 0;
    const uint64_t t0 = agora_ns();
    for (const auto& ev : carga.eventos) {
      core::IngressFrame* f = nullptr;
      uint32_t tentativas = 0;
      while ((f = entrada.claim()) == nullptr) {
        (void)loop.poll(agora += 1000);
        if (++tentativas > 1000) break;  // fail-stop ou saturação: para de alimentar
      }
      if (f == nullptr) break;
      f->arrival_ts_ns = agora;
      f->tmpl = ev.tmpl;
      f->len = ev.len;
      std::memcpy(f->payload, ev.bytes, ev.len);
      entrada.publish();
      ++entregues;
    }
    while (loop.poll(agora += 1000) > 0) {
    }
    const uint64_t dt = agora_ns() - t0;
    return Amostra{entregues, dt};
  });

  // ------------------------------------------------------------ o domínio, sem o ring
  (void)r.medir("nucleo", "nucleo.apply.eventos_por_s", Forma::Taxa, [&] {
    if (!nucleo->monta(cap)) return Amostra{0, 1};
    BenchJournal diario{buf->data(), buf->size()};
    diario.set_atraso(kAtrasoDurabilidade);
    const uint64_t t0 = agora_ns();
    const Espelho e = roda_espelho(*nucleo, carga, diario, nullptr);
    return Amostra{e.aplicados, agora_ns() - t0};
  });

  // ------------------------------------------------------------ a distribuição
  (void)r.medir_latencia("nucleo", "nucleo.apply.latencia_ns", Forma::DuracaoNs, [&](Histogram& h) {
    if (!nucleo->monta(cap)) return Amostra{0, 1};
    BenchJournal diario{buf->data(), buf->size()};
    diario.set_atraso(kAtrasoDurabilidade);
    const uint64_t t0 = agora_ns();
    const Espelho e = roda_espelho(*nucleo, carga, diario, &h);
    return Amostra{e.aplicados, agora_ns() - t0};
  });

  // ------------------------------------------------------------ o portão de I10
  // `stage` + `ready` + `commit`: o ciclo que TODA saída do núcleo atravessa. Se ele custar caro,
  // custa caro uma vez por evento aceito — e é o único ponto por onde a saída passa, então não há
  // caminho alternativo para amortizá-lo.
  (void)r.medir("nucleo", "nucleo.outbox.ciclos_por_s", Forma::Taxa, [&] {
    if (!nucleo->monta(cap)) return Amostra{0, 1};
    core::Outbox& saida = nucleo->outbox();
    constexpr uint32_t kN = 1'000'000;
    const core::PositionUpdate carga_saida{};
    const ByteSpan bytes{reinterpret_cast<const std::byte*>(&carga_saida), sizeof carga_saida};
    const uint64_t t0 = agora_ns();
    uint64_t n = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      const Lsn lsn{i + 1};
      if (saida.stage(lsn, core::OutKind::ReadModelUpdate, bytes).is_error()) break;
      const uint32_t prontos = saida.ready(lsn);
      if (prontos > 0) saida.commit(prontos);
      ++n;
    }
    return Amostra{n, agora_ns() - t0};
  });
}

}  // namespace rv::bench
