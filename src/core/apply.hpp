#pragma once
// A fronteira do núcleo: uma função.
//
//     Status apply(PartitionState&, const EventView&, ApplyContext&) noexcept;
//
// Tudo o que o motor faz com um evento acontece aqui. O log é a verdade do que CHEGOU, não do que
// foi aceito: `append` acontece ANTES de `apply`, então evento rejeitado está no log — e o replay
// o rejeita de novo, pelo mesmo código, com o mesmo `Status`. Não existe "caminho de validação"
// separado do caminho de aplicação; existiriam duas verdades.
//
// ---------------------------------------------------------------------------------------------
// CONTRATO DE DETERMINISMO — D1..D7. É o texto que reprova um PR.
//
//  D1  `apply` é função pura de (estado, evento): mesmo par ⇒ mesmo estado final, mesmo `Status`,
//      mesmas entradas de outbox, na mesma ordem.                      (I11)
//  D2  Sem relógio, sem RNG, sem I/O. `ev.ts_ns` só pode ser COPIADO para uma saída — nunca
//      comparado, nunca somado.                                        (I12)
//  D3  Sem alocação: tudo vem das arenas de `state`, já seladas.
//  D4  Sem dependência de endereço: nada de iterar tabela de hash e depender da ordem, nada de
//      comparar ponteiros.
//  D5  Sem `double`/`float` em nenhum ponto de `rv_core`.
//  D6  Sem exceções.
//  D7  Mudar o SIGNIFICADO de um `templateId` existente é proibido; comportamento novo pede
//      template novo e bump de versão de schema.

#include <cstdint>

#include "base/ids.hpp"
#include "base/status.hpp"

namespace rv {
class Metrics;
}

namespace rv::core {

class Outbox;
struct PartitionState;

// 32 bytes, trivialmente copiável: cabe em registradores.
struct EventView {
  Lsn lsn;
  uint64_t ts_ns;            // AUDITORIA. Ver D2.
  const std::byte* payload;  // buffer de commit do WAL; válido até o próximo `maybe_submit`
  uint16_t tmpl;             // == codec::Tmpl
  uint16_t len;
  uint32_t reserved;
};
static_assert(sizeof(EventView) == 32);

// Tudo o que `apply` pode tocar além do estado. Se não está aqui, `apply` não alcança — e é essa
// lista curta que torna D2 verificável por leitura, e não só por teste.
struct ApplyContext {
  Outbox* outbox;
  Metrics* metrics;
};

// Duas classes de falha, e só duas.
//
//   Rejected — entrada externa inválida ou regra de negócio. Métrica sobe, o evento é consumido,
//              o motor segue. Derrubar a partição porque uma corretora mandou lixo transformaria
//              o erro do outro em indisponibilidade nossa.
//   Fatal    — corrupção: log inconsistente, bucket estourado, arena sem espaço. A partição para,
//              o outbox congela. Continuar significaria publicar número errado.
//
// A classificação vem da FAIXA numérica de `Err` (base/status.hpp), não de uma tabela paralela
// que alguém esquece de atualizar.
enum class ApplyClass : uint8_t { Accepted, Rejected, Fatal };

[[nodiscard]] constexpr ApplyClass classify(Status s) noexcept {
  if (s.is_ok()) return ApplyClass::Accepted;
  return s.is_fatal() ? ApplyClass::Fatal : ApplyClass::Rejected;
}

[[nodiscard]] Status apply(PartitionState& state, const EventView& ev, ApplyContext& ctx) noexcept;

}  // namespace rv::core
