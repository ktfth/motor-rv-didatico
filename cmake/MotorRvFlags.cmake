# Flags do projeto em um lugar só.
#
# Regra: nenhum CMakeLists de módulo escreve flag de compilador. Todos linkam a biblioteca de
# interface `motor_rv::flags` (avisos, padrão, sanitizers) e, se forem hot path, também
# `motor_rv::hot` (arquitetura, LTO, -fno-exceptions). Assim uma mudança de política acontece
# aqui e em nenhum outro lugar — e o `verificador` tem um arquivo só para revisar.

include_guard(GLOBAL)

set(MOTOR_RV_ARCH "x86-64-v2" CACHE STRING
    "Nível de ISA do baseline. ADR-0022: a máquina de referência não tem AVX2, então v3 gera SIGILL.")
set(MOTOR_RV_SANITIZER "" CACHE STRING
    "Lista para -fsanitize=, vazia por padrão. Ex.: address,undefined | thread")
option(MOTOR_RV_LTO "Link-time optimization no baseline" OFF)
option(MOTOR_RV_INVARIANT_ASSERTS "Asserts de invariante (I1..I12) dentro do apply" ON)
option(MOTOR_RV_FUZZERS "Compila os alvos de fuzz da borda" OFF)
option(MOTOR_RV_WERROR "Trata aviso como erro (CODING_RULES §9)" ON)

# ---------------------------------------------------------------- flags comuns
add_library(motor_rv_flags INTERFACE)
add_library(motor_rv::flags ALIAS motor_rv_flags)

target_compile_features(motor_rv_flags INTERFACE cxx_std_23)

target_compile_options(motor_rv_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion   # dinheiro em int64: conversão silenciosa é bug de dinheiro
  -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough
  $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast>
  $<$<BOOL:${MOTOR_RV_WERROR}>:-Werror>
  -fno-omit-frame-pointer          # perf e backtrace de fail-stop valem mais que um registrador
)

target_compile_definitions(motor_rv_flags INTERFACE
  $<$<BOOL:${MOTOR_RV_INVARIANT_ASSERTS}>:MOTOR_RV_INVARIANT_ASSERTS=1>
  $<$<NOT:$<BOOL:${MOTOR_RV_INVARIANT_ASSERTS}>>:MOTOR_RV_INVARIANT_ASSERTS=0>
)

if(MOTOR_RV_SANITIZER)
  # Sanitizer precisa das mesmas flags na compilação e no link, senão o runtime não entra.
  target_compile_options(motor_rv_flags INTERFACE -fsanitize=${MOTOR_RV_SANITIZER} -g)
  target_link_options(motor_rv_flags    INTERFACE -fsanitize=${MOTOR_RV_SANITIZER})
  # ASan e o hot path não convivem com -fno-exceptions + arenas sem redzone: o preset asan
  # desliga a otimização agressiva de propósito. Ver docs/ambiente.md.
endif()

# ------------------------------------------------------- flags só do hot path
# Núcleo, WAL e codecs. A borda NÃO usa: ela precisa de exceções (OpenSSL, parsing) e não é
# medida pelo baseline de eventos/s.
add_library(motor_rv_hot INTERFACE)
add_library(motor_rv::hot ALIAS motor_rv_hot)
target_link_libraries(motor_rv_hot INTERFACE motor_rv::flags)

if(MOTOR_RV_ARCH STREQUAL "native")
  target_compile_options(motor_rv_hot INTERFACE -march=native)
elseif(MOTOR_RV_ARCH)
  target_compile_options(motor_rv_hot INTERFACE -march=${MOTOR_RV_ARCH})
endif()

# `MOTOR_RV_SANITIZER` é uma LISTA ("address,undefined"), e a vírgula separa argumentos de
# generator expression: `$<BOOL:address,undefined>` é erro de sintaxe. Por isso a condição é
# reduzida a um booleano simples AQUI, fora da expressão. Custou um `cmake --preset asan` para
# descobrir — e é a razão de os presets serem todos exercitados, não só o `debug`.
if(MOTOR_RV_SANITIZER)
  set(_sem_sanitizer 0)
else()
  set(_sem_sanitizer 1)
endif()

target_compile_options(motor_rv_hot INTERFACE
  $<$<CONFIG:Release>:-O3>
  # CODING_RULES §4: sem exceções no hot path. Fora dos sanitizers, cujo runtime precisa delas.
  $<$<AND:$<BOOL:${_sem_sanitizer}>,$<CONFIG:Release>>:-fno-exceptions>
)

if(MOTOR_RV_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE PARENT_SCOPE)
  else()
    message(WARNING "LTO pedido mas indisponível: ${_ipo_msg}")
  endif()
endif()

# ------------------------------------------------------------------ utilidades
# Declara um alvo de teste ligado ao GoogleTest e registrado no CTest, com rótulos.
# Rótulos usados pelos testPresets: `lento`, `concorrencia`, `caos`, `contrato`.
function(motor_rv_test nome)
  cmake_parse_arguments(T "" "" "FONTES;LIGA;ROTULOS" ${ARGN})
  add_executable(${nome} ${T_FONTES})
  target_link_libraries(${nome} PRIVATE motor_rv::flags ${T_LIGA} GTest::gtest GTest::gmock GTest::gtest_main)
  add_test(NAME ${nome} COMMAND ${nome})
  if(T_ROTULOS)
    set_tests_properties(${nome} PROPERTIES LABELS "${T_ROTULOS}")
  endif()
endfunction()
