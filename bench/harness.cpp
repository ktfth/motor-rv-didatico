#include "bench/harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

#include <fcntl.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wal/block_align.hpp"

namespace rv::bench {
namespace {

// Roda um programa e devolve o que ele imprimiu. Uma vez, na coleta de ambiente — nunca dentro
// de medição.
//
// Sem `popen`: ele passa a string por `/bin/sh`, e `cert-env33-c` — que o `.clang-tidy` deste
// projeto trata como ERRO, não aviso — recusa qualquer processador de comandos. Aqui não há
// entrada externa nenhuma, então o risco concreto é zero; obedecer mesmo assim custa esta função
// e evita a discussão "neste caso não tem problema", que é como a regra morre.
//
// `posix_spawnp` executa o binário diretamente: sem shell, sem expansão, sem citação. O `argv` é
// um vetor, e vetor não tem injeção.
[[nodiscard]] std::string executa(const char* const argv[]) {
  int tubo[2] = {-1, -1};
  if (::pipe(tubo) != 0) return {};

  posix_spawn_file_actions_t acoes;
  if (::posix_spawn_file_actions_init(&acoes) != 0) {
    (void)::close(tubo[0]);
    (void)::close(tubo[1]);
    return {};
  }
  // stdout do filho vai para o tubo; stderr vai para /dev/null, porque "fatal: not a git
  // repository" na saída do harness assusta sem informar — a ausência do valor já é a resposta.
  (void)::posix_spawn_file_actions_adddup2(&acoes, tubo[1], STDOUT_FILENO);
  (void)::posix_spawn_file_actions_addopen(&acoes, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  (void)::posix_spawn_file_actions_addclose(&acoes, tubo[0]);

  pid_t pid = -1;
  const int rc =
      ::posix_spawnp(&pid, argv[0], &acoes, nullptr, const_cast<char* const*>(argv), ::environ);
  (void)::posix_spawn_file_actions_destroy(&acoes);
  (void)::close(tubo[1]);
  if (rc != 0) {
    (void)::close(tubo[0]);
    return {};
  }

  std::string out;
  std::array<char, 256> pedaco{};
  ssize_t n = 0;
  while ((n = ::read(tubo[0], pedaco.data(), pedaco.size())) > 0) {
    out.append(pedaco.data(), static_cast<size_t>(n));
  }
  (void)::close(tubo[0]);
  int estado = 0;
  (void)::waitpid(pid, &estado, 0);
  if (!WIFEXITED(estado) || WEXITSTATUS(estado) != 0) return {};
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

[[nodiscard]] std::string primeira_linha_com(const char* arquivo, const char* prefixo) {
  std::ifstream f(arquivo);
  std::string linha;
  const std::string pre = prefixo;
  while (std::getline(f, linha)) {
    if (linha.starts_with(pre)) {
      const auto dois = linha.find(':');
      if (dois == std::string::npos) return linha;
      std::string v = linha.substr(dois + 1);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      return v;
    }
  }
  return {};
}

// O filesystem que hospeda um caminho, lido de /proc/mounts pelo prefixo mais longo. É a
// informação que falta em toda medição de WAL publicada sem ela: 200 µs em btrfs e 200 µs em
// tmpfs são números que não têm nada em comum.
[[nodiscard]] std::string fs_de(const std::string& caminho) {
  std::ifstream f("/proc/mounts");
  std::string linha;
  std::string melhor_ponto;
  std::string melhor_tipo;
  std::string melhor_dev;
  while (std::getline(f, linha)) {
    std::istringstream ss(linha);
    std::string dev;
    std::string ponto;
    std::string tipo;
    if (!(ss >> dev >> ponto >> tipo)) continue;
    if (caminho.starts_with(ponto) && ponto.size() >= melhor_ponto.size()) {
      melhor_ponto = ponto;
      melhor_tipo = tipo;
      melhor_dev = dev;
    }
  }
  if (melhor_tipo.empty()) return "desconhecido";
  return melhor_dev + " (" + melhor_tipo + " em " + melhor_ponto + ")";
}

}  // namespace

uint64_t agora_ns() noexcept {
  timespec ts{};
  (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

const char* unidade_de(Forma f) noexcept {
  switch (f) {
    case Forma::Taxa:
      return "ops/s";
    case Forma::Vazao:
      return "MiB/s";
    case Forma::DuracaoNs:
      return "ns";
    case Forma::DuracaoUs:
      return "us";
    case Forma::DuracaoMs:
      return "ms";
    case Forma::Tamanho:
      return "bytes";
  }
  return "?";
}

// ---------------------------------------------------------------------------- estatística
double mediana_de(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double media_de(const std::vector<double>& v) noexcept {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (const double x : v) s += x;
  return s / static_cast<double>(v.size());
}

// Coeficiente de variação amostral, em porcento. É o número que decide descartar a série: ele
// compara dispersão com magnitude, e por isso serve igual para 3 M eventos/s e para 40 µs.
double cv_pct_de(const std::vector<double>& v) noexcept {
  if (v.size() < 2) return 0.0;
  const double m = media_de(v);
  if (m == 0.0) return 0.0;
  double soma = 0.0;
  for (const double x : v) soma += (x - m) * (x - m);
  const double dp = std::sqrt(soma / static_cast<double>(v.size() - 1));
  return 100.0 * dp / std::abs(m);
}

namespace {

// A conta que transforma `Amostra` em valor publicável. Uma só, para que a unidade impressa e o
// número nunca possam divergir.
[[nodiscard]] double valor_de(Forma f, const Amostra& a) noexcept {
  const double ops = static_cast<double>(a.operacoes);
  const double ns = static_cast<double>(a.ns);
  switch (f) {
    case Forma::Taxa:
      return ns > 0.0 ? ops * 1'000'000'000.0 / ns : 0.0;
    case Forma::Vazao:
      return ns > 0.0 ? ops * 1'000'000'000.0 / ns / (1024.0 * 1024.0) : 0.0;
    case Forma::DuracaoNs:
      return ops > 0.0 ? ns / ops : 0.0;
    case Forma::DuracaoUs:
      return ops > 0.0 ? ns / ops / 1'000.0 : 0.0;
    case Forma::DuracaoMs:
      return ops > 0.0 ? ns / ops / 1'000'000.0 : 0.0;
    case Forma::Tamanho:
      return ops;
  }
  return 0.0;
}

}  // namespace

// ---------------------------------------------------------------------------- Runner
Serie Runner::roda_(const char* grupo, const char* nome, Forma forma,
                    const std::function<Amostra(Histogram*)>& corpo, Histogram* hist) {
  Serie s;
  s.grupo = grupo;
  s.nome = nome;
  s.forma = forma;
  s.unidade = unidade_de(forma);
  s.direcao = direcao_de(forma);
  s.medida = true;

  for (uint32_t tentativa = 0; tentativa < cfg_.tentativas; ++tentativa) {
    // Aquecimento: roda e joga fora. O que ele paga é primeiro toque de página da arena, cache
    // frio e, no caso do WAL, alocação de extents do filesystem — custos que o motor de produção
    // paga uma vez no warm-up e que não pertencem a nenhuma métrica de regime.
    for (uint32_t i = 0; i < cfg_.aquecimento; ++i) (void)corpo(hist);
    if (hist != nullptr) hist->reset();  // depois do aquecimento: os quantis são só do regime

    s.amostras.clear();
    s.amostras.reserve(cfg_.repeticoes);
    uint64_t ops = 0;
    for (uint32_t i = 0; i < cfg_.repeticoes; ++i) {
      const Amostra a = corpo(hist);
      ops = a.operacoes;
      s.amostras.push_back(valor_de(forma, a));
    }
    s.operacoes_por_repeticao = ops;
    s.cv_pct = cv_pct_de(s.amostras);
    if (s.cv_pct <= cfg_.limiar_cv_pct) {
      s.estavel = true;
      break;
    }
    ++s.series_descartadas;
    if (cfg_.verboso) {
      (void)std::fprintf(stderr, "  [%s] série descartada: CV %.1f%% > %.1f%% (tentativa %u/%u)\n",
                         nome, s.cv_pct, cfg_.limiar_cv_pct, tentativa + 1, cfg_.tentativas);
    }
  }

  s.mediana = mediana_de(s.amostras);
  s.media = media_de(s.amostras);
  if (!s.amostras.empty()) {
    s.minimo = *std::min_element(s.amostras.begin(), s.amostras.end());
    s.maximo = *std::max_element(s.amostras.begin(), s.amostras.end());
  }
  if (!s.estavel) {
    s.nota =
        "série instável: o CV ficou acima do limiar em todas as tentativas; "
        "a métrica NÃO vira baseline";
  }
  if (hist != nullptr && hist->count() > 0) {
    s.tem_quantis = true;
    s.p50 = hist->quantile(0.50);
    s.p99 = hist->quantile(0.99);
    s.p999 = hist->quantile(0.999);
    s.pmax = hist->max();
  }
  return s;
}

const Serie& Runner::guarda_(Serie s) {
  series_.push_back(std::move(s));
  const Serie& r = series_.back();
  if (cfg_.verboso) {
    if (!r.medida) {
      (void)std::fprintf(stderr, "  %-44s PULADA — %s\n", r.nome.c_str(), r.nota.c_str());
    } else {
      (void)std::fprintf(stderr, "  %-44s %14.2f %-9s CV %5.2f%%%s\n", r.nome.c_str(), r.mediana,
                         r.unidade.c_str(), r.cv_pct, r.estavel ? "" : "  INSTÁVEL");
    }
  }
  return r;
}

const Serie& Runner::medir(const char* grupo, const char* nome, Forma forma,
                           const std::function<Amostra()>& corpo) {
  return guarda_(roda_(grupo, nome, forma, [&corpo](Histogram*) { return corpo(); }, nullptr));
}

const Serie& Runner::medir_latencia(const char* grupo, const char* nome, Forma forma,
                                    const std::function<Amostra(Histogram&)>& corpo) {
  // O histograma vive fora da região medida e é reusado entre repetições: `record` não aloca, e
  // 20 KiB no heap uma vez por série é setup, não medição.
  auto hist = std::make_unique<Histogram>();
  return guarda_(
      roda_(grupo, nome, forma, [&corpo](Histogram* h) { return corpo(*h); }, hist.get()));
}

const Serie& Runner::pula(const char* grupo, const char* nome, Forma forma,
                          const std::string& motivo) {
  Serie s;
  s.grupo = grupo;
  s.nome = nome;
  s.forma = forma;
  s.unidade = unidade_de(forma);
  s.direcao = direcao_de(forma);
  s.medida = false;
  s.nota = motivo;
  return guarda_(std::move(s));
}

void Runner::anota(const std::string& texto) {
  if (series_.empty() || texto.empty()) return;
  std::string& n = series_.back().nota;
  if (!n.empty()) n += " | ";
  n += texto;
}

// ---------------------------------------------------------------------------- ambiente
std::string Ambiente::por_que_invalido() const {
  std::string m;
  if (!release) m += "build não é Release; ";
  if (!sem_sanitizer) m += "sanitizer ligado; ";
  if (!asserts_desligados) m += "asserts de invariante ligados; ";
  if (m.empty()) return m;
  m.erase(m.size() - 2);
  return m;
}

Ambiente coleta_ambiente(const std::string& caminho_wal) {
  Ambiente a;

  // O commit, com marca de sujo. Medir com árvore suja e publicar o hash limpo é o jeito mais
  // barato de tornar um número irreproduzível.
  const char* const cmd_commit[] = {"git", "rev-parse", "HEAD", nullptr};
  const char* const cmd_sujo[] = {"git", "status", "--porcelain", nullptr};
  a.commit = executa(cmd_commit);
  if (a.commit.empty()) {
    a.commit = "desconhecido";
  } else if (!executa(cmd_sujo).empty()) {
    a.commit += "+sujo";
  }

  utsname u{};
  a.kernel = (::uname(&u) == 0) ? std::string(u.release) : std::string("desconhecido");

  std::string cpu = primeira_linha_com("/proc/cpuinfo", "model name");
  if (cpu.empty()) cpu = "cpu desconhecida";
  const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
  a.maquina = cpu + " (" + (cores > 0 ? std::to_string(cores) : std::string("?")) + " cpus)";

  a.flags = std::string(MOTOR_RV_COMPILER_STR) + " " + MOTOR_RV_BUILD_TYPE_STR +
            " -march=" + MOTOR_RV_ARCH_STR + (MOTOR_RV_LTO_ON ? " -flto" : "") +
            (std::strlen(MOTOR_RV_SANITIZER_STR) > 0
                 ? std::string(" -fsanitize=") + MOTOR_RV_SANITIZER_STR
                 : std::string()) +
            (MOTOR_RV_INVARIANT_ASSERTS ? " asserts=on" : " asserts=off");

  // O dispositivo do WAL: filesystem E alinhamento de I/O direto, medidos no caminho de verdade
  // pelo MESMO código que o WAL usa (`probe_block_align_path`). Sem isto, uma medição de
  // durabilidade não diz sobre o que ela é.
  a.dispositivo_wal = caminho_wal + " -> " + fs_de(caminho_wal);
  const auto al = wal::probe_block_align_path(caminho_wal.c_str());
  if (al) {
    a.dispositivo_wal +=
        ", bloco=" + std::to_string(al->block) + " (" + wal::to_string(al->source) + ")";
  } else {
    a.dispositivo_wal += ", bloco=indisponível";
  }

  a.release = std::string(MOTOR_RV_BUILD_TYPE_STR) == "Release";
  a.sem_sanitizer = std::strlen(MOTOR_RV_SANITIZER_STR) == 0;
  a.asserts_desligados = MOTOR_RV_INVARIANT_ASSERTS == 0;
  return a;
}

}  // namespace rv::bench
