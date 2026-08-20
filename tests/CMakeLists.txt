include(FetchContent)

# doctest v2.4.11's own CMakeLists.txt predates CMake's removal of
# compatibility with cmake_minimum_required < 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# FetchContent_Declare/MakeAvailable are safe to call again for the same
# dependency name from a sibling containers::custom project's subdirectory:
# CMake tracks population globally and only sets doctest_SOURCE_DIR in this
# scope, it does not re-fetch or re-add the target.
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(containers_custom_prefix_tree_tests
    test_prefix_tree.cpp
)

target_link_libraries(containers_custom_prefix_tree_tests PRIVATE
    containers::custom::prefix_tree
    doctest::doctest
)
target_compile_options(containers_custom_prefix_tree_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(containers_custom_prefix_tree_tests)
