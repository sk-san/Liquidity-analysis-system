cmake_minimum_required(VERSION 3.16)
project(abides_calculation_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CALCULATION_ENGINE_BUILD_TESTS "Build calculation-engine tests" ON)
option(CALCULATION_ENGINE_BUILD_APPS "Build calculation-engine applications" ON)
option(
    CALCULATION_ENGINE_BUILD_SERVICE
    "Build the ZeroMQ calculation-engine stream service"
    ON)

# The previous shadow-book repository is retained as an internal library.
set(SHADOW_BOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SHADOW_BOOK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/shadow_book)

add_library(calculation_engine
    src/indicators/market_metrics.cpp
    src/engine/calculation_engine.cpp
)
add_library(calculation_engine::calculation_engine ALIAS calculation_engine)

target_include_directories(calculation_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(calculation_engine PUBLIC cxx_std_17)
target_link_libraries(calculation_engine PUBLIC abides::shadow_book)

if(MSVC)
    target_compile_options(calculation_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(calculation_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(CALCULATION_ENGINE_BUILD_SERVICE OR CALCULATION_ENGINE_BUILD_TESTS)
    find_package(ZLIB REQUIRED)

    add_library(calculation_engine_transport
        src/transport/market_data_decoder.cpp
    )
    add_library(
        calculation_engine::transport
        ALIAS calculation_engine_transport)
    target_include_directories(calculation_engine_transport
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(calculation_engine_transport PUBLIC cxx_std_17)
    target_link_libraries(
        calculation_engine_transport
        PUBLIC calculation_engine::calculation_engine
        PRIVATE ZLIB::ZLIB)

    if(MSVC)
        target_compile_options(
            calculation_engine_transport PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            calculation_engine_transport
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endif()

if(CALCULATION_ENGINE_BUILD_APPS)
    add_executable(calculation_engine_demo apps/calculation_engine_main.cpp)
    target_link_libraries(calculation_engine_demo PRIVATE calculation_engine::calculation_engine)
endif()

if(CALCULATION_ENGINE_BUILD_SERVICE)
    find_path(ZeroMQ_INCLUDE_DIR NAMES zmq.h)
    find_library(ZeroMQ_LIBRARY NAMES zmq libzmq)
    if(NOT ZeroMQ_INCLUDE_DIR OR NOT ZeroMQ_LIBRARY)
        message(
            FATAL_ERROR
            "ZeroMQ development files were not found. "
            "Install libzmq or configure with "
            "-DCALCULATION_ENGINE_BUILD_SERVICE=OFF.")
    endif()

    add_library(ZeroMQ::ZeroMQ UNKNOWN IMPORTED)
    set_target_properties(ZeroMQ::ZeroMQ PROPERTIES
        IMPORTED_LOCATION "${ZeroMQ_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZeroMQ_INCLUDE_DIR}")

    add_executable(
        calculation_engine_service
        apps/calculation_engine_service.cpp)
    target_link_libraries(
        calculation_engine_service
        PRIVATE calculation_engine::transport ZeroMQ::ZeroMQ)
    if(MSVC)
        target_compile_options(
            calculation_engine_service PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            calculation_engine_service
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endif()

if(CALCULATION_ENGINE_BUILD_TESTS)
    enable_testing()

    add_executable(shadow_book_test
        tests/order_book/shadow_book_facade_test.cpp
    )
    target_link_libraries(shadow_book_test PRIVATE calculation_engine::calculation_engine)
    add_test(NAME shadow_book_test COMMAND shadow_book_test)

    add_executable(market_metrics_test
        tests/indicators/market_metrics_test.cpp
    )
    target_link_libraries(market_metrics_test PRIVATE calculation_engine::calculation_engine)
    add_test(NAME market_metrics_test COMMAND market_metrics_test)

    add_executable(calculation_engine_test
        tests/integration/calculation_engine_test.cpp
    )
    target_link_libraries(calculation_engine_test PRIVATE calculation_engine::calculation_engine)
    add_test(NAME calculation_engine_test COMMAND calculation_engine_test)

    add_executable(market_data_decoder_test
        tests/transport/market_data_decoder_test.cpp
    )
    target_link_libraries(
        market_data_decoder_test
        PRIVATE calculation_engine::transport ZLIB::ZLIB)
    add_test(NAME market_data_decoder_test COMMAND market_data_decoder_test)
endif()
