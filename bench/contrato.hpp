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
// `contrato` do próprio JSON de medição, que `saida.cpp` emite desta tabela.
//
// Falta um quarto lugar onde as mesmas chaves aparecem, e ele NÃO é gerado: `bench/baseline.json` é
// versionado e editado à mão. Renomear uma chave só lá reproduz o defeito inteiro — o comparador
// procura o que não existe, imprime "SEM BASELINE" e, antes, saía 0. Como o arquivo não pode nascer
// daqui (ele é o alvo, e quem o escreve é `--gravar-baseline` na máquina de referência), ele é
// CONFERIDO daqui: `divergencias_de_esquema()`, que o CI roda a cada PR.
//
// As três defesas contra o retorno do defeito:
//
//   1. COMPILAÇÃO. O nome da série é `constexpr` aqui e usado no ponto de registro (`bench_*.cpp`),
//      então renomear é editar UMA linha e os dois lados a seguem. `esquema_coerente()` confere no
//      compilador o que a tabela não pode deixar de ser.
//   2. EXECUÇÃO. Quem trocar a constante por um literal no ponto de registro cai em
//      `contrato_quebrado()`: a suíte rodou, a série contratual não apareceu, e o harness sai com
//      erro em vez de gravar um JSON com a métrica obrigatória nula.
//   3. ARQUIVO. `--conferir-esquema` confronta as chaves do baseline versionado com esta tabela e
//      reprova na divergência, nos dois sentidos (chave que falta, chave que sobra).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bench/harness.hpp"

namespace rv::bench {

// Os nomes das séries que alimentam o baseline. Constantes porque são citadas em dois lugares
// (o registro da série e a tabela abaixo), e dois literais iguais é a forma de um deles envelhecer.
inline constexpr const char* kSerieNucleoLoop = "nucleo.loop.eventos_por_s_por_core";
inline constexpr const char* kSerieNucleoApply = "nucleo.apply.eventos_por_s";
inline constexpr const char* kSerieSnapshotSalva = "snapshot.salva.duracao_ms";
inline constexpr const char* kSerieWalAppendParaDuravel = "wal.append_para_duravel_us";
inline constexpr const char* kSerieWalRecuperacao = "wal.recuperacao.duracao_s";

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
     .grupo = "wal",
     .serie = kSerieWalAppendParaDuravel,
     .rotulo = "Tempo entre gravar um evento e ele estar seguro no disco",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,p99,p999",
     .motivo = nullptr},
    {.chave = "wal.tamanho_grupo_bytes",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Quantos bytes cada grupo de gravação junta antes de ir ao disco",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,p99",
     .motivo = "aguarda amostragem de distribuição de tamanho de grupo na fase 3"},
    {.chave = "wal.grupos_em_voo",
     .grupo = "",
     .serie = nullptr,
     .rotulo = "Quantos grupos de gravação ficam esperando o disco ao mesmo tempo",
     .forma = FormaValor::Objeto,
     .subcampos = "p50,max",
     .motivo = "aguarda amostragem de grupos em voo na fase 3"},
    {.chave = "wal.recuperacao_s",
     .grupo = "wal",
     .serie = kSerieWalRecuperacao,
     .rotulo = "Tempo para o motor voltar ao ar relendo o log depois de uma queda",
     .forma = FormaValor::Escalar,
     .subcampos = nullptr,
     .motivo = nullptr},
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

// ---------------------------------------------------------------------------------------------
// O QUE REPROVA UM PR NÃO É O QUE VIRA BASELINE
//
// São duas perguntas, e elas estavam no mesmo campo. `kEsquemaMetricas` diz quais métricas o
// `bench/baseline.json` publica — o alvo que `desempenho` fixa na máquina de referência. O portão
// A/B do CI faz outra pergunta: quais séries, se piorarem, REPROVAM este PR. Enquanto a segunda
// resposta era "as da primeira", só 2 das 18 séries medidas podiam reprovar: uma piora de 195 % em
// `snapshot.carrega.duracao_ms` — o tempo de voltar ao ar depois de uma queda — passava como
// "informativa, não vota".
//
// A lista abaixo é a segunda resposta, e ela é maior. Ela também é MEDIDA: uma série só bloqueia
// se o ruído dela couber no portão. Ver bench/README.md para a campanha e os números.
struct SerieBloqueante {
  const char* serie;
  const char* grupo;   // a suíte que a mede — `contrato_quebrado()` confere que ela apareceu
  const char* rotulo;  // o que ela é, em português corrente, para o resumo do relatório
};

inline constexpr SerieBloqueante kSeriesBloqueantes[] = {
    {.serie = kSerieNucleoLoop,
     .grupo = "nucleo",
     .rotulo = "Eventos de um pregão processados por segundo, por core"},
};

// TRÊS candidatas ficaram de fora, e a ausência das três é medida — não esquecimento.
//
// Antes dos números, o método, porque ele mudou no meio: um "PR inocente" NÃO é o mesmo binário
// medido dos dois lados. O job compila as DUAS árvores, e dois binários de código idêntico têm
// layout diferente. Medir o mesmo binário contra si mesmo esconde esse viés por construção — foi o
// que as duas primeiras campanhas fizeram, e por isso elas deram 0/30 para uma série que reprova
// 13 de 15 PRs inocentes de verdade. As linhas abaixo dizem qual campanha produziu cada número.
//
// Com o MESMO binário dos dois lados (30 rodadas, parâmetros do job, 5 execuções por lado):
//
//   `snapshot.carrega.duracao_ms` — 1 vermelho falso (saltou de 9,09 para 16,89 ms, +85,8 %, entre
//   execuções do MESMO binário) e 2 rodadas cegas (exigido de 50,8 % e 59,3 %, acima do teto). É a
//   série que a restauração proporcional à capacidade configurada torna bimodal (HANDOFF, achado
//   9): ela é exatamente o que se quer medir, e por isso não serve para reprovar ninguém enquanto
//   o defeito que ela expõe estiver aberto.
//
//   `nucleo.apply.eventos_por_s` — 1 vermelho falso em 30. Ela entrou na lista, foi medida, e saiu
//   por causa do número: numa rodada os dois lados derivaram (6,37 M contra 5,48 M, −13,8 %) com
//   um ruído estimado de ±2,8 %, e o portão reprovou um PR que não mudou uma linha. O ruído por
//   lado não enxerga um degrau da máquina ENTRE os dois blocos; nas mesmas 30 rodadas,
//   `nucleo.loop` e `snapshot.salva` deram 0.
//
// E com DOIS BINÁRIOS de código semanticamente idêntico (15 rodadas — o mesmo commit com uma
// edição neutra em bench/harness.cpp, que só muda o layout):
//
//   `snapshot.salva.duracao_ms` — 13 vermelhos falsos em 15. Ela é repetível DENTRO de um binário
//   (±3 %) e anda de +9,0 % a +18,5 % ENTRE binários que só diferem de layout; um limiar exigido
//   de 6-8 % não tem como sobreviver a isso. A mesma edição neutra move a série de 0,32 para
//   0,37 ms sem tocar em uma linha de `save_state_image`. `nucleo.loop`, na mesma campanha, deu
//   0/15: o ruído próprio dela (exigido 10-21 %) já cobre o viés de layout.
//
// As três voltam a ser candidatas quando houver algo que as sustente — a convergência das séries de
// carga, na primeira; uma reexecução de confirmação antes de reprovar, na segunda; um piso de
// limiar por série, calibrado pelo viés de layout medido, na terceira. Qualquer uma delas é decisão
// do orquestrador, não deste arquivo. Uma série só bloqueia com a medição do lado.

// O que a tabela não pode deixar de ser, conferido pelo compilador. Cada condição existe por um
// jeito concreto de o JSON sair mentindo.
[[nodiscard]] constexpr bool esquema_coerente() noexcept {
  // `all_of` com um predicado de cinco condições explicadas uma a uma fica pior de ler do que o
  // laço, e isto é código de conferência: quem o lê está atrás das condições, não da forma.
  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (const Metrica& m : kEsquemaMetricas) {
    if (m.chave == nullptr || m.rotulo == nullptr || m.grupo == nullptr) return false;
    // Ou a métrica tem série que a preenche, ou tem motivo de não ter — nunca as duas, nunca
    // nenhuma. `null` sem explicação ao lado é como um baseline vira folclore.
    if ((m.serie == nullptr) == (m.motivo == nullptr)) return false;
    // Métrica medida sem grupo não dá para conferir na execução: `contrato_quebrado()` precisa
    // saber qual suíte deveria tê-la produzido.
    if (m.serie != nullptr && *m.grupo == '\0') return false;
    if ((m.forma == FormaValor::Objeto) == (m.subcampos == nullptr)) return false;
    // `escreve_json` e `compara` suportam Escalar e Objeto (quantis). Mapa não tem emissor de
    // série ainda.
    if (m.forma == FormaValor::Mapa && m.serie != nullptr) return false;
  }
  return true;
}
// A lista de bloqueantes tem as mesmas duas exigências: campos preenchidos, e grupo declarado
// (sem grupo, `contrato_quebrado()` não teria como saber que suíte deveria tê-la produzido).
[[nodiscard]] constexpr bool bloqueantes_coerentes() noexcept {
  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (const SerieBloqueante& b : kSeriesBloqueantes) {
    if (b.serie == nullptr || b.grupo == nullptr || b.rotulo == nullptr) return false;
    if (*b.serie == '\0' || *b.grupo == '\0') return false;
  }
  return true;
}
static_assert(bloqueantes_coerentes(),
              "bench/contrato.hpp: série bloqueante sem nome, sem grupo ou sem rótulo");

static_assert(esquema_coerente(),
              "bench/contrato.hpp: métrica sem série E sem motivo, série sem grupo, quantis sem "
              "subcampos, ou série ligada a uma métrica que escreve_json ainda não sabe emitir");

// ---------------------------------------------------------------------------------------------
// A CARGA, PELA MESMA REGRA
//
// Quais campos definem a carga era outra lista escrita duas vezes: aqui (o comparador confere um
// por um) e em `scripts/relatorio-bench.py` (que recusa comparar lados de cargas diferentes), com
// um comentário admitindo que eram "os MESMOS cinco". Mesma forma de defeito da tabela de
// métricas: acrescentar um campo à carga atualizava um dos dois, e o outro deixava de conferi-lo
// em silêncio. Agora a lista sai daqui e viaja no bloco `contrato` do JSON.
//
// `eventos` fica de fora de propósito: ele é consequência dos outros cinco. Uma mudança no
// simulador que produza mais eventos para a mesma configuração é a diferença que se QUER medir,
// não a que invalida a medição.
[[nodiscard]] std::vector<std::pair<std::string, uint64_t>> campos_da_carga(
    const DescricaoCarga& carga);

// A série de nome `nome` entre as medidas, ou nullptr.
[[nodiscard]] const Serie* serie_de(const std::vector<Serie>& series, const char* nome) noexcept;

// As séries contratuais que a execução DEVERIA ter registrado e não registrou: a suíte do grupo
// rodou (há séries dela na lista) mas o nome do contrato não apareceu. É o caso do rename que
// atualiza um lado só — antes desta função ele saía como métrica nula e gate verde. Confere as
// duas listas: as que alimentam o baseline e as que bloqueiam um PR.
[[nodiscard]] std::vector<std::string> contrato_quebrado(const std::vector<Serie>& series);

// As divergências entre as chaves de um arquivo de baseline e as deste esquema: chave contratual
// que falta no arquivo, e chave `metricas.*` do arquivo que este esquema não conhece.
//
// Existe porque `bench/baseline.json` é versionado à mão e era a QUARTA cópia da tabela: renomear
// a chave só lá fazia o comparador não achar nada e sair 0 — "SEM BASELINE, nada a comparar" —
// que é o mesmo gate verde sem ter comparado, mudado de lugar. `--conferir-esquema` roda isto no
// CI, contra o arquivo versionado.
//
// `caminhos` é o documento já achatado pelo leitor do comparador ("metricas.a.b"). Métrica de
// forma `Mapa` não é conferida: um objeto vazio não deixa caminho nenhum no documento achatado, e
// exigir presença dela seria exigir que alguém escrevesse `{}` com um filho dentro.
[[nodiscard]] std::vector<std::string> divergencias_de_esquema(
    const std::vector<std::string>& caminhos);

}  // namespace rv::bench
