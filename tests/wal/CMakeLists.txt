# Formato e backends. Rótulos = invariantes cobertos (`ctest -L I9` responde quem cobre I9).
motor_rv_test(test_format
  FONTES  test_format.cpp
  LIGA    rv::wal
  # Sem os rótulos I8/I9: este teste verifica o LAYOUT dos registros, não o LSN.
  ROTULOS "wal;formato")

motor_rv_test(test_group_commit
  FONTES  test_group_commit.cpp
  LIGA    rv::wal
  ROTULOS "I9;wal;group_commit")

motor_rv_test(test_segment_reader
  FONTES  test_segment_reader.cpp
  LIGA    rv::wal
  ROTULOS "wal;segment_reader;padding")
