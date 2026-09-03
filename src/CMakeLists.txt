# Uma linha por camada. Nenhuma flag mora aqui — todas em cmake/MotorRvFlags.cmake.
# O CMakeLists de cada camada é de UM dono; é assim que dois agentes nunca abrem o mesmo arquivo.
add_subdirectory(base)
add_subdirectory(format)
add_subdirectory(codec)
add_subdirectory(core)
add_subdirectory(wal)
