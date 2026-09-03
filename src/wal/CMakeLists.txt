# rv_wal — formato em disco e backends de I/O. Dono: `persistencia`.
#
# A biblioteca é hot path e liga `motor_rv::hot` EXPLICITAMENTE, como PRIVATE.
#
# O comentário anterior dizia que a flag vinha "por herança de `rv::base`". Não vinha: `rv_base`
# liga `motor_rv::hot` como PRIVATE — corretamente, porque `-march`, `-O3` e `-fno-exceptions` são
# propriedades das unidades de compilação de cada camada, não de quem a consome. O resultado é que
# `rv_wal` compilava sem nenhuma delas enquanto o arquivo afirmava o contrário. Comentário que
# descreve o build errado é pior que comentário nenhum: ele impede a pergunta.
#
# Sem exceções em
# release, `-march` do baseline. Depende de `rv::base` e de mais nada do projeto — em particular
# NÃO depende de `rv::core`: o WAL conhece o núcleo só pela interface de `apply`, e nesta fase
# nem isso (CODING_RULES §10).
add_library(rv_wal STATIC
  block_align.cpp
  io_uring_backend.cpp
  pwrite_backend.cpp
)
add_library(rv::wal ALIAS rv_wal)

target_include_directories(rv_wal PUBLIC ${CMAKE_SOURCE_DIR}/src)

# liburing é PUBLIC porque `io_uring_backend.hpp` inclui <liburing.h>: quem compõe o programa (e o
# teste dos backends) compila contra o cabeçalho. Os outros arquivos do WAL não o veem — é o
# ponto de ter um cabeçalho separado por backend.
target_link_libraries(rv_wal
  PUBLIC rv::base PkgConfig::URING
)
target_link_libraries(rv_wal PRIVATE motor_rv::hot)
