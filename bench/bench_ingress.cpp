// Medição do pipeline de ingress (decodificação de streaming SBE e roteamento de partições).

#include <cstring>
#include <memory>
#include <vector>

#include "bench/harness.hpp"
#include "bench/suites.hpp"
#include "codec/events.hpp"
#include "codec/template_ids.hpp"
#include "core/partition.hpp"
#include "ingress/ingress_pipeline.hpp"
#include "ingress/sbe_message_header.hpp"

namespace rv::bench {
namespace {

template <class T>
inline void nao_descarte(T&& v) noexcept {
  __asm__ __volatile__("" : : "r,m"(v) : "memory");
}

}  // namespace

void registra_ingress(Runner& r) {
  // Constrói um lote de mensagens SBE contíguas simulando um stream TCP
  constexpr uint32_t kMsgPorLote = 1000;
  std::vector<std::byte> wire;
  wire.reserve(kMsgPorLote * 128);

  for (uint32_t i = 0; i < kMsgPorLote; ++i) {
    ingress::SbeMessageHeader hdr{};
    hdr.schema_id = codec::kSchemaId;
    hdr.version = codec::kSchemaVersion;

    if (i % 3 == 0) {
      codec::TradeExecuted t{};
      t.trade_id = i + 1;
      t.account = 10000000000ULL + (i % 200);
      t.qty = 100;
      t.price = 3000;
      hdr.template_id = codec::TradeExecuted::kTemplateId;
      hdr.block_length = codec::TradeExecuted::kBlockLength;

      const size_t off = wire.size();
      wire.resize(off + sizeof(hdr) + sizeof(t));
      std::memcpy(wire.data() + off, &hdr, sizeof(hdr));
      std::memcpy(wire.data() + off + sizeof(hdr), &t, sizeof(t));
    } else if (i % 3 == 1) {
      codec::TradeAllocated a{};
      a.allocation_id = i + 1;
      a.to_account = 20000000000ULL + (i % 200);
      a.qty = 50;
      hdr.template_id = codec::TradeAllocated::kTemplateId;
      hdr.block_length = codec::TradeAllocated::kBlockLength;

      const size_t off = wire.size();
      wire.resize(off + sizeof(hdr) + sizeof(a));
      std::memcpy(wire.data() + off, &hdr, sizeof(hdr));
      std::memcpy(wire.data() + off + sizeof(hdr), &a, sizeof(a));
    } else {
      codec::BatchNetted n{};
      n.batch_id = i + 1;
      n.account = 30000000000ULL + (i % 200);
      n.net_amount = 150000;
      hdr.template_id = codec::BatchNetted::kTemplateId;
      hdr.block_length = codec::BatchNetted::kBlockLength;

      const size_t off = wire.size();
      wire.resize(off + sizeof(hdr) + sizeof(n));
      std::memcpy(wire.data() + off, &hdr, sizeof(hdr));
      std::memcpy(wire.data() + off + sizeof(hdr), &n, sizeof(n));
    }
  }

  // 1. Taxa de eventos por segundo (throughput de decode + routing)
  r.medir("ingress", "ingress.sbe_stream.eventos_por_s", Forma::Taxa, [&wire]() -> Amostra {
    constexpr size_t kNumParticoes = 4;
    std::vector<std::unique_ptr<core::Inbox>> inboxes;
    std::vector<core::Inbox*> ptrs;
    inboxes.reserve(kNumParticoes);
    ptrs.reserve(kNumParticoes);
    for (size_t i = 0; i < kNumParticoes; ++i) {
      inboxes.push_back(std::make_unique<core::Inbox>());
      ptrs.push_back(inboxes[i].get());
    }

    ingress::IngressPipeline pipeline(ptrs);
    constexpr uint32_t kIteracoes = 2000;

    const uint64_t t0 = agora_ns();
    for (uint32_t iter = 0; iter < kIteracoes; ++iter) {
      (void)pipeline.feed(ByteSpan{wire.data(), wire.size()}, 1000);

      // Drena os inboxes para simular o motor consumindo
      for (size_t i = 0; i < kNumParticoes; ++i) {
        while (inboxes[i]->peek() != nullptr) {
          inboxes[i]->pop();
        }
      }
    }
    const uint64_t t1 = agora_ns();
    return Amostra{.operacoes = kMsgPorLote * kIteracoes, .ns = t1 - t0};
  });

  // 2. Vazão de rede em MiB/s
  r.medir("ingress", "ingress.sbe_stream.vazao_mib_s", Forma::Vazao, [&wire]() -> Amostra {
    constexpr size_t kNumParticoes = 4;
    std::vector<std::unique_ptr<core::Inbox>> inboxes;
    std::vector<core::Inbox*> ptrs;
    inboxes.reserve(kNumParticoes);
    ptrs.reserve(kNumParticoes);
    for (size_t i = 0; i < kNumParticoes; ++i) {
      inboxes.push_back(std::make_unique<core::Inbox>());
      ptrs.push_back(inboxes[i].get());
    }

    ingress::IngressPipeline pipeline(ptrs);
    constexpr uint32_t kIteracoes = 2000;

    const uint64_t t0 = agora_ns();
    for (uint32_t iter = 0; iter < kIteracoes; ++iter) {
      (void)pipeline.feed(ByteSpan{wire.data(), wire.size()}, 1000);

      for (size_t i = 0; i < kNumParticoes; ++i) {
        while (inboxes[i]->peek() != nullptr) {
          inboxes[i]->pop();
        }
      }
    }
    const uint64_t t1 = agora_ns();
    return Amostra{.operacoes = wire.size() * kIteracoes, .ns = t1 - t0};
  });
}

}  // namespace rv::bench
