cmake_minimum_required(VERSION 3.22)

project(SPARQ-Protocol
        VERSION 1.0.0
        LANGUAGES CXX
)


add_library(SPARQ-Protocol INTERFACE)

target_sources(SPARQ-Protocol
        INTERFACE
        include/sparq/protocol.hpp
        include/sparq/encoder.hpp
        include/sparq/decoder.hpp
)

target_include_directories(SPARQ-Protocol
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/sparq
)

target_compile_features(SPARQ-Protocol
        INTERFACE
        cxx_std_23
)

enable_testing()
add_executable(sparq_protocol_smoke tests/smoke.cpp)
target_link_libraries(sparq_protocol_smoke PRIVATE SPARQ-Protocol)