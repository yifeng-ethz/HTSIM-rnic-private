// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef FAT_TREE_SWITCH_FACTORY_H
#define FAT_TREE_SWITCH_FACTORY_H

#include "fat_tree_switch.h"
#include "fat_tree_switch_model.h"

#include <memory>
#include <string>

class FatTreeSwitchFactory {
public:
    static std::unique_ptr<FatTreeSwitch> create(
        FatTreeSwitchModel model,
        EventList& eventlist,
        const std::string& name,
        FatTreeSwitch::switch_type type,
        uint32_t id,
        simtime_picosec switch_delay,
        FatTreeTopology* topology,
        mem_b shared_buffer_capacity = 0);
};

#endif
