# Testes do núcleo. `ctest -L I11` responde quem cobre a equivalência de replay.
motor_rv_test(test_replay_equivalence
  FONTES  test_replay_equivalence.cpp
  LIGA    rv::core
  ROTULOS "I11;I12;nucleo;replay")

motor_rv_test(test_outbox_gate
  FONTES  test_outbox_gate.cpp
  LIGA    rv::core
  ROTULOS "I10;nucleo;outbox")
