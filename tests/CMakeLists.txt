cmake_minimum_required(VERSION 3.15)
project(RE4TemporalProbeTests LANGUAGES CXX)

enable_testing()

add_executable(RE4TemporalProbeTests RE4TemporalProbeTests.cpp)
target_compile_features(RE4TemporalProbeTests PRIVATE cxx_std_23)
target_include_directories(RE4TemporalProbeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/mods")

add_test(NAME RE4TemporalProbeTests COMMAND RE4TemporalProbeTests)
