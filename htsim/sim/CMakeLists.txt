cmake_minimum_required(VERSION 3.16)
include(FetchContent)

project(htsim_project LANGUAGES CXX)
option(ENABLE_TESTS "enable the test suite" OFF)
if(WIN32)
    option(HTSIM_CREATE_SOURCE_SYMLINKS
        "create source-tree symlinks for built executables" OFF)
else()
    option(HTSIM_CREATE_SOURCE_SYMLINKS
        "create source-tree symlinks for built executables" ON)
endif()

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensures GNU extensions are disabled
# Prevent -DNDEBUG: htsim requires assertions to remain enabled. Compiler
# options must be selected per toolchain because MSVC does not understand
# GCC-style flags.
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2")
    add_compile_options(/W4 /EHsc /permissive-)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")
    add_compile_options(
        -Wall
        -g
        -Wsign-compare
        -Wuninitialized
        -fPIE
        -Wno-deprecated
        -O3
    )
endif()

# MinGW executables otherwise depend on whichever libstdc++/libgcc DLLs
# happen to appear first on PATH, which is especially fragile on developer
# workstations with several GCC distributions installed.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()
# Optionally enable sanitizers
# -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined"

if ( ENABLE_TESTS ) 
    # Fetch GoogleTest
    FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
    enable_testing()
endif()

# Function to add a symlink creation step for an executable. 
# Used to create a symlink in the source directory to the executable 
# in the build directory for easier access and backward compatibility.
function(add_symlink_target EXECUTABLE TARGET_DIR)
    if(NOT HTSIM_CREATE_SOURCE_SYMLINKS)
        return()
    endif()
    add_custom_command(
        TARGET ${EXECUTABLE}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove ${TARGET_DIR}/${EXECUTABLE} # Remove existing symlink
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE:${EXECUTABLE}>  # Path to the executable
            ${TARGET_DIR}/${EXECUTABLE}  # Path for the symlink
        COMMENT "Removing and recreating symlink for ${EXECUTABLE} in ${TARGET_DIR}"
    )
endfunction()

# Define source files
set(SOURCE_FILES
    aeolusqueue.cpp
    atlahs_wqe.cpp
    callback_pipe.cpp
    cbr.cpp
    cbrpacket.cpp
    clock.cpp
    cnppacket.cpp
    compositeprioqueue.cpp
    compositequeue.cpp
    config.cpp
    cpqueue.cpp
    dcqcn.cpp
    dcqcn_logger.cpp
    dctcp.cpp
    dctcp_transfer.cpp
    ecnprioqueue.cpp
    ecnqueue.cpp
    eqds.cpp
    eqds_logger.cpp
    eqdspacket.cpp
    eth_pause_packet.cpp
    eventlist.cpp
    exoqueue.cpp
    fairpullqueue.cpp
    hpcc.cpp
    hpccpacket.cpp
    logfile.cpp
    loggers.cpp
    metric.cpp
    data_collector.cpp
    meter.cpp
    mtcp.cpp
    ndp.cpp
    ndp_transfer.cpp
    ndppacket.cpp
    ndptunnel.cpp
    ndptunnelpacket.cpp
    network.cpp
    oversubscribed_cc.cpp
    pciemodel.cpp
    pipe.cpp
    priopullqueue.cpp
    prioqueue.cpp
    qcn.cpp
    queue.cpp
    queue_lossless.cpp
    queue_lossless_input.cpp
    queue_lossless_output.cpp
    randomqueue.cpp
    rnic_collective_control.cpp
    rnic_collective_packet.cpp
    rnic_fluid_manifold.cpp
    rnic_fluid_manifold_runtime.cpp
    rnic_max_min_allocator.cpp
    rnic_packetized_manifold.cpp
    rnic_packetized_manifold_runtime.cpp
    rnic_port.cpp
    rnic_prbs_pacer.cpp
    rnic_profile.cpp
    rnic_ring_cam.cpp
    rng.cpp
    roce.cpp
    rocepacket.cpp
    buffer_reps.cpp
    route.cpp
    routetable.cpp
    sent_packets.cpp
    strack.cpp
    strackpacket.cpp
    swift.cpp
    swift_scheduler.cpp
    swiftpacket.cpp
    switch.cpp
    tcp.cpp
    tcp_periodic.cpp
    tcp_transfer.cpp
    tcppacket.cpp
    trigger.cpp
    uec.cpp
    uec_logger.cpp
    uecpacket.cpp
    uec_mp.cpp
    uec_pdcses.cpp
    paper_uec.cpp
    paper_uecpacket.cpp
    logsim-interface.cpp
    compute_event.cpp
    null_event.cpp
    atlahs_htsim_api.cpp
)

# Create static library
add_library(htsim STATIC ${SOURCE_FILES})
target_include_directories(htsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) # Needed for header-only libraries (e.g., loggertypes.h).

# Add subdirectories
add_subdirectory(datacenter)

# Define executables and link with htsim library
add_executable(parse_output parse_output.cpp)
target_link_libraries(parse_output PRIVATE htsim)

# Build the GOAL text-to-binary converter from the checked-in generated
# source. This avoids requiring re2c/gengetopt for ordinary builds and gives
# Windows callers a native txt2bin.exe from the same source as Linux.
add_executable(txt2bin lgs/txt2bin.cpp)
target_include_directories(txt2bin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lgs)

if ( ENABLE_TESTS )
    # Cross-platform behavioral and structural regression checks for the
    # CMake-built GOAL converter and binary parser.
    file(SHA256
        ${CMAKE_CURRENT_SOURCE_DIR}/lgs/txt2bin
        HTSIM_SOURCE_TXT2BIN_SHA256)

    add_test(
        NAME PortabilityRegression.Txt2BinSourceArtifactUnchanged
        COMMAND ${CMAKE_COMMAND}
            "-DCASE=source_txt2bin"
            "-DSOURCE_TXT2BIN=${CMAKE_CURRENT_SOURCE_DIR}/lgs/txt2bin"
            "-DEXPECTED_SHA256=${HTSIM_SOURCE_TXT2BIN_SHA256}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/portability_regression.cmake)
    set_tests_properties(
        PortabilityRegression.Txt2BinSourceArtifactUnchanged
        PROPERTIES LABELS "portability;structural" TIMEOUT 10)

    add_test(
        NAME PortabilityRegression.BinaryGoalRoundTrip26Ranks
        COMMAND ${CMAKE_COMMAND}
            "-DCASE=rank_26_round_trip"
            "-DTXT2BIN=$<TARGET_FILE:txt2bin>"
            "-DHTSIM_RNIC=$<TARGET_FILE:htsim_rnic>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/portability/$<CONFIG>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/portability_regression.cmake)
    set_tests_properties(
        PortabilityRegression.BinaryGoalRoundTrip26Ranks
        PROPERTIES LABELS "portability;behavioral" TIMEOUT 60)

    # Unit tests.
    set(UNIT_TEST_FILES
        atlahs_wqe_test
        dcqcn_test
        eventlist_microphase_test
        pipe_test
        rnic_collective_control_test
        rnic_collective_packet_test
        rnic_fluid_manifold_test
        rnic_fluid_manifold_runtime_test
        rnic_max_min_allocator_test
        rnic_packetized_manifold_test
        rnic_packetized_manifold_runtime_test
        rnic_port_test
        rnic_prbs_pacer_test
        rnic_profile_test
        rnic_ring_cam_test
    )

    foreach(UNIT_TEST_FILE ${UNIT_TEST_FILES}) 
        add_executable(${UNIT_TEST_FILE} ${UNIT_TEST_FILE}.cpp)
        target_link_libraries(${UNIT_TEST_FILE} PRIVATE htsim gtest_main gmock_main)
        gtest_discover_tests(${UNIT_TEST_FILE}
            PROPERTIES LABELS "unit"
            PROPERTIES TIMEOUT 60
        )
    endforeach()

    # AtlahsHtsimApi retains its legacy Clos/UEC path alongside the injected
    # runtime seam, so this focused test also needs the datacenter definitions.
    # The two static archives reference each other (api -> topology -> queues),
    # so htsim appears on both sides of htsim_dc for single-pass resolution.
    add_executable(atlahs_flow_runtime_test atlahs_flow_runtime_test.cpp)
    target_link_libraries(
        atlahs_flow_runtime_test PRIVATE htsim htsim_dc htsim gtest_main gmock_main)
    gtest_discover_tests(atlahs_flow_runtime_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
endif()

# Always print ENABLE_TESTS status, so folks don't miss it.
add_custom_target(
  always_run ALL
  COMMAND ${CMAKE_COMMAND} -E echo "ENABLE_TESTS=${ENABLE_TESTS}"
)
