// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "fat_tree_switch_factory.h"
#include "tomahawk3_switch.h"

#include <stdexcept>

std::unique_ptr<FatTreeSwitch> FatTreeSwitchFactory::create(
    FatTreeSwitchModel model,
    EventList& eventlist,
    const std::string& name,
    FatTreeSwitch::switch_type type,
    uint32_t id,
    simtime_picosec switch_delay,
    FatTreeTopology* topology,
    mem_b shared_buffer_capacity) {
    switch (model) {
    case FatTreeSwitchModel::Default:
        return std::make_unique<FatTreeSwitch>(eventlist, name, type, id,
                                               switch_delay, topology);
    case FatTreeSwitchModel::Tomahawk3:
        return std::make_unique<Tomahawk3Switch>(
            eventlist, name, type, id, switch_delay, topology,
            shared_buffer_capacity);
    }

    throw std::invalid_argument("unknown fat-tree switch model");
}
