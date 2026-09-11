# tests/ingress — testes de framing, SBE stream e pipeline de ingresso de rede.

add_executable(test_ingress_pipeline test_ingress_pipeline.cpp)
target_link_libraries(test_ingress_pipeline
  PRIVATE
    motor_rv::flags
    rv::ingress
    rv::core
    rv::codec
    rv::base
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME test_ingress_pipeline COMMAND test_ingress_pipeline)
set_tests_properties(test_ingress_pipeline PROPERTIES LABELS "ingress;rede;sbe")
