# O rótulo `concorrencia` existe para o testPreset `tsan-concorrencia`. Ele estava no filtro do
# preset e em nenhum CMakeLists: o tsan selecionava zero testes e reportava sucesso.
motor_rv_test(test_infra
  FONTES  test_infra.cpp
  LIGA    rv::base
  ROTULOS "base;concorrencia")
target_link_libraries(test_infra PRIVATE Threads::Threads)
