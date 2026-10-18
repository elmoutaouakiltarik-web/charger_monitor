cmake_minimum_required(VERSION 3.16)
project(charger_monitor_demo VERSION 1.4.0 LANGUAGES CXX)

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
    src/charger_dbc.cpp
    src/charger_tou.cpp
    src/charger_export.cpp
    src/charger_reconcile.cpp
    src/charger_ocpp.cpp
    src/charger_candump.cpp
)
target_include_directories(charger_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(charger_core PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
# Static GCC runtime: an industrial host has exactly one DBC file and zero
# appetite for a wrong libstdc++ DLL on PATH deciding whether the journal
# survives. MinGW ships a mismatched libstdc++-6.dll in unexpected places.
target_link_options(charger_core PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
if(MINGW AND NOT SANLINK)
    # Downstream find_package() consumers inherit the same self-contained
    # binary contract: without it a consumer links libstdc++ dynamically and
    # silently fails to start (exit 127) on a host without that DLL in PATH.
    target_link_options(charger_core INTERFACE -static-libgcc -static-libstdc++)
endif()
# Export as charger::core so downstream projects link a readable name.
set_target_properties(charger_core PROPERTIES EXPORT_NAME core)

add_executable(charger_monitor src/main.cpp)
target_link_libraries(charger_monitor PRIVATE charger_core)
target_compile_options(charger_monitor PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
target_link_options(charger_monitor PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
target_compile_definitions(charger_monitor PRIVATE CHARGER_VERSION="${PROJECT_VERSION}")

if(CHARGER_BUILD_TESTS)
    add_executable(charger_tests tests/test_state_machine.cpp)
    target_link_libraries(charger_tests PRIVATE charger_core)
    target_compile_options(charger_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
    target_link_options(charger_tests PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
    target_compile_definitions(charger_tests PRIVATE CHARGER_VERSION="${PROJECT_VERSION}")
    add_test(NAME charger_state_machine COMMAND charger_tests)
    # The test reads tests/fixtures/vendor_bay.dbc, so pin the working directory.
    set_tests_properties(charger_state_machine PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(charger_export_tests tests/test_export.cpp)
    target_link_libraries(charger_export_tests PRIVATE charger_core)
    target_compile_options(charger_export_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
    target_link_options(charger_export_tests PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
    add_test(NAME charger_export COMMAND charger_export_tests)

    # Clean shutdown (INTERRUPTED sessions) and the candump reader/replay. The candump
    # test replays tests/fixtures/bay_session.candump in-process.
    foreach(suite shutdown candump)
        add_executable(charger_${suite}_tests tests/test_${suite}.cpp)
        target_link_libraries(charger_${suite}_tests PRIVATE charger_core)
        target_compile_options(charger_${suite}_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
        target_link_options(charger_${suite}_tests PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
    endforeach()
    add_test(NAME charger_shutdown COMMAND charger_shutdown_tests)
    add_test(NAME charger_candump COMMAND charger_candump_tests
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bay_session.candump)

    foreach(suite reconcile ocpp)
        add_executable(charger_${suite}_tests tests/test_${suite}.cpp)
        target_link_libraries(charger_${suite}_tests PRIVATE charger_core)
        target_compile_options(charger_${suite}_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
        target_link_options(charger_${suite}_tests PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
        add_test(NAME charger_${suite} COMMAND charger_${suite}_tests)
    endforeach()
endif()

if(CHARGER_BUILD_TOOLS)
    # These are also downstream consumers of charger::core, so they double as a
    # compile-time check that the public headers are self-contained.
    foreach(tool charger_demo charger_settle charger_bench charger_synth charger_probe)
        add_executable(${tool} tools/${tool}/main.cpp)
        target_link_libraries(${tool} PRIVATE charger_core)
        target_compile_options(${tool} PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
        target_link_options(${tool} PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
    endforeach()

    # The demo and the settlement tool must be reproducible in CI: they assert
    # exact figures, so a regression in the ledger fails the job.
    add_test(NAME charger_demo_runs COMMAND charger_demo --brief)
    add_test(NAME charger_settle_usage COMMAND charger_settle)
    set_tests_properties(charger_settle_usage PROPERTIES WILL_FAIL TRUE)

    if(CHARGER_BUILD_TESTS)
        # The commissioning analyser on the candump fixture: facts only, deterministic.
        add_test(NAME charger_probe COMMAND ${CMAKE_COMMAND}
                 -DPROBE=$<TARGET_FILE:charger_probe>
                 -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bay_session.candump
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/probe_check.cmake)
    endif()

    if(CHARGER_BUILD_TESTS)
        # Runs the built synthesiser, then verifies its journal in-process.
        add_executable(charger_synth_tests tests/test_synth.cpp)
        target_link_libraries(charger_synth_tests PRIVATE charger_core)
        target_compile_options(charger_synth_tests PRIVATE -Wall -Wextra -Wpedantic ${SANFLAGS})
        target_link_options(charger_synth_tests PRIVATE ${SANLINK} -static-libgcc -static-libstdc++)
        add_dependencies(charger_synth_tests charger_synth)
        add_test(NAME charger_synth COMMAND charger_synth_tests $<TARGET_FILE:charger_synth>)
    endif()
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
        include/charger_config.hpp include/charger_dbc.hpp include/charger_tou.hpp include/charger_export.hpp
        include/charger_candump.hpp include/charger_reconcile.hpp include/charger_ocpp.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

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
