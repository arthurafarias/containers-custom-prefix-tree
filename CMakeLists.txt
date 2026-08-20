cmake_minimum_required(VERSION 3.20)

project(containers-custom-prefix-tree
    VERSION 1.0.0
    DESCRIPTION "Header-only STL-like PMR-aware prefix tree (trie) container, part of the containers::custom family"
    LANGUAGES CXX
)

option(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_TESTS
    "Build the containers-custom-prefix-tree test suite" ${PROJECT_IS_TOP_LEVEL})
option(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_EXAMPLES
    "Build the containers-custom-prefix-tree examples" ${PROJECT_IS_TOP_LEVEL})
option(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_BENCHMARKS
    "Build the containers-custom-prefix-tree benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(CONTAINERS_CUSTOM_PREFIX_TREE_COVERAGE
    "Instrument the test suite for coverage (gcc/clang --coverage)" OFF)

# Target and option names are fully qualified (rather than e.g. BUILD_TESTS)
# so that this project can be add_subdirectory()'d alongside sibling
# containers::custom libraries in one umbrella build without name clashes.
add_library(containers_custom_prefix_tree INTERFACE)
add_library(containers::custom::prefix_tree ALIAS containers_custom_prefix_tree)

target_include_directories(containers_custom_prefix_tree INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(containers_custom_prefix_tree INTERFACE cxx_std_23)

# The container's thread safety is built on std::recursive_mutex, which on
# some platforms (notably glibc-based Linux) requires linking against the
# threading library even though no thread is ever explicitly created here.
find_package(Threads REQUIRED)
target_link_libraries(containers_custom_prefix_tree INTERFACE Threads::Threads)

if(CONTAINERS_CUSTOM_PREFIX_TREE_COVERAGE)
    target_compile_options(containers_custom_prefix_tree INTERFACE --coverage -O0 -g)
    target_link_options(containers_custom_prefix_tree INTERFACE --coverage)
endif()

if(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CONTAINERS_CUSTOM_PREFIX_TREE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS containers_custom_prefix_tree EXPORT containers-custom-prefix-tree-targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT containers-custom-prefix-tree-targets
    FILE containers-custom-prefix-tree-targets.cmake
    NAMESPACE containers::custom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/containers-custom-prefix-tree
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/containers-custom-prefix-tree-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/containers-custom-prefix-tree-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/containers-custom-prefix-tree
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/containers-custom-prefix-tree-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/containers-custom-prefix-tree-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/containers-custom-prefix-tree-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/containers-custom-prefix-tree
)
