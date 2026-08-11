cmake_minimum_required(VERSION 3.16)
project(liquidity_analysis_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
    CMAKE_EXPORT_COMPILE_COMMANDS ON
    CACHE BOOL "Export compile commands for IDE and language-server tooling"
    FORCE)

# The calculation engine is maintained in one canonical component tree. Keep
# this root project as a thin entry point for repository-level presets and CI.
enable_testing()

add_subdirectory(
    components/calculation-engine
    calculation-engine)
