cmake_minimum_required(VERSION 3.16)
project(abides_calculation_engine VERSION 0.1.0 LANGUAGES CXX)

set(
    CMAKE_EXPORT_COMPILE_COMMANDS ON
    CACHE BOOL "Export compile commands for IDE and language-server tooling"
    FORCE)

option(CALCULATION_ENGINE_BUILD_TESTS "Build calculation-engine tests" ON)
option(CALCULATION_ENGINE_BUILD_APPS "Build calculation-engine applications" ON)

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
set_target_properties(calculation_engine PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(calculation_engine PUBLIC abides::shadow_book)

if(MSVC)
    target_compile_options(calculation_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(calculation_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(CALCULATION_ENGINE_BUILD_APPS)
    add_executable(calculation_engine_demo apps/calculation_engine_main.cpp)
    target_link_libraries(calculation_engine_demo PRIVATE calculation_engine::calculation_engine)
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
endif()
