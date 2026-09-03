# tests/support — andaimes compartilhados

Donos: `dominio-pos-negociacao` e `nucleo`. Código que os testes usam e o motor não conhece.

| Arquivo | O que é |
|---|---|
| `engine_fixture.hpp` | Uma partição em memória (`Engine`) e os construtores dos dez eventos. Nenhum toque em disco, relógio ou rede — a mesma restrição do `apply` (D2). |
| `cenario.hpp` | Gerador determinístico de sessões: LCG com semente fixa, mistura de eventos aceitos e **rejeitados**. Sem `std::random_device` — um teste de replay com aleatoriedade real falharia de vez em quando e ninguém saberia reproduzir. |

Estes arquivos ficaram primeiro dentro de `tests/domain/` e `tests/core/`, e cada suíte que
precisava do andaime da outra acrescentava um `target_include_directories` cruzado. Dois cruzamentos
depois ficou claro que o andaime não pertence a nenhuma das duas: pertence às duas.
