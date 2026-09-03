# rv_base — o vocabulário. Não depende de nada do projeto; todo o resto depende dele.
add_library(rv_base STATIC
  panic.cpp
  status.cpp
  crc32c.cpp
)
add_library(rv::base ALIAS rv_base)

target_include_directories(rv_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rv_base PUBLIC motor_rv::hot)

# CRC32C precisa de SSE4.2 no arquivo que usa _mm_crc32_u64. A flag NÃO vai no alvo inteiro:
# o baseline é x86-64-v2 (ADR-0022), que já inclui SSE4.2, mas o atributo `target("sse4.2")` na
# própria função mantém o arquivo compilável mesmo num baseline mais baixo.
set_source_files_properties(crc32c.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2")
