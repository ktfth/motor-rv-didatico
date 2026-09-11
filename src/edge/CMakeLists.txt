# rv_edge — camada de borda: observabilidade, métricas Prometheus, auditoria e FAPI.
#
# Regras de arquitetura (CODING_RULES §10):
# A borda linka rv::base e rv::format, e NÃO linka rv::core.
add_library(rv_edge STATIC
  audit.cpp
  audit.hpp
  endpoint.hpp
  metrics_server.cpp
  metrics_server.hpp
  observability.cpp
  observability.hpp
)

add_library(rv::edge ALIAS rv_edge)

target_include_directories(rv_edge PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(rv_edge
  PUBLIC
    motor_rv::flags
    rv::base
    rv::format
    Threads::Threads
)
