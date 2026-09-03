# Testes do núcleo. `ctest -L I11` responde quem cobre a equivalência de replay.
motor_rv_test(test_replay_equivalence
  FONTES  test_replay_equivalence.cpp
  LIGA    rv::core
  ROTULOS "I11;I12;nucleo;replay")
target_include_directories(test_replay_equivalence PRIVATE ${CMAKE_SOURCE_DIR}/tests/domain)
