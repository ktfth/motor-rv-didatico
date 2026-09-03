// Testes da camada base. Vários deles existiam como programa solto em /tmp durante a construção e
// nunca entraram na suíte — que é o mesmo que não existirem: um teste que ninguém roda não protege
// nada. Uma revisão independente apontou exatamente isso para a equivalência do CRC32C.

#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <vector>

#include "base/arena.hpp"
#include "base/crc32c.hpp"
#include "base/dense_index.hpp"
#include "base/metrics.hpp"
#include "base/pair_index.hpp"
#include "base/spsc_ring.hpp"

using namespace rv;

namespace {
alignas(64) std::byte g_bloco[4u << 20];
}

// ---------------------------------------------------------------- CRC32C
TEST(Crc32c, VetoresCanonicos) {
  struct Caso { const char* s; uint32_t esperado; };
  const Caso casos[] = {
      {"", 0x00000000U},
      {"a", 0xC1D04330U},
      {"123456789", 0xE3069283U},
      {"The quick brown fox jumps over the lazy dog", 0x22620404U},
  };
  for (const auto& c : casos) {
    const size_t n = std::strlen(c.s);
    EXPECT_EQ(crc32c_table(0, c.s, n), c.esperado) << "tabela: " << c.s;
    EXPECT_EQ(crc32c_hw(0, c.s, n), c.esperado) << "hardware: " << c.s;
  }
}

TEST(Crc32c, OsDoisCaminhosConcordamEmTodoAlinhamentoETamanho) {
  // O caminho de hardware consome de oito em oito depois de alinhar byte a byte. É exatamente aí
  // que ele costuma divergir da tabela — e é por isso que o teste varre offset E tamanho.
  std::vector<unsigned char> buf(4096);
  for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<unsigned char>(i * 31 + 7);
  for (int off = 0; off < 9; ++off) {
    for (size_t len = 0; len < 200; ++len) {
      ASSERT_EQ(crc32c_table(0, buf.data() + off, len), crc32c_hw(0, buf.data() + off, len))
          << "off=" << off << " len=" << len;
    }
  }
}

TEST(Crc32c, ContinuacaoEquivaleAoCalculoDeUmaVez) {
  const char* s = "motor-rv: o CRC de um registro cobre cabeçalho e payload em duas chamadas";
  const size_t n = std::strlen(s);
  for (size_t corte = 0; corte <= n; corte += 7) {
    const uint32_t emduas = crc32c(crc32c(0, s, corte), s + corte, n - corte);
    EXPECT_EQ(emduas, crc32c(0, s, n)) << "corte=" << corte;
  }
}

// ---------------------------------------------------------------- Arena
TEST(Arena, SelarBloqueiaTodaAlocacaoPosterior) {
  Arena a{g_bloco, sizeof g_bloco};
  ASSERT_NE(a.alloc_array<uint64_t>(100), nullptr);
  ASSERT_NE(a.alloc_array<uint32_t>(50), nullptr);
  EXPECT_EQ(a.blocks(), 2u);
  a.seal();
  EXPECT_EQ(a.alloc_array<uint64_t>(1), nullptr) << "CODING_RULES §1 como mecanismo, não como frase";
  EXPECT_TRUE(a.sealed());
}

TEST(Arena, PedidoQueNaoCabeMarcaTransbordoENaoCorrompe) {
  alignas(64) std::byte pequeno[256];
  Arena a{pequeno, sizeof pequeno};
  EXPECT_NE(a.alloc_array<uint64_t>(8), nullptr);
  EXPECT_EQ(a.alloc_array<uint64_t>(1000), nullptr);
  EXPECT_TRUE(a.overflowed()) << "o dimensionamento errado tem de ser detectável no warm-up";
  EXPECT_LE(a.used(), a.capacity());
}

// ---------------------------------------------------------------- DenseIndex
TEST(DenseIndex, CapacidadeUtilEhOQueFoiPedido) {
  // A primeira versão desta API dimensionava a TABELA e deixava a divisão pelo fator de carga
  // para quem chamava: pedir 1000 e conseguir guardar 716 era erro silencioso.
  Arena a{g_bloco, sizeof g_bloco};
  DenseIndex idx;
  ASSERT_TRUE(idx.init(a, 1000));
  EXPECT_GE(idx.capacity(), 1000u);
  for (uint32_t i = 0; i < 1000; ++i) {
    bool novo = false;
    EXPECT_EQ(idx.insert_or_get(i * 2654435761ULL, i, novo), i);
    EXPECT_TRUE(novo) << "i=" << i;
  }
  for (uint32_t i = 0; i < 1000; ++i) EXPECT_EQ(idx.find(i * 2654435761ULL), i);
  bool novo = false;
  EXPECT_EQ(idx.insert_or_get(0, 999999, novo), 0u) << "reinserção devolve o existente";
  EXPECT_FALSE(novo);
}

// ---------------------------------------------------------------- PairIndex
TEST(PairIndex, ChaveDeParNaoColide) {
  // O motivo de existir: (action_id, conta) tem 96 bits. Misturar em 64 criaria chance de pular
  // um evento corporativo de um investidor, e "chance pequena de errar dinheiro" não é aceitável.
  Arena a{g_bloco, sizeof g_bloco};
  PairIndex idx;
  ASSERT_TRUE(idx.init(a, 4096));
  bool cheio = false;
  EXPECT_TRUE(idx.insert(900, 1, cheio));
  EXPECT_FALSE(idx.insert(900, 1, cheio)) << "mesma chave: já aplicado";
  EXPECT_TRUE(idx.insert(900, 2, cheio)) << "mesmo evento, OUTRA conta";
  EXPECT_TRUE(idx.insert(901, 1, cheio)) << "outro evento, MESMA conta";
  EXPECT_TRUE(idx.contains(900, 1) && idx.contains(900, 2) && idx.contains(901, 1));
  EXPECT_FALSE(idx.contains(901, 2));
}

TEST(TwoGenSet, RotacaoPorDataEsqueceOQueSaiuDaJanela) {
  Arena a{g_bloco, sizeof g_bloco};
  TwoGenSet s;
  ASSERT_TRUE(s.init(a, 1024));
  bool cheio = false;
  s.maybe_rotate(DateYmd::from_ymd(2026, 1, 1), 60);
  EXPECT_TRUE(s.insert(1, 1, cheio));
  EXPECT_FALSE(s.insert(1, 1, cheio));

  s.maybe_rotate(DateYmd::from_ymd(2026, 2, 15), 60);  // 45 dias: ainda não rotaciona
  EXPECT_FALSE(s.insert(1, 1, cheio)) << "dentro da janela, continua lembrando";

  s.maybe_rotate(DateYmd::from_ymd(2026, 3, 10), 60);  // 68 dias: rotaciona
  s.maybe_rotate(DateYmd::from_ymd(2026, 5, 20), 60);  // rotaciona de novo: a geração antiga cai
  EXPECT_TRUE(s.insert(1, 1, cheio))
      << "fora da janela, reaplica — comportamento ACEITO e documentado em golden/13";
}

// ---------------------------------------------------------------- Histogram
TEST(Histogram, QuantisSobreDistribuicaoConhecida) {
  Histogram h;
  for (uint64_t v = 1; v <= 10000; ++v) h.record(v);
  EXPECT_EQ(h.count(), 10000u);
  EXPECT_EQ(h.max(), 10000u);
  // Erro relativo do balde log-linear: ~1,5 % por oitava.
  auto perto = [](uint64_t obtido, uint64_t esperado) {
    const double e = static_cast<double>(esperado);
    return static_cast<double>(obtido) > e * 0.97 && static_cast<double>(obtido) < e * 1.03;
  };
  EXPECT_TRUE(perto(h.quantile(0.50), 5000)) << h.quantile(0.50);
  EXPECT_TRUE(perto(h.quantile(0.99), 9900)) << h.quantile(0.99);
  EXPECT_TRUE(perto(h.quantile(0.999), 9990)) << h.quantile(0.999);
}

TEST(Histogram, ValoresPequenosTemResolucaoDeUmaUnidade) {
  // A latência de append vive nos valores pequenos: abaixo de 64 o índice É o valor.
  Histogram h;
  for (uint64_t v = 0; v < 64; ++v) h.record(v);
  EXPECT_EQ(h.quantile(0.0), 0u);
  EXPECT_EQ(h.quantile(0.5), 32u);
  h.reset();
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.quantile(0.5), 0u);
}

// ---------------------------------------------------------------- SPSC ring
namespace {
struct Msg {
  uint64_t seq;
  uint64_t carga;
};
SpscRing<Msg, 4096> g_ring;
}  // namespace

TEST(SpscRing, DoisFiosDeVerdadeSemPerdaNemReordenacao) {
  // Rótulo `concorrencia`: é este teste que o preset tsan seleciona. Antes da correção, o filtro
  // apontava para um rótulo que nenhum CMakeLists declarava — o tsan rodava ZERO testes e saía 0,
  // deixando o único ponto de atomics do projeto sem verificação nenhuma.
  constexpr uint64_t kN = 500'000;
  std::thread produtor([] {
    for (uint64_t i = 0; i < kN; ++i) {
      Msg* s = nullptr;
      while ((s = g_ring.claim()) == nullptr) {
      }
      s->seq = i;
      s->carga = i * 1099511628211ULL;
      g_ring.publish();
    }
  });

  uint64_t recebidos = 0, erros = 0;
  while (recebidos < kN) {
    const Msg* m = nullptr;
    while ((m = g_ring.peek()) == nullptr) {
    }
    if (m->seq != recebidos || m->carga != recebidos * 1099511628211ULL) ++erros;
    g_ring.pop();
    ++recebidos;
  }
  produtor.join();
  EXPECT_EQ(recebidos, kN);
  EXPECT_EQ(erros, 0u) << "SPSC não pode perder nem reordenar";
  EXPECT_EQ(g_ring.produced(), g_ring.consumed());
}
