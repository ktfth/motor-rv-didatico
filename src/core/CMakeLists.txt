# rv_core — o núcleo. Hot path: sem exceções, sem RTTI, sem alocação depois do warm-up.
add_library(rv_core STATIC
  apply.cpp
  ledger.cpp
  outbox.cpp
  partition_state.cpp
  state_image.cpp
)
add_library(rv::core ALIAS rv_core)
target_link_libraries(rv_core PUBLIC rv::base rv::codec)
target_link_libraries(rv_core PRIVATE motor_rv::hot)
# `rv_core` NÃO linka `rv_wal`: ele declara o que precisa de um log (concept Journal) em vez de
# importar quem fornece. Inversão de dependência sem vtable — ver src/core/journal.hpp.
