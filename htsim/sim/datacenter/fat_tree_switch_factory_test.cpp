// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

#include "fat_tree_switch_factory.h"
#include "fat_tree_topology.h"
#include "tomahawk3_switch.h"

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

TEST(FatTreeTopologyCfgTest, SwitchModelIsIndependentOfQueueConfiguration) {
    constexpr mem_b kQueueSize = 32768;
    std::istringstream input(kTopologyWithoutQueueOverrides);
    FatTreeTopologyCfg cfg(input, kQueueSize, COMPOSITE, FAIR_PRIO);

    EXPECT_EQ(cfg.switch_model(), FatTreeSwitchModel::Default);
    cfg.set_switch_model(FatTreeSwitchModel::Tomahawk3);

    EXPECT_EQ(cfg.switch_model(), FatTreeSwitchModel::Tomahawk3);
    EXPECT_EQ(cfg.queue_down(TOR_TIER), kQueueSize);
    EXPECT_EQ(cfg.queue_up(TOR_TIER), kQueueSize);
}

TEST(FatTreeSwitchModelTest, HasStableManifestNames) {
    EXPECT_EQ(fat_tree_switch_model_name(FatTreeSwitchModel::Default),
              "default");
    EXPECT_EQ(fat_tree_switch_model_name(FatTreeSwitchModel::Tomahawk3),
              "tomahawk3");
    EXPECT_EQ(fat_tree_switch_model_from_string("default"),
              FatTreeSwitchModel::Default);
    EXPECT_EQ(fat_tree_switch_model_from_string("tomahawk3"),
              FatTreeSwitchModel::Tomahawk3);
    EXPECT_THROW(fat_tree_switch_model_from_string("tm3"),
                 std::invalid_argument);
}

TEST(FatTreeSwitchFactoryTest, DefaultPreservesCurrentImplementation) {
    EventList& eventlist = EventList::getTheEventList();

    auto switch_instance = FatTreeSwitchFactory::create(
        FatTreeSwitchModel::Default, eventlist, "default-switch",
        FatTreeSwitch::TOR, 7, 0, nullptr);

    ASSERT_NE(switch_instance, nullptr);
    EXPECT_EQ(switch_instance->getType(), FatTreeSwitch::TOR);
    EXPECT_EQ(switch_instance->getID(), 7);
}

TEST(FatTreeSwitchFactoryTest, ConstructsTomahawk3) {
    EventList& eventlist = EventList::getTheEventList();

    auto switch_instance = FatTreeSwitchFactory::create(
        FatTreeSwitchModel::Tomahawk3, eventlist, "tomahawk3-switch",
        FatTreeSwitch::TOR, 0, 0, nullptr, 32768);

    auto* tomahawk3 = dynamic_cast<Tomahawk3Switch*>(switch_instance.get());
    ASSERT_NE(tomahawk3, nullptr);
    EXPECT_EQ(tomahawk3->shared_buffer_capacity(), 32768);
}

}  // namespace
