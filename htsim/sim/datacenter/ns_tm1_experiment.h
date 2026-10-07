// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NS_TM1_EXPERIMENT_H
#define NS_TM1_EXPERIMENT_H

#include "ns_tm1_switch.h"
#include <cstdint>
#include <string>

struct NsTm1ExperimentConfig {
    std::string policy{"quota_dd"};
    std::string source{"4x25"}, receiver{"4x25"}, routing{"balanced"};
    uint32_t mtu{1500};
    uint64_t payload_bytes{1048576}, window_ps{32000000}, seed{1};
    uint32_t fraction_ppm{950000}, rate_scale_ppm{1000000};
    uint64_t source_queue_bytes{262144};
    uint8_t xpe_mask{3};
    NsTm1Config switch_config;
};

struct NsTm1ExperimentResult {
    uint64_t requested_payload_bytes{0}, expected_wire_bytes{0}, quota_overspend_bytes{0};
    std::array<uint64_t, 4> receiver_lane_wire_bytes{{0, 0, 0, 0}};
    std::array<uint64_t, 4> receiver_lane_max_queue_cells{{0, 0, 0, 0}};
    uint64_t generated_packets{0}, generated_payload_bytes{0}, generated_wire_bytes{0};
    uint64_t delivered_packets{0}, delivered_payload_bytes{0}, delivered_wire_bytes{0};
    uint64_t dropped_packets{0}, dropped_payload_bytes{0}, pending_packets{0};
    uint64_t source_drops{0}, switch_drops{0};
    uint64_t source_max_bytes{0}, data_max_cells{0}, xpe_max_cells{0};
    uint64_t completion_ps{0}, latency_mean_ps{0}, latency_p99_ps{0}, latency_max_ps{0};
    uint64_t queue_wait_max_ps{0}, allocated_wire_bps{0}, receiver_capacity_bps{0};
    uint64_t buffered_residence_max_ps{0};
    uint64_t total_windows{0}, maximum_window_grant_wire_bytes{0};
    uint64_t maximum_window_budget_excess_bytes{0}, maximum_deficit_carry_bytes{0};
    uint64_t minimum_receiver_floor_ps{0};
    bool conservation{false}, allocation_constraints{false}, complete_payload{false};
};

NsTm1ExperimentResult runNsTm1Experiment(const NsTm1ExperimentConfig& config);
std::string nsTm1CsvHeader();
std::string nsTm1CsvRow(const NsTm1ExperimentConfig& config, const NsTm1ExperimentResult& result);

#endif
