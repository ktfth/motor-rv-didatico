// A imagem de recuperação (stall-and-copy, ADR-0014): gravar, carregar, e quanto ela ocupa.
//
// Por que estas três séries e não uma: `docs/wal.md` mira 20–40 ms de stall para 200 MB, e a
// revisão de 03/09 mostrou que a primeira versão gravava 126 MiB tanto com 10 negócios quanto com
// 20 mil — porque a imagem era proporcional à CAPACIDADE CONFIGURADA e não aos dados. O conserto
// existe; medi-lo é o que impede a regressão de voltar sem ninguém notar. Por isso `snapshot.bytes`
// é uma série publicada, com direção "menor é melhor", e não uma nota de rodapé.
//
// A duração da GRAVAÇÃO é o stall: enquanto ela roda, a partição não aplica evento. É o número que
// decide se o fechamento diário cabe na janela.
//
// A duração da CARGA é o outro lado — recuperação — e inclui a reconstrução dos quatro índices
// densos, que a imagem deliberadamente NÃO grava (uma tabela de hash é cache de uma relação que já
// está nos dados). Medir a carga é o que dá o direito de continuar não gravando os índices.

#include <cstdint>
#include <memory>
#include <vector>

#include "base/arena.hpp"
#include "bench/bench_journal.hpp"
#include "bench/harness.hpp"
#include "bench/nucleo_bench.hpp"
#include "bench/suites.hpp"
#include "core/apply.hpp"
#include "core/state_image.hpp"

namespace rv::bench {
namespace {

constexpr size_t kBufJournal = 64u << 20;

// Aplica a sessão inteira e devolve o estado carregado. Fora de qualquer cronômetro: o que se
// mede aqui é o snapshot, não a construção do estado.
[[nodiscard]] bool prepara_estado(Nucleo& n, const Carga& carga, std::vector<std::byte>& buf,
                                  const core::PartitionCapacity& cap) {
  if (!n.monta(cap)) return false;
  BenchJournal diario{buf.data(), buf.size()};
  for (const auto& ev : carga.eventos) {
    if (!n.outbox().has_room(4, 256)) {
      const uint32_t prontos = n.outbox().ready(diario.last_lsn());
      if (prontos > 0) n.outbox().commit(prontos);
    }
    const auto a = diario.append(ev.tmpl, ByteSpan{ev.bytes, ev.len}, 0);
    if (!a) continue;
    const core::EventView view{a->lsn, 0, a->payload, ev.tmpl, ev.len, 0};
    if (core::classify(core::apply(n.estado(), view, n.ctx())) == core::ApplyClass::Fatal) {
      return false;
    }
  }
  return true;
}

}  // namespace

void registra_snapshot(Runner& r, const Carga& carga) {
  auto nucleo = std::make_shared<Nucleo>(kArenaDeMedicao);
  auto buf_journal = std::make_shared<std::vector<std::byte>>(kBufJournal);
  const core::PartitionCapacity cap_larga = capacidade_de_medicao();

  if (!prepara_estado(*nucleo, carga, *buf_journal, cap_larga)) {
    (void)r.pula("snapshot", "snapshot.salva.duracao_ms", Forma::DuracaoMs,
                 "não consegui construir o estado de origem");
    return;
  }

  const uint64_t bytes_imagem = core::state_image_bytes(nucleo->estado());
  auto imagem = std::make_shared<std::vector<std::byte>>(bytes_imagem + 4096);
  // Uma arena separada para a restauração: `load_state_image` exige arena virgem, e alocá-la
  // dentro do cronômetro mediria o `new` do processo em vez do formato.
  auto mem_destino = std::make_shared<std::vector<std::byte>>(kArenaDeMedicao);

  (void)r.medir("snapshot", "snapshot.bytes", Forma::Tamanho,
                [bytes_imagem] { return Amostra{bytes_imagem, 1}; });
  r.anota(
      "proporcional aos DADOS, não à capacidade configurada — é o conserto da revisão de "
      "03/09, e esta série é o que impede que ele volte atrás sem ninguém notar");

  (void)r.medir("snapshot", "snapshot.salva.duracao_ms", Forma::DuracaoMs, [&] {
    uint64_t escritos = 0;
    const uint64_t t0 = agora_ns();
    const Status st = core::save_state_image(nucleo->estado(), MutBytes{*imagem}, &escritos);
    const uint64_t dt = agora_ns() - t0;
    return st.is_ok() ? Amostra{1, dt} : Amostra{0, dt};
  });
  r.anota("é o STALL do stall-and-copy: enquanto ela roda, a partição não aplica evento");

  (void)r.medir("snapshot", "snapshot.salva.vazao", Forma::Vazao, [&] {
    uint64_t escritos = 0;
    const uint64_t t0 = agora_ns();
    const Status st = core::save_state_image(nucleo->estado(), MutBytes{*imagem}, &escritos);
    const uint64_t dt = agora_ns() - t0;
    return st.is_ok() ? Amostra{escritos, dt} : Amostra{0, dt};
  });

  // Grava uma vez, fora do cronômetro, para as séries de carga terem o que ler.
  uint64_t escritos_larga = 0;
  if (core::save_state_image(nucleo->estado(), MutBytes{*imagem}, &escritos_larga).is_error()) {
    (void)r.pula("snapshot", "snapshot.carrega.duracao_ms", Forma::DuracaoMs,
                 "a gravação da imagem falhou");
    return;
  }
  const ByteSpan imagem_larga{imagem->data(), escritos_larga};

  // ------------------------------------------------------------------ o experimento da capacidade
  // A imagem já é proporcional aos dados. A RESTAURAÇÃO, não: `load_state_image` começa chamando
  // `PartitionState::init`, que aloca e zera as colunas inteiras — 1 M linhas de negócio, 256 K de
  // posição — porque é o que a capacidade configurada manda, e não o que a imagem contém.
  //
  // É a mesma classe de defeito que a revisão de 03/09 corrigiu do lado da ESCRITA, viva do lado
  // da LEITURA: o custo depende de quanto alguém dimensionou, não de quanto o dia teve. Medir os
  // dois lados com os MESMOS dados e capacidades diferentes é o que transforma essa suspeita em
  // número — e o número é a diferença entre as duas séries abaixo.
  const auto folga = [](uint32_t n, uint32_t minimo) {
    const uint64_t com_folga = static_cast<uint64_t>(n) + n / 4 + 64;
    uint32_t p = minimo;
    while (p < com_folga) p <<= 1U;
    return p;
  };
  core::PartitionCapacity cap_justa{};
  cap_justa.accounts = folga(nucleo->estado().cash.count, 64);
  cap_justa.positions = folga(nucleo->estado().custody.count, 64);
  cap_justa.instruments = folga(nucleo->estado().instruments.count, 64);
  cap_justa.trades = folga(nucleo->estado().trades.count, 64);
  cap_justa.corporate_actions_per_gen = 1u << 12;
  cap_justa.exceptions = 1u << 12;
  cap_justa.outbox_slots = cap_larga.outbox_slots;
  cap_justa.outbox_payload_bytes = cap_larga.outbox_payload_bytes;

  auto imagem2 = std::make_shared<std::vector<std::byte>>(imagem->size());
  uint64_t escritos_justa = 0;
  const bool justa_ok =
      prepara_estado(*nucleo, carga, *buf_journal, cap_justa) &&
      core::save_state_image(nucleo->estado(), MutBytes{*imagem2}, &escritos_justa).is_ok();

  (void)r.medir("snapshot", "snapshot.carrega.duracao_ms", Forma::DuracaoMs, [&] {
    // Estado e arena virgens por repetição. A construção do objeto fica fora do cronômetro;
    // dentro dele está só o que a recuperação de verdade faz: `init`, ler as seções, conferir
    // CRC e reconstruir os quatro índices densos.
    auto destino = std::make_unique<core::PartitionState>();
    Arena arena{mem_destino->data(), mem_destino->size()};
    const uint64_t t0 = agora_ns();
    const Status st = core::load_state_image(*destino, arena, imagem_larga);
    const uint64_t dt = agora_ns() - t0;
    return st.is_ok() ? Amostra{1, dt} : Amostra{0, dt};
  });
  r.anota("capacidade de medição (1 M negócios, 256 K posições configurados)");

  if (!justa_ok) {
    (void)r.pula("snapshot", "snapshot.carrega_capacidade_justa.duracao_ms", Forma::DuracaoMs,
                 "não consegui reconstruir o estado com a capacidade ajustada ao dado");
    return;
  }
  const ByteSpan imagem_justa{imagem2->data(), escritos_justa};

  (void)r.medir("snapshot", "snapshot.carrega_capacidade_justa.duracao_ms", Forma::DuracaoMs, [&] {
    auto destino = std::make_unique<core::PartitionState>();
    Arena arena{mem_destino->data(), mem_destino->size()};
    const uint64_t t0 = agora_ns();
    const Status st = core::load_state_image(*destino, arena, imagem_justa);
    const uint64_t dt = agora_ns() - t0;
    return st.is_ok() ? Amostra{1, dt} : Amostra{0, dt};
  });
  r.anota(
      "MESMOS dados, capacidade dimensionada ao que o dia teve. A diferença para a série "
      "anterior é o custo que a recuperação paga por configuração, e não por dado");

  (void)r.medir("snapshot", "snapshot.bytes_capacidade_justa", Forma::Tamanho,
                [escritos_justa] { return Amostra{escritos_justa, 1}; });
  r.anota("comparar com snapshot.bytes: iguais confirmam que a IMAGEM já é proporcional ao dado");
}

}  // namespace rv::bench
