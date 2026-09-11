// Testes do IngressPipeline (TCP stream framing, SBE decodificação e roteamento).
//
// Valida:
//   - Decodificação de SbeMessageHeader
//   - Reconstituição de stream TCP fragmentado
//   - Despacho em lote (múltiplas mensagens no mesmo buffer)
//   - Roteamento por hash de documento (Partitioner)
//   - Broadcast de eventos globais (DayOpened, ClosingPriceSet, etc.)
//   - Contrapressão quando o Inbox atinge saturação (WouldBlock)
//   - Rejeição de mensagens malformadas ou com schemaId divergente

#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "codec/events.hpp"
#include "codec/template_ids.hpp"
#include "ingress/ingress_pipeline.hpp"
#include "ingress/sbe_message_header.hpp"

namespace rv::ingress {
namespace {

class IngressPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (size_t i = 0; i < kNumPartitions; ++i) {
      inboxes_.push_back(std::make_unique<core::Inbox>());
      inbox_ptrs_.push_back(inboxes_[i].get());
    }
    pipeline_ = std::make_unique<IngressPipeline>(inbox_ptrs_);
  }

  static constexpr size_t kNumPartitions = 4;
  std::vector<std::unique_ptr<core::Inbox>> inboxes_;
  std::vector<core::Inbox*> inbox_ptrs_;
  std::unique_ptr<IngressPipeline> pipeline_;
};

TEST_F(IngressPipelineTest, MensagemUnicaRoteadaCorretamente) {
  codec::TradeExecuted trade{};
  trade.trade_id = 1001;
  trade.account = 11122233344ULL;  // Documento fixado
  trade.qty = 100;
  trade.price = 2550;
  trade.instrument = 10;

  SbeMessageHeader hdr{};
  hdr.block_length = codec::TradeExecuted::kBlockLength;
  hdr.template_id = codec::TradeExecuted::kTemplateId;
  hdr.schema_id = codec::kSchemaId;
  hdr.version = codec::kSchemaVersion;

  std::vector<std::byte> wire(sizeof(hdr) + sizeof(trade));
  std::memcpy(wire.data(), &hdr, sizeof(hdr));
  std::memcpy(wire.data() + sizeof(hdr), &trade, sizeof(trade));

  constexpr uint64_t kTs = 1700000000123ULL;
  auto st = pipeline_->feed(ByteSpan{wire.data(), wire.size()}, kTs);
  EXPECT_TRUE(st.is_ok());

  // Partição esperada para o documento 11122233344ULL com 4 partições:
  Partitioner part(kNumPartitions);
  const PartitionId p = part.of(DocumentId{trade.account});

  // O inbox da partição destino deve ter exatamente 1 mensagem
  const core::IngressFrame* f = inboxes_[p.v]->peek();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->arrival_ts_ns, kTs);
  EXPECT_EQ(f->tmpl, codec::TradeExecuted::kTemplateId);
  EXPECT_EQ(f->len, codec::TradeExecuted::kBlockLength);

  codec::TradeExecuted recebido{};
  std::memcpy(&recebido, f->payload, sizeof(recebido));
  EXPECT_EQ(recebido.trade_id, trade.trade_id);
  EXPECT_EQ(recebido.account, trade.account);
  EXPECT_EQ(recebido.price, trade.price);

  inboxes_[p.v]->pop();
  EXPECT_EQ(inboxes_[p.v]->peek(), nullptr);

  // As outras partições devem estar vazias
  for (size_t i = 0; i < kNumPartitions; ++i) {
    if (i != p.v) {
      EXPECT_EQ(inboxes_[i]->peek(), nullptr);
    }
  }

  EXPECT_EQ(pipeline_->stats().events_received, 1u);
  EXPECT_EQ(pipeline_->stats().events_routed, 1u);
  EXPECT_EQ(pipeline_->stats().events_broadcast, 0u);
}

TEST_F(IngressPipelineTest, FragmentacaoStreamTcpReconstituida) {
  codec::TradeAllocated alloc{};
  alloc.allocation_id = 999;
  alloc.to_account = 22233344455ULL;
  alloc.qty = 50;

  SbeMessageHeader hdr{};
  hdr.block_length = codec::TradeAllocated::kBlockLength;
  hdr.template_id = codec::TradeAllocated::kTemplateId;
  hdr.schema_id = codec::kSchemaId;
  hdr.version = codec::kSchemaVersion;

  std::vector<std::byte> wire(sizeof(hdr) + sizeof(alloc));
  std::memcpy(wire.data(), &hdr, sizeof(hdr));
  std::memcpy(wire.data() + sizeof(hdr), &alloc, sizeof(alloc));

  // Divide o buffer em 3 pedaços fragmentados
  ByteSpan chunk1{wire.data(), 5};                      // 5 bytes do header
  ByteSpan chunk2{wire.data() + 5, 10};                 // resto do header + 7 bytes payload
  ByteSpan chunk3{wire.data() + 15, wire.size() - 15};  // restante do payload

  EXPECT_TRUE(pipeline_->feed(chunk1, 100).is_ok());
  EXPECT_EQ(pipeline_->pending_bytes(), 5u);
  EXPECT_EQ(pipeline_->stats().events_received, 0u);

  EXPECT_TRUE(pipeline_->feed(chunk2, 101).is_ok());
  EXPECT_EQ(pipeline_->pending_bytes(), 15u);
  EXPECT_EQ(pipeline_->stats().events_received, 0u);

  // O último pedaço completa o quadro
  EXPECT_TRUE(pipeline_->feed(chunk3, 102).is_ok());
  EXPECT_EQ(pipeline_->pending_bytes(), 0u);
  EXPECT_EQ(pipeline_->stats().events_received, 1u);

  Partitioner part(kNumPartitions);
  const PartitionId p = part.of(DocumentId{alloc.to_account});
  const core::IngressFrame* f = inboxes_[p.v]->peek();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->tmpl, codec::TradeAllocated::kTemplateId);
  inboxes_[p.v]->pop();
}

TEST_F(IngressPipelineTest, MultiplasMensagensNoMesmoBuffer) {
  std::vector<std::byte> batch;

  for (uint64_t i = 0; i < 5; ++i) {
    codec::BatchNetted netted{};
    netted.batch_id = i + 1;
    netted.account = 10000000000ULL + i;
    netted.net_amount = 50000;

    SbeMessageHeader hdr{};
    hdr.block_length = codec::BatchNetted::kBlockLength;
    hdr.template_id = codec::BatchNetted::kTemplateId;
    hdr.schema_id = codec::kSchemaId;
    hdr.version = codec::kSchemaVersion;

    const size_t offset = batch.size();
    batch.resize(offset + sizeof(hdr) + sizeof(netted));
    std::memcpy(batch.data() + offset, &hdr, sizeof(hdr));
    std::memcpy(batch.data() + offset + sizeof(hdr), &netted, sizeof(netted));
  }

  EXPECT_TRUE(pipeline_->feed(ByteSpan{batch.data(), batch.size()}, 200).is_ok());
  EXPECT_EQ(pipeline_->stats().events_received, 5u);
  EXPECT_EQ(pipeline_->stats().events_routed, 5u);
  EXPECT_EQ(pipeline_->pending_bytes(), 0u);
}

TEST_F(IngressPipelineTest, BroadcastEnviaParaTodasAsParticoes) {
  codec::DayOpened abertura{};
  abertura.business_date = 20260902;
  abertura.settle_d1 = 20260903;
  abertura.settle_d2 = 20260904;

  SbeMessageHeader hdr{};
  hdr.block_length = codec::DayOpened::kBlockLength;
  hdr.template_id = codec::DayOpened::kTemplateId;
  hdr.schema_id = codec::kSchemaId;
  hdr.version = codec::kSchemaVersion;

  std::vector<std::byte> wire(sizeof(hdr) + sizeof(abertura));
  std::memcpy(wire.data(), &hdr, sizeof(hdr));
  std::memcpy(wire.data() + sizeof(hdr), &abertura, sizeof(abertura));

  EXPECT_TRUE(pipeline_->feed(ByteSpan{wire.data(), wire.size()}, 300).is_ok());
  EXPECT_EQ(pipeline_->stats().events_broadcast, 1u);

  // Cada partição deve ter recebido exatamente 1 quadro
  for (size_t i = 0; i < kNumPartitions; ++i) {
    const core::IngressFrame* f = inboxes_[i]->peek();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->tmpl, codec::DayOpened::kTemplateId);
    inboxes_[i]->pop();
    EXPECT_EQ(inboxes_[i]->peek(), nullptr);
  }
}

TEST_F(IngressPipelineTest, ContrapressaoQuandoRingSatura) {
  codec::TradeExecuted trade{};
  trade.account = 11122233344ULL;
  Partitioner part(kNumPartitions);
  const PartitionId p = part.of(DocumentId{trade.account});

  // Enche o Inbox da partição p até a capacidade máxima
  for (size_t i = 0; i < core::Inbox::kCapacity; ++i) {
    core::IngressFrame* slot = inboxes_[p.v]->claim();
    ASSERT_NE(slot, nullptr);
    slot->tmpl = 1;
    inboxes_[p.v]->publish();
  }

  // Tenta empurrar mais um evento para essa partição cheia
  SbeMessageHeader hdr{};
  hdr.block_length = codec::TradeExecuted::kBlockLength;
  hdr.template_id = codec::TradeExecuted::kTemplateId;
  hdr.schema_id = codec::kSchemaId;
  hdr.version = codec::kSchemaVersion;

  std::vector<std::byte> wire(sizeof(hdr) + sizeof(trade));
  std::memcpy(wire.data(), &hdr, sizeof(hdr));
  std::memcpy(wire.data() + sizeof(hdr), &trade, sizeof(trade));

  auto st = pipeline_->feed(ByteSpan{wire.data(), wire.size()}, 400);
  EXPECT_FALSE(st.is_ok());
  EXPECT_EQ(st.code(), Err::WouldBlock);
  EXPECT_EQ(pipeline_->stats().backpressure_drops, 1u);
}

TEST_F(IngressPipelineTest, RejeitaSchemaInvalidoOuCorrompido) {
  SbeMessageHeader hdr{};
  hdr.block_length = 32;
  hdr.template_id = codec::TradeExecuted::kTemplateId;
  hdr.schema_id = 999;  // SchemaId inválido (esperado: 1)
  hdr.version = codec::kSchemaVersion;

  std::vector<std::byte> wire(sizeof(hdr) + 32, std::byte{0});
  std::memcpy(wire.data(), &hdr, sizeof(hdr));

  auto st = pipeline_->feed(ByteSpan{wire.data(), wire.size()}, 500);
  EXPECT_FALSE(st.is_ok());
  EXPECT_EQ(pipeline_->stats().parse_errors, 1u);
}

}  // namespace
}  // namespace rv::ingress
