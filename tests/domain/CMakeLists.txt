# Cada teste declara os invariantes que cobre como RÓTULO do CTest. `ctest -L I4` responde
# "qual teste cobre I4" sem consultar documento nenhum — é a mecanização de
# "invariante sem teste não existe" (docs/invariantes.md).
motor_rv_test(test_rounding_golden
  FONTES  test_rounding_golden.cpp
  LIGA    rv::base
  ROTULOS "I4;I7;dominio;golden")

motor_rv_test(test_apply_golden
  FONTES  test_apply_golden.cpp
  LIGA    rv::core
  ROTULOS "I1;I3;I4;I5;I6;I13;dominio;golden")

motor_rv_test(test_i2_cash_by_date
  FONTES  test_i2_cash_by_date.cpp
  LIGA    rv::core
  ROTULOS "I2;I5;dominio")

motor_rv_test(test_allocation
  FONTES  test_allocation.cpp
  LIGA    rv::core
  ROTULOS "I2;I3;I5;dominio;alocacao")

motor_rv_test(test_trade_state_machine
  FONTES  test_trade_state_machine.cpp
  LIGA    rv::core
  ROTULOS "I5;dominio;maquina-de-estados")
