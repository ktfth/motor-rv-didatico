# bench/ — o harness de medição (ADR-0021). Dono: `desempenho`.
#
# Ele NÃO liga `motor_rv::hot`: liga `motor_rv::medicao`, que é a mesma ISA e a mesma otimização
# COM exceções. O motivo está escrito em cmake/MotorRvFlags.cmake — em resumo, o harness compila
# código de cabeçalho do hot path dentro das suas próprias unidades (SpscRing, DenseIndex, Fixed,
# o `poll` da partição), então medi-las com outra ISA mediria outro binário; mas o driver lê CSV e
# monta JSON, e ali `-fno-exceptions` seria restrição sem contrapartida.
#
# O alvo NÃO é registrado no CTest como teste. Medição não é teste: ela não tem veredito binário
# em máquina qualquer, e um `ctest` que falha porque a máquina do desenvolvedor estava ocupada
# ensina a ignorar o CTest. O gate de regressão é `motor-rv-bench --comparar`, chamado
# explicitamente por quem quer o gate.

add_executable(motor-rv-bench
  main.cpp
  harness.cpp
  saida.cpp
  comparador.cpp
  contrato.cpp
  carga.cpp
  bench_base.cpp
  bench_nucleo.cpp
  bench_snapshot.cpp
  bench_wal.cpp
)

target_link_libraries(motor-rv-bench
  PRIVATE motor_rv::medicao rv::core rv::ingress rv::wal Threads::Threads
)

# A raiz do repositório entra no caminho de include para que os arquivos daqui se incluam como
# "bench/...", do mesmo jeito que o resto do projeto se inclui como "core/..." e "base/...".
# Caminho relativo ("../harness.hpp") economizaria esta linha e custaria a propriedade de dar para
# mover um arquivo de diretório sem reescrever include nenhum.
target_include_directories(motor-rv-bench PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_definitions(motor-rv-bench PRIVATE MOTOR_RV_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

add_custom_target(quick-bench
  COMMAND motor-rv-bench --rapido
  DEPENDS motor-rv-bench
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Executando benchmark de sanidade rápida (--rapido)"
)
