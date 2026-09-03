# rv_codec — runtime SBE escrito à mão + codecs GERADOS a partir de schema/events.xml.
#
# O gerado vai para o diretório de build e nunca para o repositório (ADR-0006, ADR-0017).
# `DEPENDS` no XML e no gerador: mexeu em qualquer um dos dois, regera.

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated/codec)
set(SCHEMA  ${CMAKE_SOURCE_DIR}/schema/events.xml)
set(GERADOR ${CMAKE_SOURCE_DIR}/scripts/sbe_gen.py)

set(GERADOS
  ${GEN_DIR}/template_ids.hpp
  ${GEN_DIR}/events.hpp
  ${GEN_DIR}/events_decode.hpp
  ${GEN_DIR}/events_debug.cpp
)

add_custom_command(
  OUTPUT ${GERADOS}
  COMMAND ${Python3_EXECUTABLE} ${GERADOR} ${SCHEMA} -o ${GEN_DIR}
  DEPENDS ${SCHEMA} ${GERADOR}
  COMMENT "sbe_gen: gerando codecs de schema/events.xml"
  VERBATIM
)
add_custom_target(rv_codec_gen DEPENDS ${GERADOS})

add_library(rv_codec STATIC ${GEN_DIR}/events_debug.cpp)
add_library(rv::codec ALIAS rv_codec)
add_dependencies(rv_codec rv_codec_gen)

# O gerado é incluído como "codec/events.hpp": o diretório PAI do gerado entra no caminho de
# include, e não o diretório em si. Assim o caminho no `#include` é o mesmo para gerado e escrito
# à mão, e mover um arquivo de um lado para o outro não muda nenhuma linha de include.
target_include_directories(rv_codec PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(rv_codec PUBLIC rv::base)
target_link_libraries(rv_codec PRIVATE motor_rv::hot)
