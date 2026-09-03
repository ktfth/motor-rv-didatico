# rv_format — as FOLHAS de formato. INTERFACE: só cabeçalhos de dados, zero comportamento.
#
# É o alvo que a borda enxerga do núcleo, e nada mais. `rv_edge` linka `rv_format` e NÃO linka
# `rv_core`: um `#include "core/..."` na borda quebra o link antes de qualquer revisão humana.
# É CODING_RULES §10 na forma executável.
add_library(rv_format INTERFACE)
add_library(rv::format ALIAS rv_format)
target_include_directories(rv_format INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rv_format INTERFACE rv::base)
