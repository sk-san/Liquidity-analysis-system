cmake_minimum_required(VERSION 3.16)
project(abides_shadow_book LANGUAGES CXX)

option(SHADOW_BOOK_BUILD_TESTS "Build shadow-book tests" ON)
option(SHADOW_BOOK_BUILD_EXAMPLES "Build shadow-book examples" ON)

add_library(abides_shadow_book
    src/shadow_book.cpp
)
add_library(abides::shadow_book ALIAS abides_shadow_book)

target_include_directories(abides_shadow_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(abides_shadow_book PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(abides_shadow_book PRIVATE /W4 /permissive-)
else()
    target_compile_options(abides_shadow_book PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(SHADOW_BOOK_BUILD_EXAMPLES)
    add_executable(shadow_book_demo examples/demo.cpp)
    target_link_libraries(shadow_book_demo PRIVATE abides::shadow_book)
endif()

if(SHADOW_BOOK_BUILD_TESTS)
    enable_testing()
    add_executable(shadow_book_test tests/shadow_book_test.cpp)
    target_link_libraries(shadow_book_test PRIVATE abides::shadow_book)
    add_test(NAME shadow_book_test COMMAND shadow_book_test)
endif()
