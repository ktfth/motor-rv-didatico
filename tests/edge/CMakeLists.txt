# tests/edge — testes da borda: observabilidade, SLA regulatório (R17, R18) e exportador Prometheus.

add_executable(test_r17_latency_slo test_r17_latency_slo.cpp)
target_link_libraries(test_r17_latency_slo
  PRIVATE
    motor_rv::flags
    rv::edge
    rv::base
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME test_r17_latency_slo COMMAND test_r17_latency_slo)
set_tests_properties(test_r17_latency_slo PROPERTIES LABELS "edge;r17;latency;slo")

add_executable(test_r18_availability_classes test_r18_availability_classes.cpp)
target_link_libraries(test_r18_availability_classes
  PRIVATE
    motor_rv::flags
    rv::edge
    rv::base
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME test_r18_availability_classes COMMAND test_r18_availability_classes)
set_tests_properties(test_r18_availability_classes PROPERTIES LABELS "edge;r18;availability;sla")

add_executable(test_observability test_observability.cpp)
target_link_libraries(test_observability
  PRIVATE
    motor_rv::flags
    rv::edge
    rv::base
    Threads::Threads
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME test_observability COMMAND test_observability)
set_tests_properties(test_observability PROPERTIES LABELS "edge;observability;prometheus;http")
