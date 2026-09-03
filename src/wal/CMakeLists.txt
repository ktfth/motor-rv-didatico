# rv_wal — formato em disco e backends de I/O. Dono: `persistencia`.
#
# A biblioteca é hot path (`motor_rv::hot` vem por herança de `rv::base`): sem exceções em
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
