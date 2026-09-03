# Formato e backends. Rótulos = invariantes cobertos (`ctest -L I9` responde quem cobre I9).
motor_rv_test(test_format
  FONTES  test_format.cpp
  LIGA    rv::wal
  ROTULOS "I8;I9;wal;formato")
