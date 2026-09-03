// motor-rv-sim — roda pregões inteiros pelo motor e mostra o que aconteceu.
//
// É a ferramenta que responde "o motor funciona?" sem precisar ler um teste. Ela monta N
// partições, gera uma sessão determinística com o simulador de ingestão e alimenta cada partição
// pelo seu SPSC ring, exatamente como o processo de produção faria — mesmo loop, mesma ordem,
// mesmo `apply`.
//
// O que ela NÃO faz: medir desempenho. Números de latência e vazão saem do harness de bench
// (ADR-0021), com aquecimento, descarte de série e o bloco de ambiente preenchido. Um número
// impresso por uma ferramenta de demonstração seria comparado com outro amanhã, e a comparação
// não valeria nada.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "core/partition.hpp"
#include "core/state_image.hpp"
#include "core/testing/memory_journal.hpp"
#include "ingress/simulator.hpp"

using namespace rv;

namespace {

struct Nucleo {
  static constexpr size_t kBytes = 256u << 20;
  std::unique_ptr<std::byte[]> memoria{new std::byte[kBytes]};
  Arena arena{memoria.get(), kBytes};
  core::PartitionState estado;
  core::Outbox outbox;
  Metrics metricas;
  core::testing::MemoryJournal diario{64u << 20};
  core::Inbox* entrada = nullptr;
  std::unique_ptr<core::Partition<core::testing::MemoryJournal>> loop;

  bool monta(uint16_t id) {
    core::PartitionCapacity c{};
    if (!estado.init(arena, PartitionId{id}, c)) return false;
    if (!outbox.init(arena, c.outbox_slots, c.outbox_payload_bytes)) return false;
    entrada = arena.emplace<core::Inbox>();
    if (entrada == nullptr) return false;
    // Fim do warm-up. A partir daqui, uma alocação é erro — é o `seal()` que transforma
    // CODING_RULES §1 de regra escrita em regra aplicada.
    arena.seal();
    loop = std::make_unique<core::Partition<core::testing::MemoryJournal>>(estado, diario, *entrada,
                                                                          outbox, metricas);
    return true;
  }
};

void uso() {
  (void)std::puts(
      "uso: motor-rv-sim [--dias N] [--negocios N] [--investidores N] [--particoes N]\n"
      "                  [--semente N] [--data AAAAMMDD] [--dados DIR]\n"
      "\n"
      "Gera uma sessão determinística e a passa pelo motor. Mesma semente, mesmo resultado.");
}

}  // namespace

int main(int argc, char** argv) {
  ingress::ConfigSimulacao cfg{};
  uint32_t data_inicial = 20260902;
  std::string dados = "data";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool tem = i + 1 < argc;
    if (a == "--dias" && tem) cfg.dias = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (a == "--negocios" && tem) cfg.negocios_por_dia = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (a == "--investidores" && tem) cfg.investidores = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (a == "--particoes" && tem) cfg.particoes = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (a == "--semente" && tem) cfg.semente = std::stoull(argv[++i]);
    else if (a == "--data" && tem) data_inicial = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (a == "--dados" && tem) dados = argv[++i];
    else { uso(); return a == "--help" || a == "-h" ? 0 : 2; }
  }
  // Toda opção numérica é validada. `--investidores 0` chegava a `Lcg::below(0)`, que faz
  // `next() % 0`: SIGFPE, sem mensagem. Um binário de demonstração que morre com sinal em vez de
  // dizer o que está errado ensina a coisa errada sobre o projeto.
  if (!ingress::Partitioner::is_valid_count(cfg.particoes)) {
    (void)std::fprintf(stderr, "--particoes tem de ser potência de dois entre 1 e 65536 (recebi %u)\n",
                 cfg.particoes);
    return 2;
  }
  struct Limite { const char* nome; uint32_t valor; uint32_t minimo; };
  for (const Limite& l : {Limite{"--dias", cfg.dias, 1}, Limite{"--negocios", cfg.negocios_por_dia, 1},
                          Limite{"--investidores", cfg.investidores, 1}}) {
    if (l.valor < l.minimo) {
      (void)std::fprintf(stderr, "%s tem de ser >= %u (recebi %u)\n", l.nome, l.minimo, l.valor);
      return 2;
    }
  }
  if (cfg.um_em_n_invalidos == 0 || cfg.um_em_n_falhas == 0) return 2;

  std::vector<ingress::Instrumento> instrumentos;
  std::vector<ingress::DiaDePregao> calendario;
  std::string erro;
  if (!ingress::carrega_instrumentos(dados + "/instrumentos.csv", instrumentos, erro) ||
      !ingress::carrega_calendario(dados + "/calendario-b3-2026.csv", data_inicial, cfg.dias,
                                   calendario, erro)) {
    (void)std::fprintf(stderr, "motor-rv-sim: %s\n", erro.c_str());
    return 1;
  }

  (void)std::printf("== simulação ==\n");
  (void)std::printf("  semente      : %" PRIu64 "\n", cfg.semente);
  (void)std::printf("  instrumentos : %zu (de %s/instrumentos.csv)\n", instrumentos.size(), dados.c_str());
  (void)std::printf("  pregões      : ");
  for (const auto& d : calendario) std::printf("%u→D+2=%u  ", d.data, d.liquidacao_d2);
  (void)std::printf("\n  investidores : %u em %u partições\n", cfg.investidores, cfg.particoes);

  std::vector<ingress::EventoRoteado> eventos;
  ingress::gera(cfg, instrumentos, calendario, eventos);
  (void)std::printf("  eventos      : %zu\n\n", eventos.size());

  std::vector<std::unique_ptr<Nucleo>> nucleos;
  for (uint32_t k = 0; k < cfg.particoes; ++k) {
    auto n = std::make_unique<Nucleo>();
    if (!n->monta(static_cast<uint16_t>(k))) {
      (void)std::fprintf(stderr, "partição %u não coube na arena\n", k);
      return 1;
    }
    nucleos.push_back(std::move(n));
  }

  // Alimenta e drena intercalado: o ring tem 4096 slots e a sessão tem centenas de milhares de
  // eventos. É também o que o processo de produção faz — o ingress produz, a partição consome.
  uint64_t agora = 0;
  for (const auto& e : eventos) {
    Nucleo& n = *nucleos[e.particao.v];
    core::IngressFrame* f = nullptr;
    while ((f = n.entrada->claim()) == nullptr) n.loop->poll(agora += 1000);
    f->arrival_ts_ns = agora;
    f->tmpl = e.tmpl;
    f->len = e.len;
    std::memcpy(f->payload, e.bytes, e.len);
    n.entrada->publish();
  }
  for (auto& n : nucleos) {
    while (n->loop->poll(agora += 1000) > 0) {
    }
  }

  (void)std::printf("%-4s %10s %10s %9s %9s %9s %9s %12s\n", "part", "aceitos", "rejeitados", "contas",
              "posições", "papéis", "negócios", "publicados");
  uint64_t tot_ok = 0, tot_rej = 0;
  for (uint32_t k = 0; k < cfg.particoes; ++k) {
    const Nucleo& n = *nucleos[k];
    (void)std::printf("%-4u %10" PRIu64 " %10" PRIu64 " %9u %9u %9u %9u %12" PRIu64 "\n", k,
                n.metricas.apply_accepted, n.metricas.apply_rejected, n.estado.cash.count,
                n.estado.custody.count, n.estado.instruments.count, n.estado.trades.count,
                n.loop->published());
    tot_ok += n.metricas.apply_accepted;
    tot_rej += n.metricas.apply_rejected;
  }
  (void)std::printf("%-4s %10" PRIu64 " %10" PRIu64 "\n\n", "tot", tot_ok, tot_rej);

  // Por que os rejeitados importam tanto quanto os aceitos: o log é a verdade do que CHEGOU. Uma
  // simulação sem rejeição não exercitaria o caminho que mais quebra no replay.
  (void)std::printf("== rejeições por motivo ==\n");
  uint64_t conferencia = 0;
  for (uint32_t c = 0; c < Metrics::kMaxErrCode; ++c) {
    uint64_t soma = 0;
    for (const auto& n : nucleos) soma += n->metricas.rejected_by_code[c];
    if (soma != 0) {
      (void)std::printf("  %-30s %10" PRIu64 "\n", to_string(static_cast<Err>(c)), soma);
      conferencia += soma;
    }
  }
  // A soma da tabela TEM de bater com o total. Se não bater, algum código escapou do intervalo
  // contabilizado e o relatório está mentindo por omissão.
  (void)std::printf("  %-30s %10" PRIu64 "%s\n", "(soma da tabela)", conferencia,
              conferencia == tot_rej ? "" : "   <-- NAO BATE COM O TOTAL");

  (void)std::printf("\n== invariantes, conferidos agora ==\n");
  uint64_t violacoes = 0;
  for (const auto& n : nucleos) {
    for (uint32_t i = 0; i < n->estado.custody.count; ++i) {
      if (!n->estado.custody.buckets_non_negative(i)) ++violacoes;
    }
  }
  (void)std::printf("  I3  (bucket não negativo)  : %s\n", violacoes == 0 ? "ok" : "VIOLADO");
  uint64_t soma_custodia = 0, soma_caixa = 0;
  for (uint32_t k = 0; k < cfg.particoes; ++k) {
    const uint64_t c = nucleos[k]->estado.custody_checksum();
    const uint64_t x = nucleos[k]->estado.cash_checksum();
    soma_custodia += c;
    soma_caixa += x;
    (void)std::printf("  partição %u  custódia=%016" PRIx64 "  caixa=%016" PRIx64 "  LSN=%" PRIu64 "\n", k,
                c, x, nucleos[k]->estado.applied_lsn.v);
  }
  // A SOMA atravessa as partições, e é o número mais forte que esta ferramenta imprime.
  //
  // Cada checksum é uma soma sobre as posições de uma partição; somando todas, obtém-se a soma
  // sobre TODAS as posições do sistema — um conjunto que não depende de em quantos cores ele foi
  // dividido. Logo o total tem de ser o mesmo com 1, 2, 4 ou 8 partições. Se mudar, ou o
  // roteamento por documento deixou de ser função pura, ou algum evento foi aplicado no core
  // errado. É um gate de CI de uma linha para a promessa central de ADR-0005.
  (void)std::printf("  TOTAL       custódia=%016" PRIx64 "  caixa=%016" PRIx64
              "   (não deve depender de --particoes)\n",
              soma_custodia, soma_caixa);

  (void)std::printf("\n== imagem de recuperação (stall-and-copy) ==\n");
  for (uint32_t k = 0; k < cfg.particoes; ++k) {
    const auto bytes = core::state_image_bytes(nucleos[k]->estado.arena_bytes);
    (void)std::printf("  partição %u: %.1f MiB de estado\n", k,
                static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  return violacoes == 0 ? 0 : 1;
}
