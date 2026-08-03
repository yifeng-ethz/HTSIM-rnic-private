// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rnic_atlahs_cli.h"

namespace {

RnicAtlahsCliOptions parse(std::vector<std::string> arguments) {
    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    return parseRnicAtlahsCli(static_cast<int>(argv.size()), argv.data());
}

std::vector<std::string> baseArguments(const std::string& profile) {
    return {"htsim_rnic",     "-goal",        "workload.goal", "-nodes", "32",
            "-linkspeed_bps", "100000000000", "-rnic_profile", profile};
}

void append(std::vector<std::string>& arguments,
            const std::string& option,
            const std::string& value) {
    arguments.push_back(option);
    arguments.push_back(value);
}

TEST(RnicAtlahsCliTest, ResolvesCollectiveDefaultsInConstructorUnits) {
    const RnicAtlahsCliOptions options = parse(baseArguments("rnic-cn"));

    EXPECT_EQ(options.goal_file, "workload.goal");
    EXPECT_EQ(options.goal_rank_mapping, RnicAtlahsGoalRankMapping::Auto);
    EXPECT_EQ(std::string(rnicAtlahsGoalRankMappingName(options.goal_rank_mapping)), "auto");
    EXPECT_FALSE(options.explicitly_supplied.goal_rank_mapping);
    EXPECT_EQ(options.node_count, 32U);
    EXPECT_TRUE(options.explicitly_supplied.node_count);
    EXPECT_EQ(options.link_capacity_bps, UINT64_C(100000000000));
    EXPECT_EQ(options.profile, RnicProfile::CollectiveNetwork);
    EXPECT_EQ(options.packet.max_wire_packet_bytes, 4160U);
    EXPECT_EQ(options.packet.data_header_bytes, 64U);
    EXPECT_FALSE(options.collective.topology_file.has_value());
    EXPECT_EQ(options.collective.hop_latency_ps, 1000000U);
    EXPECT_EQ(options.collective.switch_latency_ps, 0U);
    EXPECT_EQ(options.collective.global_prbs_seed, UINT64_C(0x123456789abcdef0));
    EXPECT_EQ(options.collective.control_deadline_ps, 10000000U);
    EXPECT_EQ(options.collective.margin_ppm, 900000U);
    EXPECT_EQ(options.collective.control_wire_bytes, 64U);
    EXPECT_EQ(options.collective.ring_delay_window_ps, 4096000U);
    EXPECT_EQ(options.collective.ring_release_tick_ps, 16000U);
    EXPECT_EQ(options.collective.ring_wire_capacity_bytes, UINT64_C(1) << 20);
    EXPECT_EQ(options.collective.ns_tm3_shared_buffer_bytes, UINT64_C(1) << 20);
    EXPECT_EQ(options.collective.maximum_retransmissions, 8U);
    EXPECT_EQ(options.collective.retransmission_rto_ps, UINT64_C(50000000000));

    const RnicAtlahsExplicitCliOptions& supplied = options.explicitly_supplied;
    EXPECT_FALSE(supplied.max_wire_packet_bytes);
    EXPECT_FALSE(supplied.data_header_bytes);
    EXPECT_FALSE(supplied.topology_file);
    EXPECT_FALSE(supplied.hop_latency_ps);
    EXPECT_FALSE(supplied.switch_latency_ps);
    EXPECT_FALSE(supplied.global_prbs_seed);
    EXPECT_FALSE(supplied.control_deadline_ps);
    EXPECT_FALSE(supplied.margin_ppm);
    EXPECT_FALSE(supplied.control_wire_bytes);
    EXPECT_FALSE(supplied.ring_delay_window_ps);
    EXPECT_FALSE(supplied.ring_release_tick_ps);
    EXPECT_FALSE(supplied.ring_wire_capacity_bytes);
    EXPECT_FALSE(supplied.ns_tm3_shared_buffer_bytes);
    EXPECT_FALSE(supplied.cn_maximum_retransmissions);
    EXPECT_FALSE(supplied.cn_retransmission_rto_ps);
}

TEST(RnicAtlahsCliTest, RecordsExplicitGeneratedCollectiveOverrides) {
    std::vector<std::string> arguments = baseArguments("rnic-cn");
    append(arguments, "-rnic_max_wire_bytes", "1500");
    append(arguments, "-rnic_data_header_bytes", "100");
    append(arguments, "-rnic_hop_latency_ps", "200000");
    append(arguments, "-rnic_switch_latency_ps", "50000");
    append(arguments, "-rnic_cn_prbs_seed", "42");
    append(arguments, "-rnic_cn_control_deadline_ps", "12000000");
    append(arguments, "-rnic_cn_margin_ppm", "985000");
    append(arguments, "-rnic_cn_control_wire_bytes", "96");
    append(arguments, "-rnic_cn_ring_window_ps", "8192000");
    append(arguments, "-rnic_cn_ring_tick_ps", "32000");
    append(arguments, "-rnic_cn_ring_capacity_bytes", "2097152");
    append(arguments, "-rnic_cn_ns_tm3_buffer_bytes", "4194304");
    append(arguments, "-rnic_cn_max_retransmissions", "12");
    append(arguments, "-rnic_cn_retransmission_rto_ps", "25000000000");

    const RnicAtlahsCliOptions options = parse(std::move(arguments));
    EXPECT_FALSE(options.collective.topology_file.has_value());
    EXPECT_EQ(options.packet.max_wire_packet_bytes, 1500U);
    EXPECT_EQ(options.packet.data_header_bytes, 100U);
    EXPECT_EQ(options.collective.hop_latency_ps, 200000U);
    EXPECT_EQ(options.collective.switch_latency_ps, 50000U);
    EXPECT_EQ(options.collective.global_prbs_seed, 42U);
    EXPECT_EQ(options.collective.control_deadline_ps, 12000000U);
    EXPECT_EQ(options.collective.margin_ppm, 985000U);
    EXPECT_EQ(options.collective.control_wire_bytes, 96U);
    EXPECT_EQ(options.collective.ring_delay_window_ps, 8192000U);
    EXPECT_EQ(options.collective.ring_release_tick_ps, 32000U);
    EXPECT_EQ(options.collective.ring_wire_capacity_bytes, 2097152U);
    EXPECT_EQ(options.collective.ns_tm3_shared_buffer_bytes, 4194304U);
    EXPECT_EQ(options.collective.maximum_retransmissions, 12U);
    EXPECT_EQ(options.collective.retransmission_rto_ps, UINT64_C(25000000000));

    const RnicAtlahsExplicitCliOptions& supplied = options.explicitly_supplied;
    EXPECT_TRUE(supplied.max_wire_packet_bytes);
    EXPECT_TRUE(supplied.data_header_bytes);
    EXPECT_FALSE(supplied.topology_file);
    EXPECT_TRUE(supplied.hop_latency_ps);
    EXPECT_TRUE(supplied.switch_latency_ps);
    EXPECT_TRUE(supplied.global_prbs_seed);
    EXPECT_TRUE(supplied.control_deadline_ps);
    EXPECT_TRUE(supplied.margin_ppm);
    EXPECT_TRUE(supplied.control_wire_bytes);
    EXPECT_TRUE(supplied.ring_delay_window_ps);
    EXPECT_TRUE(supplied.ring_release_tick_ps);
    EXPECT_TRUE(supplied.ring_wire_capacity_bytes);
    EXPECT_TRUE(supplied.ns_tm3_shared_buffer_bytes);
    EXPECT_TRUE(supplied.cn_maximum_retransmissions);
    EXPECT_TRUE(supplied.cn_retransmission_rto_ps);
}

TEST(RnicAtlahsCliTest, ResolvesCanonicalSlingshotLikeOverrides) {
    std::vector<std::string> arguments = baseArguments("rnic-ss");
    append(arguments, "-rnic_ss_control_wire_bytes", "96");
    append(arguments, "-rnic_ss_ns_rosetta_buffer_bytes", "16777216");
    append(arguments, "-rnic_ss_q_hi_bytes", "4194304");
    append(arguments, "-rnic_ss_q_lo_bytes", "1048576");
    append(arguments, "-rnic_ss_telemetry_delay_ps", "1000000");
    append(arguments, "-rnic_ss_path_hysteresis_ps", "32000");
    append(arguments, "-rnic_ss_sample_age_ps", "20000000");
    append(arguments, "-rnic_ss_credit_quantum_packets", "8");
    append(arguments, "-rnic_ss_window_packets", "32");
    append(arguments, "-rnic_ss_rto_ps", "50000000000");
    append(arguments, "-rnic_ss_max_retransmissions", "12");
    append(arguments, "-rnic_ss_max_credit_ahead_bytes", "8388608");
    append(arguments, "-rnic_ss_routing_seed", "42");
    append(arguments, "-rnic_ss_routing", "ordered");
    append(arguments, "-rnic_ss_loss_stress", "on");

    const RnicAtlahsCliOptions options = parse(std::move(arguments));
    EXPECT_EQ(options.profile, RnicProfile::SlingshotLike);
    EXPECT_EQ(options.slingshot.control_wire_bytes, 96U);
    EXPECT_EQ(options.slingshot.ns_rosetta_shared_buffer_bytes, 16777216U);
    EXPECT_EQ(options.slingshot.q_hi_bytes, 4194304U);
    EXPECT_EQ(options.slingshot.q_lo_bytes, 1048576U);
    EXPECT_EQ(options.slingshot.telemetry_delay_ps, 1000000U);
    EXPECT_EQ(options.slingshot.path_hysteresis_ps, 32000U);
    EXPECT_EQ(options.slingshot.maximum_sample_age_ps, 20000000U);
    EXPECT_EQ(options.slingshot.credit_quantum_packets, 8U);
    EXPECT_EQ(options.slingshot.outstanding_window_packets, 32U);
    EXPECT_EQ(options.slingshot.rto_ps, UINT64_C(50000000000));
    EXPECT_EQ(options.slingshot.maximum_retransmissions, 12U);
    EXPECT_EQ(options.slingshot.maximum_credit_ahead_bytes, 8388608U);
    EXPECT_EQ(options.slingshot.routing_seed, 42U);
    EXPECT_FALSE(options.slingshot.unordered_packet_routing);
    EXPECT_TRUE(options.slingshot.allow_loss_stress);
    EXPECT_TRUE(options.explicitly_supplied.ss_routing);
    EXPECT_TRUE(options.explicitly_supplied.ss_loss_stress);
}

TEST(RnicAtlahsCliTest, ResolvesOrderedSack128SlingshotDefaults) {
    const RnicAtlahsCliOptions options = parse(baseArguments("rnic-ss"));

    EXPECT_EQ(options.slingshot.outstanding_window_packets, 128U);
    EXPECT_FALSE(options.slingshot.unordered_packet_routing);
    EXPECT_EQ(options.slingshot.control_wire_bytes, 64U);

    std::vector<std::string> too_wide = baseArguments("rnic-ss");
    append(too_wide, "-rnic_ss_window_packets", "129");
    EXPECT_THROW(parse(std::move(too_wide)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, ResolvesPacketizedManifoldOverrides) {
    std::vector<std::string> arguments = baseArguments("rnic-nn");
    arguments[4] = "7";
    append(arguments, "-rnic_max_wire_bytes", "2048");
    append(arguments, "-rnic_data_header_bytes", "48");
    append(arguments, "-rnic_nn_propagation_ps", "123456");

    const RnicAtlahsCliOptions options = parse(std::move(arguments));
    EXPECT_EQ(options.profile, RnicProfile::PacketizedManifold);
    EXPECT_EQ(options.packet.max_wire_packet_bytes, 2048U);
    EXPECT_EQ(options.packet.data_header_bytes, 48U);
    EXPECT_EQ(options.manifold.fixed_propagation_delay_ps, 123456U);
    EXPECT_TRUE(options.explicitly_supplied.max_wire_packet_bytes);
    EXPECT_TRUE(options.explicitly_supplied.data_header_bytes);
    EXPECT_TRUE(options.explicitly_supplied.fixed_propagation_delay_ps);
}

TEST(RnicAtlahsCliTest, FluidProfileAcceptsOnlyItsFixedDelayOverride) {
    std::vector<std::string> arguments = baseArguments("rnic-nn-fluid");
    arguments[4] = "1";
    append(arguments, "-rnic_nn_propagation_ps", "0");

    const RnicAtlahsCliOptions options = parse(std::move(arguments));
    EXPECT_EQ(options.profile, RnicProfile::FluidManifold);
    EXPECT_EQ(options.manifold.fixed_propagation_delay_ps, 0U);
    EXPECT_TRUE(options.explicitly_supplied.fixed_propagation_delay_ps);
}

TEST(RnicAtlahsCliTest, OmitsNodeOverrideForGoalLayoutDiscovery) {
    std::vector<std::string> arguments = baseArguments("rnic-cn");
    arguments.erase(arguments.begin() + 3, arguments.begin() + 5);

    const RnicAtlahsCliOptions options = parse(std::move(arguments));
    EXPECT_EQ(options.node_count, 0U);
    EXPECT_FALSE(options.explicitly_supplied.node_count);
}

TEST(RnicAtlahsCliTest, AcceptsProtocolNeutralCompletionCsv) {
    for (const std::string& profile : {"rnic-cn", "rnic-ss", "rnic-nn", "rnic-nn-fluid"}) {
        std::vector<std::string> arguments = baseArguments(profile);
        append(arguments, "-completion_csv", "results/fct.csv");
        const RnicAtlahsCliOptions options = parse(std::move(arguments));
        ASSERT_TRUE(options.completion_csv.has_value());
        EXPECT_EQ(*options.completion_csv, "results/fct.csv");
    }
}

TEST(RnicAtlahsCliTest, AcceptsOnlyExactGoalRankMappingNames) {
    const std::vector<std::pair<std::string, RnicAtlahsGoalRankMapping>> valid{
        {"auto", RnicAtlahsGoalRankMapping::Auto},
        {"gpu-rank", RnicAtlahsGoalRankMapping::GpuRank},
        {"unique-nic", RnicAtlahsGoalRankMapping::UniqueNic},
    };
    for (const auto& item : valid) {
        std::vector<std::string> arguments = baseArguments("rnic-nn-fluid");
        append(arguments, "-goal_rank_mapping", item.first);
        const RnicAtlahsCliOptions options = parse(std::move(arguments));
        EXPECT_EQ(options.goal_rank_mapping, item.second);
        EXPECT_TRUE(options.explicitly_supplied.goal_rank_mapping);
        EXPECT_EQ(std::string(rnicAtlahsGoalRankMappingName(item.second)), item.first);
    }

    for (const std::string& invalid : {"gpu_rank", "unique_nic", "GPU-RANK", "infer", "v1", "v2"}) {
        std::vector<std::string> arguments = baseArguments("rnic-cn");
        append(arguments, "-goal_rank_mapping", invalid);
        EXPECT_THROW(parse(std::move(arguments)), std::invalid_argument) << invalid;
    }
}

TEST(RnicAtlahsCliTest, RejectsEveryNonPublicProfileName) {
    for (const std::string& alias :
         {"rnic-cc", "rnic-null", "tm3", "collective-network", "RNIC-CN"}) {
        EXPECT_THROW(parse(baseArguments(alias)), std::invalid_argument) << alias;
    }
}

TEST(RnicAtlahsCliTest, RejectsRetiredTomahawk3BufferFlag) {
    std::vector<std::string> arguments = baseArguments("rnic-cn");
    append(arguments, "-rnic_cn_tomahawk3_buffer_bytes", "4194304");
    EXPECT_THROW(parse(std::move(arguments)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, RejectsMissingRequiredOptionsAndValues) {
    EXPECT_THROW(parse({"htsim_rnic"}), std::invalid_argument);
    EXPECT_THROW(parse({"htsim_rnic", "-goal"}), std::invalid_argument);

    std::vector<std::string> missing_goal = baseArguments("rnic-cn");
    missing_goal.erase(missing_goal.begin() + 1, missing_goal.begin() + 3);
    EXPECT_THROW(parse(std::move(missing_goal)), std::invalid_argument);

    std::vector<std::string> missing_speed = baseArguments("rnic-cn");
    missing_speed.erase(missing_speed.begin() + 5, missing_speed.begin() + 7);
    EXPECT_THROW(parse(std::move(missing_speed)), std::invalid_argument);

    std::vector<std::string> missing_profile = baseArguments("rnic-cn");
    missing_profile.erase(missing_profile.begin() + 7, missing_profile.begin() + 9);
    EXPECT_THROW(parse(std::move(missing_profile)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, RejectsMalformedNegativeOverflowAndHexNumerics) {
    for (const std::string& bad : {"-1", "1.5", "12x", "+12", "0x10", "18446744073709551616"}) {
        std::vector<std::string> arguments = baseArguments("rnic-nn");
        arguments[6] = bad;
        EXPECT_THROW(parse(std::move(arguments)), std::invalid_argument) << bad;
    }

    std::vector<std::string> nodes = baseArguments("rnic-nn");
    nodes[4] = "4294967296";
    EXPECT_THROW(parse(std::move(nodes)), std::invalid_argument);

    std::vector<std::string> zero_nodes = baseArguments("rnic-nn");
    zero_nodes[4] = "0";
    EXPECT_THROW(parse(std::move(zero_nodes)), std::invalid_argument);

    std::vector<std::string> maximum_api_nodes = baseArguments("rnic-nn");
    maximum_api_nodes[4] = "2147483647";
    EXPECT_NO_THROW(parse(std::move(maximum_api_nodes)));

    std::vector<std::string> above_api_nodes = baseArguments("rnic-nn");
    above_api_nodes[4] = "2147483648";
    EXPECT_THROW(parse(std::move(above_api_nodes)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, RecognizesOnlyGeneratedTwoTierClosNodeCounts) {
    for (const std::uint32_t valid : {2U, 8U, 18U, 32U, 50U, 72U, 128U}) {
        EXPECT_TRUE(isRnicGeneratedTwoTierClosNodeCount(valid)) << valid;
    }
    for (const std::uint32_t invalid : {0U, 1U, 3U, 7U, 31U, 33U, 127U}) {
        EXPECT_FALSE(isRnicGeneratedTwoTierClosNodeCount(invalid)) << invalid;
    }
}

TEST(RnicAtlahsCliTest, RejectsDuplicateAndUnknownOptions) {
    std::vector<std::string> duplicate = baseArguments("rnic-cn");
    append(duplicate, "-nodes", "32");
    EXPECT_THROW(parse(std::move(duplicate)), std::invalid_argument);

    std::vector<std::string> unknown = baseArguments("rnic-cn");
    append(unknown, "-rnic-cc", "1");
    EXPECT_THROW(parse(std::move(unknown)), std::invalid_argument);

    std::vector<std::string> legacy_speed = baseArguments("rnic-cn");
    legacy_speed[5] = "-linkspeed";
    EXPECT_THROW(parse(std::move(legacy_speed)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, RejectsCrossProfileOptions) {
    std::vector<std::string> fluid_packet = baseArguments("rnic-nn-fluid");
    append(fluid_packet, "-rnic_max_wire_bytes", "1000");
    EXPECT_THROW(parse(std::move(fluid_packet)), std::invalid_argument);

    std::vector<std::string> cn_manifold = baseArguments("rnic-cn");
    append(cn_manifold, "-rnic_nn_propagation_ps", "1000");
    EXPECT_THROW(parse(std::move(cn_manifold)), std::invalid_argument);

    std::vector<std::string> nn_topology = baseArguments("rnic-nn");
    append(nn_topology, "-topo", "clos.topo");
    EXPECT_THROW(parse(std::move(nn_topology)), std::invalid_argument);

    std::vector<std::string> fluid_control = baseArguments("rnic-nn-fluid");
    append(fluid_control, "-rnic_cn_margin_ppm", "900000");
    EXPECT_THROW(parse(std::move(fluid_control)), std::invalid_argument);

    std::vector<std::string> cn_ss = baseArguments("rnic-cn");
    append(cn_ss, "-rnic_ss_q_hi_bytes", "1048576");
    EXPECT_THROW(parse(std::move(cn_ss)), std::invalid_argument);

    std::vector<std::string> ss_cn_retransmission = baseArguments("rnic-ss");
    append(ss_cn_retransmission, "-rnic_cn_max_retransmissions", "8");
    EXPECT_THROW(parse(std::move(ss_cn_retransmission)), std::invalid_argument);

    std::vector<std::string> nn_cn_rto = baseArguments("rnic-nn");
    append(nn_cn_rto, "-rnic_cn_retransmission_rto_ps", "50000000000");
    EXPECT_THROW(parse(std::move(nn_cn_rto)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, ChecksPacketizationAndSimulatorResolution) {
    std::vector<std::string> zero_max = baseArguments("rnic-nn");
    append(zero_max, "-rnic_max_wire_bytes", "0");
    EXPECT_THROW(parse(std::move(zero_max)), std::invalid_argument);

    std::vector<std::string> equal_header = baseArguments("rnic-nn");
    append(equal_header, "-rnic_max_wire_bytes", "64");
    append(equal_header, "-rnic_data_header_bytes", "64");
    EXPECT_THROW(parse(std::move(equal_header)), std::invalid_argument);

    std::vector<std::string> collapsed_slot = baseArguments("rnic-nn");
    collapsed_slot[6] = "8000000000001";
    append(collapsed_slot, "-rnic_max_wire_bytes", "1");
    append(collapsed_slot, "-rnic_data_header_bytes", "0");
    EXPECT_THROW(parse(std::move(collapsed_slot)), std::invalid_argument);

    std::vector<std::string> fluid_no_packet_clock = baseArguments("rnic-nn-fluid");
    fluid_no_packet_clock[6] = "18446744073709551615";
    EXPECT_NO_THROW(parse(std::move(fluid_no_packet_clock)));
}

TEST(RnicAtlahsCliTest, ChecksCollectiveTopologyAndRuntimeBounds) {
    std::vector<std::string> invalid_shape = baseArguments("rnic-cn");
    invalid_shape[4] = "31";
    EXPECT_THROW(parse(std::move(invalid_shape)), std::invalid_argument);

    std::vector<std::string> file_shape = baseArguments("rnic-cn");
    file_shape[4] = "31";
    append(file_shape, "-topo", "checked-later.topo");
    const RnicAtlahsCliOptions file_options = parse(std::move(file_shape));
    ASSERT_TRUE(file_options.collective.topology_file.has_value());
    EXPECT_EQ(*file_options.collective.topology_file, "checked-later.topo");
    EXPECT_TRUE(file_options.explicitly_supplied.topology_file);

    std::vector<std::string> too_fast = baseArguments("rnic-cn");
    too_fast[6] = "8000000000001";
    EXPECT_THROW(parse(std::move(too_fast)), std::invalid_argument);

    std::vector<std::string> data_extent = baseArguments("rnic-cn");
    append(data_extent, "-rnic_max_wire_bytes", "65536");
    EXPECT_THROW(parse(std::move(data_extent)), std::invalid_argument);

    std::vector<std::string> control_extent = baseArguments("rnic-cn");
    append(control_extent, "-rnic_cn_control_wire_bytes", "65536");
    EXPECT_THROW(parse(std::move(control_extent)), std::invalid_argument);

    std::vector<std::string> zero_deadline = baseArguments("rnic-cn");
    append(zero_deadline, "-rnic_cn_control_deadline_ps", "0");
    EXPECT_THROW(parse(std::move(zero_deadline)), std::invalid_argument);

    for (const std::string& margin : {"0", "1000001"}) {
        std::vector<std::string> arguments = baseArguments("rnic-cn");
        append(arguments, "-rnic_cn_margin_ppm", margin);
        EXPECT_THROW(parse(std::move(arguments)), std::invalid_argument);
    }

    std::vector<std::string> small_ring = baseArguments("rnic-cn");
    append(small_ring, "-rnic_cn_ring_capacity_bytes", "52199");
    EXPECT_THROW(parse(std::move(small_ring)), std::invalid_argument);

    std::vector<std::string> exact_ring = baseArguments("rnic-cn");
    append(exact_ring, "-rnic_cn_ring_capacity_bytes", "55360");
    EXPECT_NO_THROW(parse(std::move(exact_ring)));

    std::vector<std::string> zero_tick = baseArguments("rnic-cn");
    append(zero_tick, "-rnic_cn_ring_tick_ps", "0");
    EXPECT_THROW(parse(std::move(zero_tick)), std::invalid_argument);

    std::vector<std::string> zero_buffer = baseArguments("rnic-cn");
    append(zero_buffer, "-rnic_cn_ns_tm3_buffer_bytes", "0");
    EXPECT_THROW(parse(std::move(zero_buffer)), std::invalid_argument);

    std::vector<std::string> zero_retransmission = baseArguments("rnic-cn");
    append(zero_retransmission, "-rnic_cn_max_retransmissions", "0");
    EXPECT_THROW(parse(std::move(zero_retransmission)), std::invalid_argument);

    std::vector<std::string> zero_retransmission_rto = baseArguments("rnic-cn");
    append(zero_retransmission_rto, "-rnic_cn_retransmission_rto_ps", "0");
    EXPECT_THROW(parse(std::move(zero_retransmission_rto)), std::invalid_argument);

    std::vector<std::string> huge_buffer = baseArguments("rnic-cn");
    append(huge_buffer, "-rnic_cn_ns_tm3_buffer_bytes", "9223372036854775808");
    EXPECT_THROW(parse(std::move(huge_buffer)), std::invalid_argument);

    std::vector<std::string> diameter_overflow = baseArguments("rnic-cn");
    append(diameter_overflow, "-rnic_hop_latency_ps", "18446744073709551615");
    EXPECT_THROW(parse(std::move(diameter_overflow)), std::invalid_argument);

    std::vector<std::string> file_and_latency = baseArguments("rnic-cn");
    append(file_and_latency, "-topo", "two-tier.topo");
    append(file_and_latency, "-rnic_hop_latency_ps", "1000000");
    EXPECT_THROW(parse(std::move(file_and_latency)), std::invalid_argument);
}

TEST(RnicAtlahsCliTest, UsageNamesOnlyCanonicalProfilesAndExactUnits) {
    const std::string usage = rnicAtlahsCliUsage("htsim_rnic");
    EXPECT_NE(usage.find("rnic-cn|rnic-ss|rnic-nn|rnic-nn-fluid"), std::string::npos);
    EXPECT_NE(usage.find("-linkspeed_bps"), std::string::npos);
    EXPECT_NE(usage.find("-completion_csv FILE"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_cn_max_retransmissions N"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_cn_retransmission_rto_ps PS"), std::string::npos);
    EXPECT_NE(usage.find("-goal_rank_mapping auto|gpu-rank|unique-nic"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_hop_latency_ps"), std::string::npos);
    EXPECT_NE(usage.find("-topo FILE"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_cn_ns_tm3_buffer_bytes"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_ss_ns_rosetta_buffer_bytes"), std::string::npos);
    EXPECT_NE(usage.find("-rnic_ss_routing unordered|ordered"), std::string::npos);
    EXPECT_EQ(usage.find("-rnic_cn_tomahawk3_buffer_bytes"), std::string::npos);
    EXPECT_EQ(usage.find("rnic-cc"), std::string::npos);
    EXPECT_EQ(usage.find("rnic-null"), std::string::npos);
}

}  // namespace
