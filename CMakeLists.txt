# motor-rv — motor de renda variável (pós-negociação B3) em C++23.
#
# Leia docs/leitura-guiada.md antes deste arquivo se for a sua primeira visita ao repositório.
#
# Este CMakeLists faz três coisas e nada mais: exige o padrão, acha as dependências do sistema e
# entra nos subdiretórios. Toda política de flags mora em cmake/MotorRvFlags.cmake — inclusive
# para que o `verificador` tenha um arquivo só para revisar quando a política mudar.

cmake_minimum_required(VERSION 3.25)

project(motor_rv
  VERSION 0.1.0
  DESCRIPTION "Motor de renda variável com WAL io_uring, snapshot EOD e borda Open Finance"
  LANGUAGES CXX C)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR
    "motor-rv depende de io_uring e O_DIRECT: só Linux. Ver docs/ambiente.md.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MotorRvFlags)
include(CTest)

# ------------------------------------------------------------------ dependências
# Todas do sistema. O que não existe no sistema virou código nosso, com ADR (0017..0021).
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FMT IMPORTED_TARGET fmt)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  pkg_check_modules(GTEST IMPORTED_TARGET gtest gtest_main gmock)
  if(GTEST_FOUND AND NOT TARGET GTest::gtest)
    add_library(GTest::gtest      ALIAS PkgConfig::GTEST)
    add_library(GTest::gtest_main ALIAS PkgConfig::GTEST)
    add_library(GTest::gmock      ALIAS PkgConfig::GTEST)
  endif()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)   # gerador de codecs SBE (ADR-0017)

# ------------------------------------------------------------------ Python p/ o gerador
message(STATUS "motor-rv: arch=${MOTOR_RV_ARCH} lto=${MOTOR_RV_LTO} "
               "sanitizer='${MOTOR_RV_SANITIZER}' asserts=${MOTOR_RV_INVARIANT_ASSERTS}")

# ------------------------------------------------------------------ subdiretórios
# Adiciona só o que já existe: o repositório cresce por fases (docs/plano-fases.md) e um
# CMakeLists que quebra porque a fase 4 ainda não começou não ajuda ninguém.
foreach(sub IN ITEMS src tests bench)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sub}/CMakeLists.txt")
    add_subdirectory(${sub})
  else()
    message(STATUS "motor-rv: ${sub}/ ainda sem CMakeLists — pulando (fase pendente)")
  endif()
endforeach()
