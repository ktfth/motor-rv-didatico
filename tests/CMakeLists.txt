if(NOT TARGET GTest::gtest)
  message(STATUS "motor-rv: GoogleTest ausente — testes desligados")
  return()
endif()
# O andaime compartilhado entra no caminho de include de TODA suíte: ele não é de ninguém.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/support)

add_subdirectory(base)
add_subdirectory(domain)
add_subdirectory(core)
add_subdirectory(wal)
