# Suíte de caos e crash do WAL. Cobre I8, I9, I10, I11, I12.
motor_rv_test(test_recovery_chaos
  FONTES  test_recovery_chaos.cpp
  LIGA    rv::wal rv::core
  ROTULOS "I8;I9;I10;I11;I12;chaos;wal;recovery")
