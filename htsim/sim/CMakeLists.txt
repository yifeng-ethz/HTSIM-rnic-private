cmake_minimum_required(VERSION 3.16)
include(FetchContent)

project(htsim_project LANGUAGES CXX)
option(ENABLE_TESTS "enable the test suite" OFF)
option(HTSIM_ENABLE_SIMLLM_RNIC
       "build the composed SimLLM RNIC runtime and Tier A producer"
       OFF)
set(SIMLLM_REPOSITORY_ROOT "" CACHE PATH
    "path to a SimLLM checkout that owns the versioned RNIC ABI")

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensures GNU extensions are disabled
set(CMAKE_CXX_FLAGS_RELEASE -O3) # Prevent -DNDEBUG, htsim requires 
                                 # asserts to be enabled.
# Set compiler flags
add_compile_options(
    -Wall
    -g
    -Wsign-compare
    -Wuninitialized
    -fPIE
    -Wno-deprecated
    -O3
)
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
    logsim-interface.cpp
    compute_event.cpp
    null_event.cpp
    atlahs_htsim_api.cpp
)

# Create static library
add_library(htsim STATIC ${SOURCE_FILES})
target_include_directories(htsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) # Needed for header-only libraries (e.g., loggertypes.h).

if(HTSIM_ENABLE_SIMLLM_RNIC)
    if(SIMLLM_REPOSITORY_ROOT STREQUAL "")
        message(FATAL_ERROR
            "HTSIM_ENABLE_SIMLLM_RNIC requires SIMLLM_REPOSITORY_ROOT")
    endif()
    get_filename_component(
        SIMLLM_REPOSITORY_ROOT
        "${SIMLLM_REPOSITORY_ROOT}"
        ABSOLUTE)
    set(SIMLLM_RNIC_SOURCE_DIR
        "${SIMLLM_REPOSITORY_ROOT}/simllm/backends/rnic")
    set(SIMLLM_TIER_A_NATIVE_DIR
        "${SIMLLM_REPOSITORY_ROOT}/examples/rnic_live_v1/native")
    foreach(REQUIRED_SIMLLM_SOURCE IN ITEMS
            "${SIMLLM_RNIC_SOURCE_DIR}/CMakeLists.txt"
            "${SIMLLM_TIER_A_NATIVE_DIR}/tier_a_port_factory.h"
            "${SIMLLM_TIER_A_NATIVE_DIR}/tier_a_producer.cpp")
        if(NOT EXISTS "${REQUIRED_SIMLLM_SOURCE}")
            message(FATAL_ERROR
                "required SimLLM source is missing: ${REQUIRED_SIMLLM_SOURCE}")
        endif()
    endforeach()

    set(SIMLLM_RNIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SIMLLM_RNIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(SIMLLM_RNIC_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
    add_subdirectory(
        "${SIMLLM_RNIC_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/simllm-rnic")

    target_sources(htsim PRIVATE
        simllm_atlahs_flow_runtime.cpp
        simllm_htsim_network_port.cpp)
    target_link_libraries(htsim PUBLIC simllm::rnic)

    add_executable(htsim_rnic_tier_a
        simllm_htsim_tier_a_port_factory.cpp
        "${SIMLLM_TIER_A_NATIVE_DIR}/tier_a_producer.cpp")
    target_include_directories(htsim_rnic_tier_a PRIVATE
        "${SIMLLM_TIER_A_NATIVE_DIR}")
    target_link_libraries(htsim_rnic_tier_a PRIVATE htsim simllm::rnic)
    target_compile_features(htsim_rnic_tier_a PRIVATE cxx_std_17)
endif()

# Add subdirectories
add_subdirectory(datacenter)

# Define executables and link with htsim library
add_executable(parse_output parse_output.cpp)
target_link_libraries(parse_output PRIVATE htsim)

if ( ENABLE_TESTS )
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

    if(HTSIM_ENABLE_SIMLLM_RNIC)
        list(APPEND UNIT_TEST_FILES
            simllm_atlahs_flow_runtime_test
            simllm_htsim_network_port_test)
    endif()

    foreach(UNIT_TEST_FILE ${UNIT_TEST_FILES}) 
        add_executable(${UNIT_TEST_FILE} ${UNIT_TEST_FILE}.cpp)
        target_link_libraries(${UNIT_TEST_FILE} PRIVATE htsim gtest_main gmock_main)
        gtest_discover_tests(${UNIT_TEST_FILE}
            PROPERTIES LABELS "unit"
            PROPERTIES TIMEOUT 60
        )
    endforeach()

    if(HTSIM_ENABLE_SIMLLM_RNIC)
        target_link_libraries(
            simllm_atlahs_flow_runtime_test PRIVATE htsim_dc htsim)
    endif()

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
