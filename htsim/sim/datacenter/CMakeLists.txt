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
    rnic_atlahs_cli.cpp
    rnic_atlahs_driver.cpp
    rnic_atlahs_runtime_factory.cpp
    rnic_collective_network_runtime.cpp
    rnic_collective_route.cpp
    star_topology.cpp
    fat_tree_switch.cpp
    fat_tree_switch_factory.cpp
    fat_tree_switch_model.cpp
    ns_tm3_switch.cpp
    ns_rosetta_switch.cpp
    dragon_fly_topology.cpp
    slimfly_topology.cpp
    slimfly_switch.cpp
    dragonfly_topology.cpp
    dragonfly_switch.cpp
    generic_topology.cpp
)

# Create static library
add_library(htsim_dc STATIC ${LIB_SOURCES})
target_include_directories(htsim_dc PUBLIC ${PROJECT_SOURCE_DIR}) 
target_include_directories(htsim_dc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) 

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

add_executable(htsim_uec main_uec.cpp)
add_executable(htsim_uec_sf main_uec_sf.cpp)
add_executable(htsim_uec_df main_uec_df.cpp)
add_executable(htsim_rnic main_rnic.cpp)

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
    htsim_uec
    htsim_uec_sf
    htsim_uec_df
    htsim_rnic
)
foreach(EXECUTABLE ${ALL_EXECUTABLES})
    # Link each executable with the 'htsim' library defined in the parent directory.
    target_link_libraries(${EXECUTABLE} PRIVATE htsim_dc)
    target_link_libraries(${EXECUTABLE} PRIVATE htsim)
    # Add a post-build command to explicitly remove and recreate the symlink for all previous executables.
    add_symlink_target(${EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

if ( ENABLE_TESTS )
    add_executable(fat_tree_topology_cfg_test fat_tree_topology_cfg_test.cpp)
    target_link_libraries(fat_tree_topology_cfg_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(fat_tree_topology_cfg_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

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

    add_executable(ns_rosetta_switch_test ns_rosetta_switch_test.cpp)
    target_link_libraries(
        ns_rosetta_switch_test PRIVATE htsim_dc htsim gtest_main)
    gtest_discover_tests(ns_rosetta_switch_test
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
endif()
