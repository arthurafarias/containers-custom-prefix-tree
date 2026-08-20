include(FetchContent)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(containers_custom_prefix_tree_benchmarks
    bench_prefix_tree.cpp
)

target_link_libraries(containers_custom_prefix_tree_benchmarks PRIVATE
    containers::custom::prefix_tree
    benchmark::benchmark_main
)
target_compile_options(containers_custom_prefix_tree_benchmarks PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
