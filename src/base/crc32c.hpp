#pragma once
// CRC32C (polinômio de Castagnoli) — a integridade de cada registro do WAL e de cada seção do
// snapshot depende deste arquivo.
//
// Dois caminhos, escolhidos UMA vez no carregamento e nunca por chamada:
//   - `_mm_crc32_u64`, quando o processador tem SSE4.2 (a máquina de referência tem);
//   - uma tabela de 256 entradas, quando não tem.
// Escolher por chamada custaria um teste e um salto indireto no caminho mais quente do commit;
// escolher em compilação impediria o mesmo binário de rodar nas duas máquinas.
//
// Por que CRC e não hash criptográfico: o CRC protege contra escrita rasgada e bit podre, que é
// exatamente o que acontece num disco. Contra adversário, quem protege é a cifra em repouso
// (ADR-0015). Usar SHA aqui custaria uma ordem de grandeza para resolver o problema errado.

#include <cstddef>
#include <cstdint>

namespace rv {

// Calcula sobre `len` bytes, continuando de `crc` (passe 0 para começar).
[[nodiscard]] uint32_t crc32c(uint32_t crc, const void* data, size_t len) noexcept;

// Verdadeiro quando o caminho de hardware está ativo. Só para log e para o teste que exige que
// os dois caminhos deem o mesmo resultado.
[[nodiscard]] bool crc32c_uses_hardware() noexcept;

// Os dois caminhos, expostos para o teste de equivalência. Em produção, use `crc32c`.
[[nodiscard]] uint32_t crc32c_table(uint32_t crc, const void* data, size_t len) noexcept;
[[nodiscard]] uint32_t crc32c_hw(uint32_t crc, const void* data, size_t len) noexcept;

}  // namespace rv
