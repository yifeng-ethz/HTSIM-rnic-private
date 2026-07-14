// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef FAT_TREE_SWITCH_MODEL_H
#define FAT_TREE_SWITCH_MODEL_H

#include <string_view>

enum class FatTreeSwitchModel {
    Default,
    NsTm3,
};

std::string_view fat_tree_switch_model_name(FatTreeSwitchModel model);
FatTreeSwitchModel fat_tree_switch_model_from_string(std::string_view name);

#endif
