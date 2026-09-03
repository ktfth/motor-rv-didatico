# Cada teste declara os invariantes que cobre como RÓTULO do CTest. `ctest -L I4` responde
# "qual teste cobre I4" sem consultar documento nenhum — é a mecanização de
# "invariante sem teste não existe" (docs/invariantes.md).
motor_rv_test(test_rounding_golden
  FONTES  test_rounding_golden.cpp
  LIGA    rv::base
  ROTULOS "I4;I7;dominio;golden")
