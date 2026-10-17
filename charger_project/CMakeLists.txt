cmake_minimum_required(VERSION 3.16)
project(charger_monitor_demo VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CHARGER_WERROR "Treat compiler warnings as errors" OFF)
option(CHARGER_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(CTest) # defines BUILD_TESTING
option(CHARGER_BUILD_TESTS "Build the automated tests" ${BUILD_TESTING})
option(CHARGER_BUILD_FUZZ "Build the libFuzzer harness (requires clang with -fsanitize=fuzzer)" OFF)
option(CHARGER_BUILD_TOOLS "Build the demo, settlement and benchmark tools" ON)

if(CHARGER_SANITIZERS)
    set(SANFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    set(SANLINK -fsanitize=address,undefined -no-pie)
endif()
if(CHARGER_WERROR)
    add_compile_options(-Werror)
endif()

add_library(charger_core
    src/charger_monitor.cpp
    src/charger_portfolio.cpp
    src/charger_ledger.cpp
    src/charger_config.cpp
)
target_include_directories(charger_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(charger_core PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
target_link_options(charger_core PRIVATE ${SANLINK})
# Export as charger::core so downstream projects link a readable name.
set_target_properties(charger_core PROPERTIES EXPORT_NAME core)

add_executable(charger_monitor src/main.cpp)
target_link_libraries(charger_monitor PRIVATE charger_core)
target_compile_options(charger_monitor PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
target_link_options(charger_monitor PRIVATE ${SANLINK})
target_compile_definitions(charger_monitor PRIVATE CHARGER_VERSION="${PROJECT_VERSION}")

if(CHARGER_BUILD_TESTS)
    add_executable(charger_tests tests/test_state_machine.cpp)
    target_link_libraries(charger_tests PRIVATE charger_core)
    target_compile_options(charger_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
    target_link_options(charger_tests PRIVATE ${SANLINK})
    target_compile_definitions(charger_tests PRIVATE CHARGER_VERSION="${PROJECT_VERSION}")
    add_test(NAME charger_state_machine COMMAND charger_tests)
endif()

if(CHARGER_BUILD_TOOLS)
    # These are also downstream consumers of charger::core, so they double as a
    # compile-time check that the public headers are self-contained.
    foreach(tool charger_demo charger_settle charger_bench)
        add_executable(${tool} tools/${tool}/main.cpp)
        target_link_libraries(${tool} PRIVATE charger_core)
        target_compile_options(${tool} PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
        target_link_options(${tool} PRIVATE ${SANLINK})
    endforeach()

    # The demo and the settlement tool must be reproducible in CI: they assert
    # exact figures, so a regression in the ledger fails the job.
    add_test(NAME charger_demo_runs COMMAND charger_demo --brief)
    add_test(NAME charger_settle_usage COMMAND charger_settle)
    set_tests_properties(charger_settle_usage PROPERTIES WILL_FAIL TRUE)
endif()

if(CHARGER_BUILD_FUZZ)
    add_executable(charger_fuzz tests/fuzz_can_replay.cpp)
    target_link_libraries(charger_fuzz PRIVATE charger_core)
    target_compile_options(charger_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-pie)
    target_link_options(charger_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -no-pie)
endif()

install(TARGETS charger_core
        EXPORT chargerTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS charger_monitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/charger_monitor.hpp include/charger_portfolio.hpp include/charger_ledger.hpp
        include/charger_config.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Downstream projects consume the library with find_package(charger_monitor).
install(EXPORT chargerTargets
        FILE chargerTargets.cmake
        NAMESPACE charger::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/charger_monitor)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/chargerMonitorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/charger_monitorConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/charger_monitorConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/charger_monitor)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/charger_monitorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/chargerMonitorConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/charger_monitor)

# pkg-config for non-CMake consumers.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/charger_monitor.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/charger_monitor.pc"
    @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/charger_monitor.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
