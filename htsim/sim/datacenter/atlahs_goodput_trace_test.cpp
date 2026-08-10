// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include "atlahs_goodput_trace.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST(AtlahsGoodputTraceTest, AggregatesExactPayloadIntoEpochAlignedSparseBins) {
    AtlahsGoodputTrace trace(100);
    trace.record(199, 7, 1, 9, 2);
    trace.record(100, 7, 1, 9, 3);
    trace.record(200, 8, 2, 9, 5);
    trace.record(250, 8, 2, 9, 0);
    EXPECT_EQ(trace.size(), 2U);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "atlahs-goodput-trace-test.csv";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
    trace.writeCsvAtomically(path.string());
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(text,
              "bin_start_ps,bin_end_ps,flow_id,source,destination,"
              "delivered_payload_bytes,goodput_bps\n"
              "100,200,7,1,9,5,400000000000\n"
              "200,300,8,2,9,5,400000000000\n");
    input.close();
    std::filesystem::remove(path);
}

TEST(AtlahsGoodputTraceTest, DisabledTraceIsAnObservationalNoop) {
    AtlahsGoodputTrace trace;
    trace.record(1, 7, 1, 9, 4096);
    EXPECT_FALSE(trace.enabled());
    EXPECT_EQ(trace.size(), 0U);
    EXPECT_THROW(trace.writeCsvAtomically("unused.csv"), std::logic_error);
}
