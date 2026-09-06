#pragma once
// O CONTRATO de métricas: uma definição só de quais chaves `bench/baseline.json` publica, que
// série de medição alimenta cada uma, e o que cada uma significa em português corrente.
//
// ---------------------------------------------------------------------------------------------
// POR QUE ISTO É UM ARQUIVO, E NÃO UMA TABELA EM CADA LUGAR QUE PRECISA DELA
//
// A correspondência série → chave do baseline estava escrita TRÊS vezes: em `saida.cpp` (que
// emite o JSON), em `comparador.cpp` (que confronta com o baseline) e no default de `--exigir`
// de `scripts/relatorio-bench.py` (que decide se o gate chegou a olhar a métrica obrigatória).
// Renomear uma série atualizava uma delas; as outras duas ficavam procurando um nome que não
// existe mais.
//
// E procurar um nome que não existe é SILÊNCIO, não erro: o comparador não tem como distinguir
// "a série não regrediu" de "a série não está aqui". O gate seguiria verde tendo comparado coisa
// nenhuma — o defeito que `HANDOFF.md` descreve como o pior deste projeto, e que já apareceu duas
// vezes nele (o LTO que nunca ligava, o preset `tsan` que selecionava zero testes).
//
// Agora a tabela é uma. O C++ a lê dos dois lados; o Python NÃO a copia — ele a recebe pelo bloco
// `contrato` do próprio JSON de medição, que `saida.cpp` emite desta tabela. Uma verdade, três
// consumidores, nenhuma cópia.
//
// As duas defesas contra o retorno do defeito:
//
//   1. COMPILAÇÃO. O nome da série é `constexpr` aqui e usado no ponto de registro (`bench_*.cpp`),
//      então renomear é editar UMA linha e os dois lados a seguem. `esquema_coerente()` confere no
//      compilador o que a tabela não pode deixar de ser.
//   2. EXECUÇÃO. Quem trocar a constante por um literal no ponto de registro cai em
//      `contrato_quebrado()`: a suíte rodou, a série contratual não apareceu, e o harness sai com
//      erro em vez de gravar um JSON com a métrica obrigatória nula.

#include <cstdint>
#include <string>
#include <vector>

#include "bench/harness.hpp"

namespace rv::bench {

// Os nomes das séries que alimentam o baseline. Constantes porque são citadas em dois lugares
// (o registro da série e a tabela abaixo), e dois literais iguais é a forma de um deles envelhecer.
inline constexpr const char* kSerieNucleoLoop = "nucleo.loop.eventos_por_s_por_core";
inline constexpr const char* kSerieSnapshotSalva = "snapshot.salva.duracao_ms";

// A forma do valor no JSON. Não é enfeite de emissão: é o que permite `escreve_json` gerar o
// bloco `metricas` a partir da tabela em vez de o ter digitado em prosa, com os `null` literais
// dentro de `fputs` e a tabela ao lado sendo ignorada.
enum class FormaValor : uint8_t {
  Escalar,  // um número, ou `null` enquanto não houver série estável
  Objeto,   // um objeto de quantis: { "p50": null, "p99": null }
  Mapa,     // um mapa que só ganha chaves quando a borda existir: {}
};

struct Metrica {
  const char* chave;   // a chave em `bench/baseline.json` (sem o prefixo `metricas.`)
  const char* grupo;   // a suíte que a mede; vazio quando nenhuma mede ainda
  const char* serie;   // a série que a preenche; nullptr = ainda não medida
  const char* rotulo;  // o que ela é, para quem não conhece o projeto — o resumo do relatório
  FormaValor forma;
  const char* subcampos;  // "p50,p99,p999" quando `forma` é Objeto; nullptr nas outras
  const char* motivo;     // por que ainda não é medida; nullptr quando há série
};

// O esquema, na ORDEM em que `bench/baseline.json` o publica. Acrescentar uma métrica ao esquema
// é acrescentar uma linha aqui — e só aqui.
inline constexpr Metrica kEsquemaMetricas[] = {
    {.chave = "nucleo.eventos_por_s_por_core",
     .grupo = "nucleo",
     .serie = kSerieNucleoLoop,
     .rotulo = "Eventos de um pregão processados por segundo, por core",
     .forma = FormaValor::Escalar,
     .subcampos = nullptr,
     .motivo = nullptr},
    {.chave = "wal.append_para_duravel_us",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Tempo entre gravar um evento e ele estar seguro no disco",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,p99,p999",
     .motivo = "o escritor do WAL (segment/group_commit/wal) não existe ainda; só o formato e os "
               "backends. O piso físico do dispositivo está nas séries wal.*_backend_*"},
    {.chave = "wal.tamanho_grupo_bytes",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Quantos bytes cada grupo de gravação junta antes de ir ao disco",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,p99",
     .motivo = "não há group commit para agrupar: mesma pendência"},
    {.chave = "wal.grupos_em_voo",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Quantos grupos de gravação ficam esperando o disco ao mesmo tempo",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,max",
     .motivo = "não há group commit: mesma pendência"},
    {.chave = "wal.recuperacao_s",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Tempo para o motor voltar ao ar relendo o log depois de uma queda",
     .forma = FormaValor::Escalar,
     .subcampos = nullptr,
     .motivo = "não há wal/recovery.cpp; a velocidade de reaplicação está em "
               "nucleo.apply.eventos_por_s, que é o limite superior (sem custo de leitura de "
               "disco)"},
    {.chave = "snapshot.duracao_ms",
     .grupo = "snapshot",
     .serie = kSerieSnapshotSalva,
     .rotulo = "Tempo para gravar a imagem de recuperação (o motor fica parado nele)",
     .forma = FormaValor::Escalar,
     .subcampos = nullptr,
     .motivo = nullptr},
    {.chave = "rs.latencia_por_endpoint_ms",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Tempo de resposta de cada endpoint da API de Open Finance",
     .forma = FormaValor::Mapa,
     .subcampos = nullptr,
     .motivo = "src/edge/ não existe (fase 4)"},
};

// O que a tabela não pode deixar de ser, conferido pelo compilador. Cada condição existe por um
// jeito concreto de o JSON sair mentindo.
[[nodiscard]] constexpr bool esquema_coerente() noexcept {
  for (const Metrica& m : kEsquemaMetricas) {
    if (m.chave == nullptr || m.rotulo == nullptr || m.grupo == nullptr) return false;
    // Ou a métrica tem série que a preenche, ou tem motivo de não ter — nunca as duas, nunca
    // nenhuma. `null` sem explicação ao lado é como um baseline vira folclore.
    if ((m.serie == nullptr) == (m.motivo == nullptr)) return false;
    // Métrica medida sem grupo não dá para conferir na execução: `contrato_quebrado()` precisa
    // saber qual suíte deveria tê-la produzido.
    if (m.serie != nullptr && *m.grupo == '\0') return false;
    if ((m.forma == FormaValor::Objeto) == (m.subcampos == nullptr)) return false;
    // `escreve_json` só sabe preencher escalar. Dar uma série a uma métrica de quantis sem
    // ensinar o emissor a escrevê-la publicaria um objeto de nulos ao lado de uma série medida —
    // exatamente o silêncio que esta tabela existe para impedir. Quebrar aqui é o aviso.
    if (m.forma != FormaValor::Escalar && m.serie != nullptr) return false;
  }
  return true;
}
static_assert(esquema_coerente(),
              "bench/contrato.hpp: métrica sem série E sem motivo, série sem grupo, quantis sem "
              "subcampos, ou série ligada a uma métrica que escreve_json ainda não sabe emitir");

// A série de nome `nome` entre as medidas, ou nullptr.
[[nodiscard]] const Serie* serie_de(const std::vector<Serie>& series, const char* nome) noexcept;

// As séries contratuais que a execução DEVERIA ter registrado e não registrou: a suíte do grupo
// rodou (há séries dela na lista) mas o nome do contrato não apareceu. É o caso do rename que
// atualiza um lado só — antes desta função ele saía como métrica nula e gate verde.
[[nodiscard]] std::vector<std::string> contrato_quebrado(const std::vector<Serie>& series);

}  // namespace rv::bench
