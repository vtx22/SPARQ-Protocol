cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(SPARQ-Protocol
        VERSION 1.0.0
        LANGUAGES CXX
)


add_library(SPARQ-Protocol INTERFACE)

target_sources(SPARQ-Protocol
        INTERFACE
        include/sparq/protocol.hpp
)

target_include_directories(SPARQ-Protocol
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/sparq
)

target_compile_features(SPARQ-Protocol
        INTERFACE
        cxx_std_20
)

enable_testing()
add_executable(sparq_protocol_smoke tests/smoke.cpp)
target_link_libraries(sparq_protocol_smoke PRIVATE SPARQ-Protocol)