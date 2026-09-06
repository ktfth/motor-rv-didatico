// Primitivas de `src/base/` — o piso sobre o qual todo número do núcleo se apoia.
//
// Cada série aqui existe para conferir uma decisão registrada, não para exibir um número:
//
//   crc32c_hw × crc32c_tabela  → ADR-0022 escolheu `x86-64-v2` porque o SSE4.2 do CRC32C é a
//                                única instrução específica de que o hot path depende. Se a
//                                diferença entre os dois caminhos fosse pequena, a decisão era
//                                complexidade sem contrapartida. A medição responde.
//   spsc 1 thread × 2 threads  → ADR-0005 (thread-per-core) e a decisão de separar os cursores em
//                                linhas de cache. Medir só numa thread esconde exatamente o custo
//                                que a separação existe para evitar.
//   dense_index                → CODING_RULES §7. É a operação mais frequente do `apply`: toda
//                                internação de conta, instrumento e posição passa por aqui.
//   arredondamento             → CODING_RULES §2 (ponto fixo, nunca `double`). O custo de fazer
//                                dinheiro certo, medido em vez de suposto.
//   relogio                    → não é do motor: é do MEDIDOR. Sem este número, ninguém sabe
//                                quanto da latência publicada é o `clock_gettime` do harness.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "base/crc32c.hpp"
#include "base/dense_index.hpp"
#include "base/fixed.hpp"
#include "base/ids.hpp"
#include "base/rounding.hpp"
#include "base/spsc_ring.hpp"
#include "bench/harness.hpp"
#include "bench/suites.hpp"

namespace rv::bench {
namespace {

constexpr size_t kBufCrc = 64u << 10;  // 64 KiB: a ordem de grandeza de um grupo de commit
constexpr uint32_t kOpsMicro = 2'000'000;
constexpr uint32_t kChavesIndice = 200'000;
constexpr size_t kBytesIndice = 16u << 20;

// Impede o compilador de apagar o cálculo cujo resultado ninguém usa. Sem isto, metade destas
// medições reportaria a velocidade de um laço vazio — que é o modo mais comum de um
// microbenchmark mentir.
template <class T>
inline void nao_descarte(T&& v) noexcept {
  __asm__ __volatile__("" : : "r,m"(v) : "memory");
}

struct MensagemRing {
  uint64_t seq;
  uint64_t carga[7];
};
using RingDeMedicao = SpscRing<MensagemRing, 4096>;

}  // namespace

void registra_base(Runner& r) {
  // ---------------------------------------------------------------- relógio
  // O custo de UMA leitura de `CLOCK_MONOTONIC`. Ele entra duas vezes em cada latência medida
  // por evento, e por isso é o primeiro número do relatório: quem lê `p50 = 120 ns` precisa
  // saber se 40 deles são do medidor.
  (void)r.medir("base", "base.relogio.custo_ns", Forma::DuracaoNs, [] {
    constexpr uint32_t kN = 1'000'000;
    const uint64_t t0 = agora_ns();
    for (uint32_t i = 0; i < kN; ++i) nao_descarte(agora_ns());
    return Amostra{kN, agora_ns() - t0};
  });

  // ---------------------------------------------------------------- CRC32C
  auto buf = std::make_unique<unsigned char[]>(kBufCrc);
  for (size_t i = 0; i < kBufCrc; ++i) buf[i] = static_cast<unsigned char>(i * 31U + 7U);

  if (crc32c_uses_hardware()) {
    (void)r.medir("base", "base.crc32c_hw.vazao", Forma::Vazao, [&buf] {
      constexpr uint32_t kVoltas = 512;
      const uint64_t t0 = agora_ns();
      uint32_t c = 0;
      for (uint32_t i = 0; i < kVoltas; ++i) c = crc32c_hw(c, buf.get(), kBufCrc);
      const uint64_t dt = agora_ns() - t0;
      nao_descarte(c);
      return Amostra{static_cast<uint64_t>(kVoltas) * kBufCrc, dt};
    });
  } else {
    (void)r.pula("base", "base.crc32c_hw.vazao", Forma::Vazao,
                 "esta CPU não tem SSE4.2: o caminho de hardware não existe aqui");
  }

  (void)r.medir("base", "base.crc32c_tabela.vazao", Forma::Vazao, [&buf] {
    constexpr uint32_t kVoltas = 128;
    const uint64_t t0 = agora_ns();
    uint32_t c = 0;
    for (uint32_t i = 0; i < kVoltas; ++i) c = crc32c_table(c, buf.get(), kBufCrc);
    const uint64_t dt = agora_ns() - t0;
    nao_descarte(c);
    return Amostra{static_cast<uint64_t>(kVoltas) * kBufCrc, dt};
  });

  // ---------------------------------------------------------------- SPSC ring
  auto ring = std::make_unique<RingDeMedicao>();
  (void)r.medir("base", "base.spsc_1thread.mensagens_por_s", Forma::Taxa, [&ring] {
    constexpr uint32_t kN = 4'000'000;
    const uint64_t t0 = agora_ns();
    uint64_t recebidas = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      MensagemRing* m = ring->claim();
      if (m == nullptr) break;
      m->seq = i;
      ring->publish();
      const MensagemRing* p = ring->peek();
      if (p != nullptr) {
        nao_descarte(p->seq);
        ring->pop();
        ++recebidas;
      }
    }
    return Amostra{recebidas, agora_ns() - t0};
  });

  // Duas threads é o número que interessa: é a topologia real (ingress num core, partição em
  // outro) e é onde a separação dos cursores em linhas de cache paga ou não paga.
  auto ring2 = std::make_unique<RingDeMedicao>();
  (void)r.medir("base", "base.spsc_2threads.mensagens_por_s", Forma::Taxa, [&ring2] {
    constexpr uint64_t kN = 2'000'000;
    std::atomic<bool> comecou{false};
    const uint64_t t0 = agora_ns();
    std::thread produtor([&ring2, &comecou] {
      comecou.store(true, std::memory_order_release);
      for (uint64_t i = 0; i < kN; ++i) {
        MensagemRing* m = nullptr;
        while ((m = ring2->claim()) == nullptr) {
        }
        m->seq = i;
        ring2->publish();
      }
    });
    uint64_t recebidas = 0;
    while (recebidas < kN) {
      const MensagemRing* p = ring2->peek();
      if (p == nullptr) continue;
      nao_descarte(p->seq);
      ring2->pop();
      ++recebidas;
    }
    produtor.join();
    return Amostra{recebidas, agora_ns() - t0};
  });

  // ---------------------------------------------------------------- índice denso
  // Carga de 0,7 — o máximo que a tabela aceita. Medir com a tabela vazia mediria a sondagem no
  // melhor caso, que é o caso que a produção não tem.
  auto mem_idx = std::make_unique<std::byte[]>(kBytesIndice);

  (void)r.medir("base", "base.dense_index.insercoes_por_s", Forma::Taxa, [&mem_idx] {
    Arena a{mem_idx.get(), kBytesIndice};
    DenseIndex idx;
    if (!idx.init(a, kChavesIndice)) return Amostra{0, 1};
    const uint64_t t0 = agora_ns();
    uint32_t n = 0;
    for (uint32_t i = 0; i < kChavesIndice; ++i) {
      bool inserido = false;
      nao_descarte(idx.insert_or_get(mix64(i + 1), i, inserido));
      n += inserido ? 1U : 0U;
    }
    return Amostra{n, agora_ns() - t0};
  });

  (void)r.medir("base", "base.dense_index.buscas_por_s", Forma::Taxa, [&mem_idx] {
    Arena a{mem_idx.get(), kBytesIndice};
    DenseIndex idx;
    if (!idx.init(a, kChavesIndice)) return Amostra{0, 1};
    for (uint32_t i = 0; i < kChavesIndice; ++i) {
      bool inserido = false;
      (void)idx.insert_or_get(mix64(i + 1), i, inserido);
    }
    const uint64_t t0 = agora_ns();
    for (uint32_t i = 0; i < kOpsMicro; ++i) {
      nao_descarte(idx.find(mix64((i % kChavesIndice) + 1)));
    }
    return Amostra{kOpsMicro, agora_ns() - t0};
  });

  // ---------------------------------------------------------------- ponto fixo
  // O quarteto que o `apply` de um negócio executa: financeiro, corretagem, custo de posição e
  // preço médio. É o preço de CODING_RULES §2, medido em vez de suposto.
  (void)r.medir("base", "base.arredondamento.negocios_por_s", Forma::Taxa, [] {
    constexpr uint32_t kN = 1'000'000;
    const uint64_t t0 = agora_ns();
    for (uint32_t i = 0; i < kN; ++i) {
      const Qty q = Qty::from_raw(static_cast<int64_t>(100 + (i % 900)) * Qty::kOne);
      const Price p = Price::from_raw(static_cast<int64_t>(1'234'567'89) + i);
      const Money bruto = notional_half_even(q, p, 1);
      const Money taxa = fee_half_up(bruto, 500);
      const Money custo = position_cost_half_even(q, p);
      const Price medio = average_price_half_even(q, p, q, custo);
      nao_descarte(bruto.raw() + taxa.raw() + custo.raw() + medio.raw());
    }
    return Amostra{kN, agora_ns() - t0};
  });
}

}  // namespace rv::bench
