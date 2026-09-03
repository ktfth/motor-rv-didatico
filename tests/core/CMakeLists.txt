# Testes do núcleo. `ctest -L I11` responde quem cobre a equivalência de replay.
motor_rv_test(test_replay_equivalence
  FONTES  test_replay_equivalence.cpp
  LIGA    rv::core
  ROTULOS "I11;I12;nucleo;replay")

motor_rv_test(test_outbox_gate
  FONTES  test_outbox_gate.cpp
  LIGA    rv::core
  ROTULOS "I10;nucleo;outbox")

# I8 e I9 estavam rotulados em tests/wal/test_format.cpp, que não afirma nada sobre LSN. Rótulo
# não é verificação — e `check_invariants.py` agora exige que o teste MENCIONE o invariante.
motor_rv_test(test_lsn
  FONTES  test_lsn.cpp
  LIGA    rv::core
  ROTULOS "I8;I9;I10;nucleo;lsn")
