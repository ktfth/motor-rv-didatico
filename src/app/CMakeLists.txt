# app/ — COMPOSIÇÃO. O único lugar do projeto com `while (true)` e com `new`.
#
# Toda a configuração e todo o ciclo de vida moram aqui; as bibliotecas abaixo não sabem que
# existe um `main`. É o que permite ao mesmo `core::Partition::poll` servir a produção, ao teste
# e ao replay sem nenhuma diferença.
add_executable(motor-rv-sim simulador_main.cpp)
target_link_libraries(motor-rv-sim PRIVATE motor_rv::flags rv::core rv::ingress)

add_executable(motor-rv-wal-inspect wal_inspect_main.cpp)
target_link_libraries(motor-rv-wal-inspect PRIVATE motor_rv::flags rv::wal rv::core rv::base)

add_executable(motor-rv motor_main.cpp)
target_link_libraries(motor-rv PRIVATE motor_rv::flags rv::edge rv::core rv::wal rv::ingress rv::codec rv::base)
