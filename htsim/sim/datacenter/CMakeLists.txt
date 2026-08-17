# List of common source files shared among executables.
set(LIB_SOURCES
    firstfit.cpp
    vl2_topology.cpp
    fat_tree_topology.cpp
    bcube_topology.cpp
    connection_matrix.cpp
    oversubscribed_fat_tree_topology.cpp
    shortflows.cpp
    multihomed_fat_tree_topology.cpp
    atlahs_goodput_trace.cpp
    atlahs_state_trace.cpp
    dcqcn_atlahs_cli.cpp
    dcqcn_atlahs_runtime.cpp
    rnic_atlahs_cli.cpp
    rnic_atlahs_driver.cpp
    rnic_atlahs_runtime_factory.cpp
    rnic_collective_network_runtime.cpp
    rnic_collective_route.cpp
    rnic_ss_packet.cpp
    rnic_ss_route.cpp
    rnic_ss_runtime.cpp
    star_topology.cpp
    fat_tree_switch.cpp
    fat_tree_switch_factory.cpp
    fat_tree_switch_model.cpp
    ns_tm3_dcqcn_policy.cpp
    ns_tm3_switch.cpp
    ns_rosetta_switch.cpp
    dragon_fly_topology.cpp
    slimfly_topology.cpp
    slimfly_switch.cpp
    dragonfly_topology.cpp
    dragonfly_switch.cpp
    dragonfly_endpoint_placement.cpp
    dragonfly_progressive_congestion.cpp
    dragonfly_progressive_routing.cpp
    dragonfly_route_state_store.cpp
    generic_topology.cpp
)

# Create static library
add_library(htsim_dc STATIC ${LIB_SOURCES})
target_include_directories(htsim_dc PUBLIC ${PROJECT_SOURCE_DIR}) 
target_include_directories(htsim_dc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) 
if(HTSIM_ENABLE_SIMLLM_RNIC)
    target_sources(htsim_dc PRIVATE rnic_flow_session.cpp)
    target_link_libraries(htsim_dc PRIVATE htsim simllm::rnic)
endif()

# -------------------------------
# Executable Definitions
# -------------------------------
# List of Executables and Their Main Source Files.
add_executable(htsim_tcp main_tcp.cpp)
set_property(TARGET htsim_tcp PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_ndp main_ndp.cpp)
set_property(TARGET htsim_ndp PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_roce main_roce.cpp)
set_property(TARGET htsim_roce PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_swift main_swift.cpp)
set_property(TARGET htsim_swift PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_hpcc main_hpcc.cpp)
set_property(TARGET htsim_hpcc PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_eqds main_eqds.cpp)
set_property(TARGET htsim_eqds PROPERTY EXCLUDE_FROM_ALL TRUE)

add_executable(htsim_dcqcn_atlahs main_dcqcn_atlahs.cpp)
add_executable(htsim_rnic main_rnic.cpp)
add_executable(htsim_uec main_uec.cpp)
add_executable(htsim_uec_sf main_uec_sf.cpp)
add_executable(htsim_uec_df main_uec_df.cpp)

if(HTSIM_ENABLE_SIMLLM_RNIC)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/../.."
        OUTPUT_VARIABLE HTSIM_COMPOSED_SOURCE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${SIMLLM_REPOSITORY_ROOT}"
        OUTPUT_VARIABLE SIMLLM_COMPOSED_SOURCE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(HTSIM_COMPOSED_SOURCE_REVISION STREQUAL "")
        set(HTSIM_COMPOSED_SOURCE_REVISION "unknown")
    endif()
    if(SIMLLM_COMPOSED_SOURCE_REVISION STREQUAL "")
        set(SIMLLM_COMPOSED_SOURCE_REVISION "unknown")
    endif()
    target_compile_definitions(htsim_rnic PRIVATE
        HTSIM_ENABLE_SIMLLM_RNIC=1
        HTSIM_COMPOSED_SOURCE_REVISION="${HTSIM_COMPOSED_SOURCE_REVISION}"
        SIMLLM_COMPOSED_SOURCE_REVISION="${SIMLLM_COMPOSED_SOURCE_REVISION}")
endif()

# -------------------------------
# Linking Executables with Libraries
# -------------------------------
set (ALL_EXECUTABLES
    htsim_tcp
    htsim_ndp
    htsim_roce
    htsim_swift
    htsim_hpcc
    htsim_eqds
    htsim_dcqcn_atlahs
    htsim_rnic
    htsim_uec
    htsim_uec_sf
    htsim_uec_df
)
foreach(EXECUTABLE ${ALL_EXECUTABLES})
    # Link each executable with the 'htsim' library defined in the parent directory.
    target_link_libraries(${EXECUTABLE} PRIVATE htsim_dc)
    target_link_libraries(${EXECUTABLE} PRIVATE htsim)
    # Add a post-build command to explicitly remove and recreate the symlink for all previous executables.
    add_symlink_target(${EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

if ( ENABLE_TESTS )
    add_executable(fat_tree_switch_factory_test fat_tree_switch_factory_test.cpp)
    target_link_libraries(fat_tree_switch_factory_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(fat_tree_switch_factory_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(ns_tm3_switch_test ns_tm3_switch_test.cpp)
    target_link_libraries(ns_tm3_switch_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(ns_tm3_switch_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        ns_tm3_dcqcn_policy_test
        ns_tm3_dcqcn_policy_test.cpp)
    target_link_libraries(
        ns_tm3_dcqcn_policy_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(ns_tm3_dcqcn_policy_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(ns_rosetta_switch_test ns_rosetta_switch_test.cpp)
    target_link_libraries(
        ns_rosetta_switch_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(ns_rosetta_switch_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        dragonfly_progressive_routing_test
        dragonfly_progressive_routing_test.cpp)
    target_link_libraries(
        dragonfly_progressive_routing_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dragonfly_progressive_routing_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        dragonfly_progressive_congestion_test
        dragonfly_progressive_congestion_test.cpp)
    target_link_libraries(
        dragonfly_progressive_congestion_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dragonfly_progressive_congestion_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        dragonfly_route_state_store_test
        dragonfly_route_state_store_test.cpp)
    target_link_libraries(
        dragonfly_route_state_store_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dragonfly_route_state_store_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        dragonfly_endpoint_placement_test
        dragonfly_endpoint_placement_test.cpp)
    target_link_libraries(
        dragonfly_endpoint_placement_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dragonfly_endpoint_placement_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(rnic_collective_route_test rnic_collective_route_test.cpp)
    target_link_libraries(
        rnic_collective_route_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_collective_route_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        rnic_collective_network_runtime_test
        rnic_collective_network_runtime_test.cpp)
    target_link_libraries(
        rnic_collective_network_runtime_test
        PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_collective_network_runtime_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(atlahs_goodput_trace_test atlahs_goodput_trace_test.cpp)
    target_link_libraries(
        atlahs_goodput_trace_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(atlahs_goodput_trace_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(dcqcn_atlahs_cli_test dcqcn_atlahs_cli_test.cpp)
    target_link_libraries(
        dcqcn_atlahs_cli_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dcqcn_atlahs_cli_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        dcqcn_atlahs_runtime_test
        dcqcn_atlahs_runtime_test.cpp)
    target_link_libraries(
        dcqcn_atlahs_runtime_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(dcqcn_atlahs_runtime_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(rnic_atlahs_cli_test rnic_atlahs_cli_test.cpp)
    target_link_libraries(
        rnic_atlahs_cli_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_atlahs_cli_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        rnic_atlahs_runtime_factory_test
        rnic_atlahs_runtime_factory_test.cpp)
    target_link_libraries(
        rnic_atlahs_runtime_factory_test
        PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_atlahs_runtime_factory_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(rnic_atlahs_driver_test rnic_atlahs_driver_test.cpp)
    target_link_libraries(
        rnic_atlahs_driver_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_atlahs_driver_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    add_executable(rnic_ss_runtime_test rnic_ss_runtime_test.cpp)
    target_link_libraries(
        rnic_ss_runtime_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(rnic_ss_runtime_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    if(HTSIM_ENABLE_SIMLLM_RNIC)
        add_executable(rnic_flow_session_test rnic_flow_session_test.cpp)
        target_link_libraries(
            rnic_flow_session_test
            PRIVATE htsim_dc htsim simllm::rnic gtest_main)
        gtest_discover_tests(rnic_flow_session_test
            PROPERTIES LABELS "unit;session"
            PROPERTIES TIMEOUT 60
        )
    endif()
endif()
