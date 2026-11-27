# Genome engine module (Piranha Bytes): ELEX, ELEX 2, Risen 3; Risen through its own route (genome_risen1.cpp).
add_library(gyro_genome OBJECT genome_engine.cpp genome_risen1.cpp)
target_include_directories(gyro_genome PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_genome PRIVATE gyro_options gyro_core)
