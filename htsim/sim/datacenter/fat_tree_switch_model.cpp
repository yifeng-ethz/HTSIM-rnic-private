// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "fat_tree_switch_model.h"

#include <stdexcept>
#include <string>

std::string_view fat_tree_switch_model_name(FatTreeSwitchModel model) {
    switch (model) {
    case FatTreeSwitchModel::Default:
        return "default";
    case FatTreeSwitchModel::NsTm3:
        return "ns-tm3";
    }

    throw std::invalid_argument("unknown fat-tree switch model");
}

FatTreeSwitchModel fat_tree_switch_model_from_string(std::string_view name) {
    if (name == "default") {
        return FatTreeSwitchModel::Default;
    }
    if (name == "ns-tm3") {
        return FatTreeSwitchModel::NsTm3;
    }

    throw std::invalid_argument("unknown fat-tree switch model: " +
                                std::string(name));
}
