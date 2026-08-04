#include <gtest/gtest.h>

#include "dcqcn.h"
#include "dcqcn_atlahs_runtime.h"
#include "eventlist.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

TEST(DcqcnAtlahsRuntimeTest, CompletesPacketizedFlowOnTheSharedNsTm3Clos) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    const std::filesystem::path topology =
        std::filesystem::path(__FILE__).parent_path() /
        "../../../experiments/rnic_multibaseline/topologies/clos_64_400g.topo";
    DcqcnAtlahsRuntimeConfig config;
    config.topology_file = topology.lexically_normal().string();
    config.ns_tm3_shared_buffer_bytes = 1024 * 1024;
    config.ns_tm3_egress_buffer_bytes = 1024 * 1024;
    config.ecn_kmin_bytes = 0;
    config.ecn_kmax_bytes = 4096;
    config.ecn_pmax_ppm = 1000000;
    config.ecn_seed = 9;
    config.pfc_low_threshold_bytes = 4096;
    config.pfc_high_threshold_bytes = 8192;
    const std::filesystem::path state_trace =
        std::filesystem::temp_directory_path() /
        ("dcqcn-atlahs-state-" + std::to_string(reinterpret_cast<std::uintptr_t>(&event_list)) +
         ".csv");
    std::filesystem::remove(state_trace);
    std::filesystem::remove(state_trace.string() + ".tmp");
    config.state_trace_csv = state_trace.string();
    const std::filesystem::path goodput_trace =
        std::filesystem::temp_directory_path() /
        ("dcqcn-atlahs-goodput-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&event_list)) + ".csv");
    std::filesystem::remove(goodput_trace);
    std::filesystem::remove(goodput_trace.string() + ".tmp");
    config.goodput_trace_csv = goodput_trace.string();
    config.goodput_trace_bin_ps = 10000000;
    DcqcnAtlahsRuntime runtime(event_list, config, 64);
    EXPECT_EQ(DCQCNSrc::minRate(), config.dcqcn_min_rate_bps);
    std::vector<AtlahsFlowId> completed;
    runtime.setup(64, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    for (std::uint32_t source = 0; source < 8; ++source) {
        runtime.send(AtlahsFlowRequest{77 + source, source, 63, 64 * 1024, EventList::now(), 9});
    }
    while (runtime.hasPendingPhysicalWork()) {
        ASSERT_TRUE(EventList::doNextEvent());
    }

    EXPECT_EQ(completed.size(), 8U);
    EXPECT_EQ(runtime.completed_flow_count(), 8U);
    EXPECT_EQ(runtime.silent_rto_count(), 0U);
    EXPECT_EQ(runtime.dropped_packet_count(), 0U);
    EXPECT_EQ(runtime.shared_pool_dropped_packet_count(), 0U);
    EXPECT_EQ(runtime.egress_domain_dropped_packet_count(), 0U);
    EXPECT_GT(runtime.ecn_marked_packet_count(), 0U);
    EXPECT_GT(runtime.pfc_pause_count(), 0U);
    EXPECT_EQ(runtime.pfc_pause_count(), runtime.pfc_resume_count());
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());
    EXPECT_GT(runtime.state_trace_row_count(), 16U);
    EXPECT_GT(runtime.goodput_trace_row_count(), 0U);
    runtime.writeStateTraceCsv();
    runtime.writeGoodputTraceCsv();
    EXPECT_TRUE(std::filesystem::is_regular_file(state_trace));
    EXPECT_FALSE(std::filesystem::exists(state_trace.string() + ".tmp"));
    std::ifstream trace_input(state_trace);
    const std::string trace_text((std::istreambuf_iterator<char>(trace_input)),
                                 std::istreambuf_iterator<char>());
    EXPECT_NE(trace_text.find("time_ps,flow_id,source,destination,event,"), std::string::npos);
    EXPECT_NE(trace_text.find(",flow-start,"), std::string::npos);
    EXPECT_NE(trace_text.find(",completion,"), std::string::npos);
    EXPECT_NE(trace_text.find(",pause,"), std::string::npos);
    EXPECT_NE(trace_text.find(",resume,"), std::string::npos);
    std::ifstream goodput_input(goodput_trace);
    const std::string goodput_text((std::istreambuf_iterator<char>(goodput_input)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(goodput_text.find("bin_start_ps,bin_end_ps,flow_id,source,destination,"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(goodput_trace.string() + ".tmp"));
    std::filesystem::remove(state_trace);
    std::filesystem::remove(goodput_trace);
    const std::string manifest =
        renderDcqcnAtlahsManifest(config, 64, "flat.bin", "flows.csv", "state.csv", "gpu-rank");
    EXPECT_NE(manifest.find("profile=dcqcn"), std::string::npos);
    EXPECT_NE(manifest.find("switch=ns-tm3"), std::string::npos);
    EXPECT_NE(manifest.find("state_trace_csv=state.csv"), std::string::npos);
    EXPECT_NE(manifest.find("goodput_trace_bin_ps=10000000"), std::string::npos);
    EXPECT_NE(manifest.find("shared_buffer_bytes=1048576"), std::string::npos);
    EXPECT_NE(manifest.find("shared_buffer_scope=switch-wide"), std::string::npos);
    EXPECT_NE(manifest.find("egress_buffer_bytes=1048576"), std::string::npos);
    EXPECT_NE(manifest.find("egress_buffer_scope=per-physical-egress"), std::string::npos);
    EXPECT_NE(manifest.find("buffer_residency=queued-voq-excludes-egress-serializer"),
              std::string::npos);
    EXPECT_NE(manifest.find("recovery=go-back-n"), std::string::npos);
    EXPECT_NE(manifest.find("ecn=ns-tm3-egress-selection-red"), std::string::npos);
    EXPECT_NE(manifest.find("ecn_kmin_bytes=0"), std::string::npos);
    EXPECT_NE(manifest.find("ecn_kmax_bytes=4096"), std::string::npos);
    EXPECT_NE(manifest.find("ecn_pmax_ppm=1000000"), std::string::npos);
    EXPECT_NE(manifest.find("ecn_seed=9"), std::string::npos);
    EXPECT_NE(manifest.find("ecn_sampler=packet-switch-egress-hash"), std::string::npos);
    EXPECT_NE(manifest.find("pfc_meter_scope=per-physical-ingress"), std::string::npos);
    EXPECT_NE(manifest.find("dcqcn_min_rate_bps=100000000"), std::string::npos);
    EXPECT_NE(manifest.find("cnp_timer=single-coalesced-event-source"), std::string::npos);
    EXPECT_NE(manifest.find("cc_timer=dedicated-coalesced-event-source"), std::string::npos);
    EXPECT_NE(manifest.find("pacing_timer=single-coalesced-event-source"), std::string::npos);
    EXPECT_NE(manifest.find("pfc_wire_bytes=64"), std::string::npos);
    EXPECT_NE(manifest.find("pfc_delivery=dedicated-link-local-reverse-serializer"),
              std::string::npos);
    EXPECT_NE(manifest.find("pfc_reverse_sharing=not-shared-with-reverse-data"), std::string::npos);
    EXPECT_NE(manifest.find("pfc_reverse_order=fifo-serialize-then-propagate"), std::string::npos);
}


TEST(DcqcnAtlahsRuntimeTest, EcnOnlyModeDropsOnOverflowAndRecoversWithARateCut) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    const std::filesystem::path topology =
        std::filesystem::path(__FILE__).parent_path() /
        "../../../experiments/rnic_multibaseline/topologies/clos_64_400g.topo";
    DcqcnAtlahsRuntimeConfig config;
    config.topology_file = topology.lexically_normal().string();
    // Comparator-realism ruling: pfc off is the ECN-only mode. The small
    // buffers force overflow drops through the counted ns-tm3 drop path;
    // recovery is the transport's job and every recovery event cuts the
    // rate as a CNP would.
    config.pfc_enabled = false;
    config.ns_tm3_shared_buffer_bytes = 128 * 1024;
    config.ns_tm3_egress_buffer_bytes = 128 * 1024;
    config.ecn_kmin_bytes = 0;
    config.ecn_kmax_bytes = 4096;
    config.ecn_pmax_ppm = 1000000;
    config.ecn_seed = 9;
    config.silent_loss_rto_ps = UINT64_C(1000000000);
    DcqcnAtlahsRuntime runtime(event_list, config, 64);
    std::vector<AtlahsFlowId> completed;
    runtime.setup(64, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    for (std::uint32_t source = 0; source < 8; ++source) {
        runtime.send(AtlahsFlowRequest{177 + source, source, 63, 64 * 1024, EventList::now(), 9});
    }
    while (runtime.hasPendingPhysicalWork()) {
        ASSERT_TRUE(EventList::doNextEvent());
    }

    EXPECT_EQ(completed.size(), 8U);
    EXPECT_EQ(runtime.completed_flow_count(), 8U);
    EXPECT_GT(runtime.dropped_packet_count(), 0U);
    EXPECT_GT(runtime.loss_rate_cut_count(), 0U);
    EXPECT_EQ(runtime.pfc_pause_count(), 0U);
    EXPECT_EQ(runtime.pfc_resume_count(), 0U);
    EXPECT_EQ(runtime.pfc_paused_wall_ps_total(), 0U);
    EXPECT_EQ(runtime.pfc_max_cascade_depth(), 0U);
    EXPECT_TRUE(runtime.renderPfcPortMetricsManifest().empty());

    const std::string manifest =
        renderDcqcnAtlahsManifest(config, 64, "flat.bin", "", "", "gpu-rank");
    EXPECT_NE(manifest.find("pfc=off-ecn-only-drop-on-overflow"), std::string::npos);
    EXPECT_NE(manifest.find("recovery=go-back-n"), std::string::npos);
    EXPECT_NE(manifest.find("sr_window_packets=64"), std::string::npos);
    EXPECT_NE(manifest.find("loss_rate_cut=on"), std::string::npos);
}

}  // namespace
