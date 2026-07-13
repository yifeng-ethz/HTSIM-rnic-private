// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "fat_tree_switch_factory.h"

#include <stdexcept>

std::unique_ptr<FatTreeSwitch> FatTreeSwitchFactory::create(
    FatTreeSwitchModel model,
    EventList& eventlist,
    const std::string& name,
    FatTreeSwitch::switch_type type,
    uint32_t id,
    simtime_picosec switch_delay,
    FatTreeTopology* topology) {
    switch (model) {
    case FatTreeSwitchModel::Default:
        return std::make_unique<FatTreeSwitch>(eventlist, name, type, id,
                                               switch_delay, topology);
    case FatTreeSwitchModel::Tomahawk3:
        throw std::invalid_argument(
            "Tomahawk3 fat-tree switch model is not implemented");
    }

    throw std::invalid_argument("unknown fat-tree switch model");
}
