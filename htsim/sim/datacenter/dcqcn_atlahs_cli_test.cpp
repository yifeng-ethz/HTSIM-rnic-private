#include <gtest/gtest.h>

#include "dcqcn_atlahs_cli.h"

#include <string>
#include <vector>

namespace {

DcqcnAtlahsCliOptions parse(std::vector<std::string> values) {
    std::vector<const char*> arguments;
    arguments.reserve(values.size());
    for (const std::string& value : values) {
        arguments.push_back(value.c_str());
    }
    return parseDcqcnAtlahsCli(static_cast<int>(arguments.size()), arguments.data());
}

TEST(DcqcnAtlahsCliTest, AcceptsCanonicalCompletionAndModelOptions) {
    const DcqcnAtlahsCliOptions options = parse({"htsim_dcqcn_atlahs",
                                                 "-goal",
                                                 "flat.bin",
                                                 "-topology",
                                                 "clos.topo",
                                                 "-completion_csv",
                                                 "flows.csv",
                                                 "-state_trace_csv",
                                                 "state.csv",
                                                 "-goal_rank_mapping",
                                                 "gpu-rank",
                                                 "-seed",
                                                 "17",
                                                 "-shared_buffer_bytes",
                                                 "67108864",
                                                 "-egress_buffer_bytes",
                                                 "1048576",
                                                 "-ecn_kmin_bytes",
                                                 "65536",
                                                 "-ecn_kmax_bytes",
                                                 "655360",
                                                 "-ecn_pmax_ppm",
                                                 "250000",
                                                 "-pfc_low_bytes",
                                                 "262144",
                                                 "-pfc_high_bytes",
                                                 "524288",
                                                 "-silent_rto_us",
                                                 "50000",
                                                 "-dcqcn_min_rate_bps",
                                                 "100000000"});

    EXPECT_EQ(options.goal_file, "flat.bin");
    ASSERT_TRUE(options.completion_csv.has_value());
    EXPECT_EQ(*options.completion_csv, "flows.csv");
    ASSERT_TRUE(options.runtime.state_trace_csv.has_value());
    EXPECT_EQ(*options.runtime.state_trace_csv, "state.csv");
    EXPECT_EQ(options.goal_rank_mapping, DcqcnGoalRankMapping::GpuRank);
    EXPECT_EQ(options.runtime.topology_file, "clos.topo");
    EXPECT_EQ(options.runtime.ecmp_seed, 17U);
    EXPECT_EQ(options.runtime.ns_tm3_shared_buffer_bytes, 67108864);
    EXPECT_EQ(options.runtime.ns_tm3_egress_buffer_bytes, 1048576);
    EXPECT_EQ(options.runtime.ecn_kmin_bytes, 65536);
    EXPECT_EQ(options.runtime.ecn_kmax_bytes, 655360);
    EXPECT_EQ(options.runtime.ecn_pmax_ppm, 250000U);
    EXPECT_EQ(options.runtime.ecn_seed, 17U);
    EXPECT_EQ(options.runtime.pfc_low_threshold_bytes, 262144);
    EXPECT_EQ(options.runtime.pfc_high_threshold_bytes, 524288);
    EXPECT_EQ(options.runtime.silent_loss_rto_ps, UINT64_C(50000000000));
    EXPECT_EQ(options.runtime.dcqcn_min_rate_bps, UINT64_C(100000000));
}

TEST(DcqcnAtlahsCliTest, ExplicitEcnSeedOverridesRunSeedInEitherOrder) {
    const DcqcnAtlahsCliOptions first = parse(
        {"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo", "-ecn_seed", "91", "-seed", "17"});
    const DcqcnAtlahsCliOptions second = parse(
        {"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo", "-seed", "17", "-ecn_seed", "91"});
    EXPECT_EQ(first.runtime.ecmp_seed, 17U);
    EXPECT_EQ(first.runtime.ecn_seed, 91U);
    EXPECT_EQ(second.runtime.ecmp_seed, 17U);
    EXPECT_EQ(second.runtime.ecn_seed, 91U);
}

TEST(DcqcnAtlahsCliTest, DefaultsToThePinned400GComparisonProfile) {
    const DcqcnAtlahsCliOptions options =
        parse({"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo"});
    EXPECT_EQ(options.runtime.ecn_kmin_bytes, 65536);
    EXPECT_EQ(options.runtime.ecn_kmax_bytes, 655360);
    EXPECT_EQ(options.runtime.ecn_pmax_ppm, 250000U);
    EXPECT_EQ(options.runtime.ecn_seed, options.runtime.ecmp_seed);
    EXPECT_EQ(options.runtime.pfc_low_threshold_bytes, 520000);
    EXPECT_EQ(options.runtime.pfc_high_threshold_bytes, 720000);
    EXPECT_EQ(options.runtime.ns_tm3_egress_buffer_bytes,
              options.runtime.ns_tm3_shared_buffer_bytes);
    EXPECT_EQ(options.runtime.dcqcn_min_rate_bps, UINT64_C(100000000));
}

TEST(DcqcnAtlahsCliTest, OmittedEgressBufferInheritsTheFinalSharedPoolValue) {
    const DcqcnAtlahsCliOptions larger = parse({"dcqcn", "-goal", "flat.bin", "-topology",
                                                "clos.topo", "-shared_buffer_bytes", "67108864"});
    EXPECT_EQ(larger.runtime.ns_tm3_shared_buffer_bytes, 67108864);
    EXPECT_EQ(larger.runtime.ns_tm3_egress_buffer_bytes, 67108864);

    const DcqcnAtlahsCliOptions smaller = parse({"dcqcn", "-goal", "flat.bin", "-topology",
                                                 "clos.topo", "-shared_buffer_bytes", "1048576"});
    EXPECT_EQ(smaller.runtime.ns_tm3_shared_buffer_bytes, 1048576);
    EXPECT_EQ(smaller.runtime.ns_tm3_egress_buffer_bytes, 1048576);
}

TEST(DcqcnAtlahsCliTest, ExplicitEgressBufferDoesNotDependOnOptionOrder) {
    const DcqcnAtlahsCliOptions egress_first =
        parse({"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo", "-egress_buffer_bytes",
               "1048576", "-shared_buffer_bytes", "67108864"});
    const DcqcnAtlahsCliOptions shared_first =
        parse({"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo", "-shared_buffer_bytes",
               "67108864", "-egress_buffer_bytes", "1048576"});
    EXPECT_EQ(egress_first.runtime.ns_tm3_egress_buffer_bytes, 1048576);
    EXPECT_EQ(shared_first.runtime.ns_tm3_egress_buffer_bytes, 1048576);
    EXPECT_EQ(egress_first.runtime.ns_tm3_shared_buffer_bytes, 67108864);
    EXPECT_EQ(shared_first.runtime.ns_tm3_shared_buffer_bytes, 67108864);
}

TEST(DcqcnAtlahsCliTest, RequiresGoalAndTopologyAndRejectsLegacyAliases) {
    EXPECT_THROW(parse({"dcqcn", "-topology", "clos.topo"}), std::invalid_argument);
    EXPECT_THROW(parse({"dcqcn", "-goal", "flat.bin"}), std::invalid_argument);
    EXPECT_THROW(parse({"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo", "-tm3", "true"}),
                 std::invalid_argument);
    EXPECT_THROW(parse({"dcqcn", "-goal", "flat.bin", "-topology", "clos.topo",
                        "-ecn_threshold_bytes", "262144"}),
                 std::invalid_argument);
}

TEST(DcqcnAtlahsCliTest, UsageNamesSeparateComparatorExecutable) {
    const std::string usage = dcqcnAtlahsCliUsage("htsim_dcqcn_atlahs");
    EXPECT_NE(usage.find("htsim_dcqcn_atlahs"), std::string::npos);
    EXPECT_NE(usage.find("-completion_csv FILE"), std::string::npos);
    EXPECT_NE(usage.find("-state_trace_csv FILE"), std::string::npos);
    EXPECT_NE(usage.find("-ecn_kmin_bytes N"), std::string::npos);
    EXPECT_NE(usage.find("-egress_buffer_bytes N"), std::string::npos);
    EXPECT_NE(usage.find("-dcqcn_min_rate_bps N"), std::string::npos);
    EXPECT_EQ(usage.find("rnic-"), std::string::npos);
}

}  // namespace
