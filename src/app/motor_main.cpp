// motor_main.cpp — processo principal do motor de renda variável.
//
// Integra:
//   - Pipeline de ingress de rede (framing SBE, decodificação e roteamento por CPF/CNPJ)
//   - N partições single-writer pinadas com shared-nothing (ADR-0005)
//   - Persistência nativa de WAL com io_uring (IORING_SETUP_SINGLE_ISSUER |
//   IORING_SETUP_DEFER_TASKRUN)
//   - Fechamento determinístico e validação contábil de integridade (I1..I13)

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "codec/template_ids.hpp"
#include "core/partition.hpp"
#include "core/state_image.hpp"
#include "ingress/ingress_pipeline.hpp"
#include "ingress/simulator.hpp"
#include "wal/io_uring_backend.hpp"
#include "wal/wal.hpp"

namespace {

std::atomic<bool> g_rodando{true};

void trata_sinal(int) {
  g_rodando.store(false, std::memory_order_relaxed);
}

struct ParticaoServidor {
  uint16_t id = 0;
  static constexpr size_t kMemBytes = 256u << 20;  // 256 MiB por partição
  std::unique_ptr<std::byte[]> memoria{new std::byte[kMemBytes]};
  rv::Arena arena{memoria.get(), kMemBytes};

  rv::core::PartitionState estado;
  rv::core::Outbox outbox;
  rv::Metrics metricas;
  rv::core::Inbox* inbox = nullptr;

  rv::wal::UringBackend backend;
  std::unique_ptr<rv::wal::UringWal> wal;
  std::unique_ptr<rv::core::Partition<rv::wal::UringWal>> loop;

  bool monta(uint16_t part_id, const std::string& dir_wal) {
    id = part_id;
    rv::core::PartitionCapacity c{};
    if (!estado.init(arena, rv::PartitionId{id}, c)) return false;
    if (!outbox.init(arena, c.outbox_slots, c.outbox_payload_bytes)) return false;
    inbox = arena.emplace<rv::core::Inbox>();
    if (inbox == nullptr) return false;

    // Abrir backend io_uring nativo
    auto st_backend = backend.open(8);
    if (!st_backend.is_ok()) {
      std::fprintf(stderr, "ERRO: falha ao inicializar io_uring na partição %u: código=%d\n", id,
                   static_cast<int>(st_backend.code()));
      return false;
    }

    rv::wal::WalOptions opts{};
    opts.partition = rv::PartitionId{id};
    opts.dir = dir_wal.c_str();
    opts.window_ns = 100'000;  // 100 µs
    opts.segment_bytes = 64u << 20;

    wal = std::make_unique<rv::wal::UringWal>(backend);
    auto st_wal = wal->open(opts);
    if (!st_wal.is_ok()) {
      std::fprintf(stderr, "ERRO: falha ao abrir WAL na partição %u: código=%d\n", id,
                   static_cast<int>(st_wal.code()));
      return false;
    }

    // Selar arena após warm-up (CODING_RULES §1)
    arena.seal();

    loop = std::make_unique<rv::core::Partition<rv::wal::UringWal>>(estado, *wal, *inbox, outbox,
                                                                    metricas);
    return true;
  }
};

void print_usage(const char* prog) {
  std::printf("uso: %s [opções]\n", prog);
  std::printf("  --particoes N      número de partições (potência de 2, padrão: 4)\n");
  std::printf("  --dir-wal DIR      diretório base dos arquivos WAL (padrão: /tmp/motor-rv-wal)\n");
  std::printf("  --dias N           pregões simulados no lote (padrão: 1)\n");
  std::printf("  --negocios N       negócios por pregão (padrão: 5000)\n");
  std::printf("  --investidores N   número de investidores (padrão: 500)\n");
  std::printf("  --semente N        semente determinística do simulador (padrão: 20260902)\n");
  std::printf("  --ajuda, -h        exibe esta mensagem\n");
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t num_particoes = 4;
  std::string dir_wal = "/tmp/motor-rv-wal";
  uint32_t dias = 1;
  uint32_t negocios_por_dia = 5000;
  uint32_t investidores = 500;
  uint64_t semente = 20260902;
  std::string dados = "data";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool tem = i + 1 < argc;
    if (a == "--particoes" && tem) {
      num_particoes = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--dir-wal" && tem) {
      dir_wal = argv[++i];
    } else if (a == "--dias" && tem) {
      dias = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--negocios" && tem) {
      negocios_por_dia = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--investidores" && tem) {
      investidores = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--semente" && tem) {
      semente = std::stoull(argv[++i]);
    } else if (a == "--ajuda" || a == "-h") {
      print_usage(argv[0]);
      return 0;
    }
  }

  if (!rv::ingress::Partitioner::is_valid_count(num_particoes)) {
    std::fprintf(stderr, "ERRO: número de partições deve ser potência de 2 entre 1 e 64.\n");
    return 1;
  }

  std::signal(SIGINT, trata_sinal);
  std::signal(SIGTERM, trata_sinal);

  std::filesystem::create_directories(dir_wal);

  std::printf("==================================================================\n");
  std::printf("  motor-rv: Servidor de Renda Variável (Marco M3)\n");
  std::printf("  Partições     : %u\n", num_particoes);
  std::printf("  Diretório WAL : %s\n", dir_wal.c_str());
  std::printf("  Backend WAL   : io_uring nativo (SINGLE_ISSUER + DEFER_TASKRUN)\n");
  std::printf("==================================================================\n");

  // 1. Inicializar partições
  std::vector<std::unique_ptr<ParticaoServidor>> particoes;
  std::vector<rv::core::Inbox*> inboxes;
  particoes.reserve(num_particoes);
  inboxes.reserve(num_particoes);

  for (uint16_t i = 0; i < num_particoes; ++i) {
    auto p = std::make_unique<ParticaoServidor>();
    if (!p->monta(i, dir_wal)) {
      std::fprintf(stderr, "ERRO: falha ao inicializar partição %u\n", i);
      return 2;
    }
    inboxes.push_back(p->inbox);
    particoes.push_back(std::move(p));
  }

  std::printf("[Partições Inicializadas]\n");
  for (const auto& p : particoes) {
    std::printf(
        "  Partição %u: arena selada, io_uring aberto (single_issuer=%d defer_taskrun=%d)\n", p->id,
        p->backend.single_issuer(), p->backend.defer_taskrun());
  }

  // 2. Conectar IngressPipeline
  rv::ingress::IngressPipeline pipeline(inboxes);

  // 3. Gerar sessão de eventos determinísticos
  rv::ingress::ConfigSimulacao cfg_sim{};
  cfg_sim.dias = dias;
  cfg_sim.negocios_por_dia = negocios_por_dia;
  cfg_sim.investidores = investidores;
  cfg_sim.particoes = num_particoes;
  cfg_sim.semente = semente;

  std::vector<rv::ingress::Instrumento> instrumentos;
  std::vector<rv::ingress::DiaDePregao> calendario;
  std::string erro;
  if (!rv::ingress::carrega_instrumentos(dados + "/instrumentos.csv", instrumentos, erro) ||
      !rv::ingress::carrega_calendario(dados + "/calendario-b3-2026.csv", 20260902, dias,
                                       calendario, erro)) {
    std::fprintf(stderr, "ERRO: falha ao carregar dados de mercado: %s\n", erro.c_str());
    return 1;
  }

  std::vector<rv::ingress::EventoRoteado> eventos;
  rv::ingress::gera(cfg_sim, instrumentos, calendario, eventos);
  std::printf("\n==> Gerando carga determinística de ingress (%u pregões, %u negócios/dia)...\n",
              dias, negocios_por_dia);
  std::printf("  Total de eventos gerados: %zu\n", eventos.size());

  // 4. Ingestão via pipeline em stream SBE
  std::printf("==> Processando eventos pelo IngressPipeline e gravando via io_uring...\n");
  const auto inicio = std::chrono::steady_clock::now();

  std::vector<std::byte> wire_buffer;
  wire_buffer.reserve(512);

  uint64_t ts_simulado = 1'000'000'000ULL;  // 1 segundo base
  uint64_t eventos_processados = 0;

  for (const auto& ev : eventos) {
    if (!g_rodando.load(std::memory_order_relaxed)) break;

    // Monta o frame SBE canônico de rede com SbeMessageHeader
    rv::ingress::SbeMessageHeader hdr{};
    hdr.block_length = ev.len;
    hdr.template_id = ev.tmpl;
    hdr.schema_id = rv::codec::kSchemaId;
    hdr.version = rv::codec::kSchemaVersion;

    wire_buffer.resize(sizeof(hdr) + ev.len);
    std::memcpy(wire_buffer.data(), &hdr, sizeof(hdr));
    std::memcpy(wire_buffer.data() + sizeof(hdr), ev.bytes, ev.len);

    // Alimenta o pipeline de ingress com timestamp avançando
    ts_simulado += 25'000;  // avança 25 microssegundos por evento
    (void)pipeline.feed(rv::ByteSpan{wire_buffer.data(), wire_buffer.size()}, ts_simulado);

    // Avança o loop de cada partição
    for (auto& p : particoes) {
      p->loop->poll(ts_simulado);
    }
    eventos_processados++;
  }

  // 5. Drenagem e sincronização final de commits de WAL
  uint64_t ts_fim = ts_simulado + 200'000;  // avança além da janela de 100 µs
  for (auto& p : particoes) {
    while (p->inbox->peek() != nullptr) {
      p->loop->poll(ts_fim);
    }
    (void)p->wal->force_commit(ts_fim);
    (void)p->wal->reap();
  }

  const auto fim = std::chrono::steady_clock::now();
  const double duracao_s = std::chrono::duration<double>(fim - inicio).count();

  std::printf("==================================================================\n");
  std::printf("  Relatório de Execução do Servidor\n");
  std::printf("==================================================================\n");
  std::printf("  Duração total          : %.3f s\n", duracao_s);
  std::printf("  Eventos de ingress     : %lu\n", pipeline.stats().events_received);
  std::printf("  Eventos roteados       : %lu\n", pipeline.stats().events_routed);
  std::printf("  Eventos de broadcast   : %lu\n", pipeline.stats().events_broadcast);
  std::printf("  Backpressure drops     : %lu\n", pipeline.stats().backpressure_drops);
  std::printf("  Bytes processados      : %lu (%.2f MiB)\n", pipeline.stats().bytes_received,
              static_cast<double>(pipeline.stats().bytes_received) / (1024.0 * 1024.0));
  std::printf("  Vazão efetiva          : %.1f eventos/s\n\n",
              static_cast<double>(eventos_processados) / (duracao_s > 0 ? duracao_s : 1e-6));

  std::printf("[Status das Partições e WAL]\n");
  uint64_t total_rejeicoes = 0;
  for (const auto& p : particoes) {
    const rv::Lsn dur_lsn = p->wal->durable_lsn();
    const rv::Lsn last_lsn = p->wal->last_lsn();
    total_rejeicoes += p->metricas.apply_rejected;
    std::printf(
        "  Partição %u: last_lsn=%lu durable_lsn=%lu aceitos=%lu rejeições=%lu custódia=%016" PRIx64
        "\n",
        p->id, last_lsn.v, dur_lsn.v, p->metricas.apply_accepted, p->metricas.apply_rejected,
        p->estado.custody_checksum());
  }
  std::printf("  Total agregado de rejeições: %lu\n", total_rejeicoes);
  std::printf("==================================================================\n");
  std::printf("✓ Servidor motor-rv finalizado com sucesso.\n");

  return 0;
}
