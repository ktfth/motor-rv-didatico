#pragma once
// Saída de emergência do motor. Duas funções, nenhuma retorna.
//
// Por que existe um arquivo só para isto: `fixed.hpp` precisa abortar em overflow de ledger, mas
// não pode depender de métricas, de log nem de `fail_stop` da partição — todos dependem dele.
// `panic.hpp` é a folha mais baixa da árvore: declara, não implementa, e quem compõe o programa
// escolhe a implementação (o binário do motor registra e para a partição; o teste captura).

#include <cstdint>

namespace rv {

// Chamada quando uma operação exata de ponto fixo estouraria int64. Um bucket de ledger que
// estourou não tem resposta correta a não ser parar: continuar significa publicar número errado.
[[noreturn]] void panic_overflow(const char* onde, int64_t a, int64_t b) noexcept;

// Chamada quando uma pré-condição de programador é violada em build de release, onde o assert
// já foi removido mas a consequência ainda seria corrupção silenciosa.
[[noreturn]] void panic_precondition(const char* onde, const char* condicao) noexcept;

}  // namespace rv
