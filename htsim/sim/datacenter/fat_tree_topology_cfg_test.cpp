// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <sstream>

#include "fat_tree_topology.h"

namespace {

const char* kTopologyWithoutQueueOverrides = R"(
Nodes 32
Tiers 2
Podsize 32

Tier 0
Downlink_speed_Gbps 100
Radix_Down 8
Radix_Up 2
Downlink_Latency_ns 1000
Switch_Latency_ns 0
Oversubscribed 4

Tier 1
Downlink_speed_Gbps 100
Radix_Down 4
Downlink_Latency_ns 1000
Switch_Latency_ns 0
)";

const char* kTopologyWithPerTierQueues = R"(
Nodes 32
Tiers 2
Podsize 32

Tier 0
Downlink_speed_Gbps 100
Radix_Down 8
Radix_Up 2
Queue_Down 1001
Queue_Up 1002
Downlink_Latency_ns 1000
Switch_Latency_ns 0
Oversubscribed 4

Tier 1
Downlink_speed_Gbps 100
Radix_Down 4
Queue_Down 2001
Downlink_Latency_ns 1000
Switch_Latency_ns 0
)";

TEST(FatTreeTopologyCfgTest, FileConstructorPreservesCallerQueueSize) {
    constexpr mem_b kQueueSize = 32768;
    std::istringstream input(kTopologyWithoutQueueOverrides);

    FatTreeTopologyCfg cfg(input, kQueueSize, COMPOSITE, FAIR_PRIO);

    EXPECT_EQ(cfg.queue_down(TOR_TIER), kQueueSize);
    EXPECT_EQ(cfg.queue_up(TOR_TIER), kQueueSize);
    EXPECT_EQ(cfg.queue_down(AGG_TIER), kQueueSize);
}

TEST(FatTreeTopologyCfgTest, FileConstructorPreservesPerTierQueueSizes) {
    std::istringstream input(kTopologyWithPerTierQueues);

    FatTreeTopologyCfg cfg(input, 0, COMPOSITE, FAIR_PRIO);

    EXPECT_EQ(cfg.queue_down(TOR_TIER), 1001);
    EXPECT_EQ(cfg.queue_up(TOR_TIER), 1002);
    EXPECT_EQ(cfg.queue_down(AGG_TIER), 2001);
}

}  // namespace
