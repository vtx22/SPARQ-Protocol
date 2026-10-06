cmake_minimum_required(VERSION 3.22)

project(SPARQ-Protocol
        VERSION 2.1.1
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(SPARQ-Protocol INTERFACE)

target_sources(SPARQ-Protocol
        INTERFACE
        include/sparq/protocol.hpp
        include/sparq/encoder.hpp
        include/sparq/decoder.hpp
)

target_include_directories(SPARQ-Protocol
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(SPARQ-Protocol
        INTERFACE
        cxx_std_23
)


include(CTest)

if (BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
    )

    FetchContent_MakeAvailable(Catch2)

    add_executable(decode_encode_test
            tests/decode_encode_test.cpp
    )

    target_link_libraries(decode_encode_test
            PRIVATE
            SPARQ-Protocol
            Catch2::Catch2WithMain
    )

    include(Catch)

    catch_discover_tests(decode_encode_test)
endif ()