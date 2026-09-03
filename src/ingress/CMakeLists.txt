# rv_ingress — os adaptadores. Traduzem o mundo para eventos; não conhecem `core` internals.
#
# Ele usa `core/partition.hpp` só pelo tamanho do quadro de ingresso (`kMaxIngressPayload`) — o
# roteamento precisa saber quanto cabe num slot do ring. É a única dependência, e ela é de dado.
add_library(rv_ingress STATIC simulator.cpp)
add_library(rv::ingress ALIAS rv_ingress)
target_link_libraries(rv_ingress PUBLIC rv::base rv::codec rv::core)
