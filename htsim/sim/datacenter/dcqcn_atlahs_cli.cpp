// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dcqcn_atlahs_cli.h"

#include "config.h"

#include <charconv>
#include <limits>
#include <stdexcept>

namespace {

std::uint64_t parseUnsigned(const std::string& option, const std::string& value) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument(option + ": expected an unsigned integer");
    }
    std::uint64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        throw std::invalid_argument(option + ": invalid unsigned integer");
    }
    return parsed;
}

template <typename Value>
Value checkedValue(const std::string& option, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
        throw std::out_of_range(option + ": value is out of range");
    }
    return static_cast<Value>(value);
}

DcqcnGoalRankMapping parseRankMapping(const std::string& value) {
    if (value == "auto") {
        return DcqcnGoalRankMapping::Auto;
    }
    if (value == "gpu-rank") {
        return DcqcnGoalRankMapping::GpuRank;
    }
    if (value == "unique-nic") {
        return DcqcnGoalRankMapping::UniqueNic;
    }
    throw std::invalid_argument("-goal_rank_mapping: expected auto, gpu-rank, or unique-nic");
}

}  // namespace

DcqcnAtlahsCliOptions parseDcqcnAtlahsCli(int argc, const char* const argv[]) {
    if (argc <= 0 || argv == nullptr) {
        throw std::invalid_argument("invalid DCQCN ATLAHS argument vector");
    }
    DcqcnAtlahsCliOptions options;
    bool ecn_seed_explicit = false;
    bool egress_buffer_explicit = false;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index] == nullptr ? "" : argv[index];
        if (index + 1 >= argc || argv[index + 1] == nullptr) {
            throw std::invalid_argument(option + ": missing value");
        }
        const std::string value = argv[++index];
        if (option == "-goal") {
            options.goal_file = value;
        } else if (option == "-topology") {
            options.runtime.topology_file = value;
        } else if (option == "-completion_csv") {
            if (value.empty()) {
                throw std::invalid_argument("-completion_csv: path must be nonempty");
            }
            options.completion_csv = value;
        } else if (option == "-state_trace_csv") {
            if (value.empty()) {
                throw std::invalid_argument("-state_trace_csv: path must be nonempty");
            }
            options.runtime.state_trace_csv = value;
        } else if (option == "-goodput_trace_csv") {
            if (value.empty()) {
                throw std::invalid_argument("-goodput_trace_csv: path must be nonempty");
            }
            options.runtime.goodput_trace_csv = value;
        } else if (option == "-goodput_trace_bin_ps") {
            options.runtime.goodput_trace_bin_ps = parseUnsigned(option, value);
        } else if (option == "-goal_rank_mapping") {
            options.goal_rank_mapping = parseRankMapping(value);
        } else if (option == "-seed") {
            options.runtime.ecmp_seed = parseUnsigned(option, value);
        } else if (option == "-lgs_o") {
            options.lgs_o_ns = checkedValue<std::uint32_t>(option, parseUnsigned(option, value));
        } else if (option == "-link_bps") {
            options.runtime.endpoint_link_bps = parseUnsigned(option, value);
        } else if (option == "-max_wire_packet_bytes") {
            options.runtime.max_wire_packet_bytes =
                checkedValue<std::uint16_t>(option, parseUnsigned(option, value));
        } else if (option == "-data_header_bytes") {
            options.runtime.data_header_bytes =
                checkedValue<std::uint16_t>(option, parseUnsigned(option, value));
        } else if (option == "-shared_buffer_bytes") {
            options.runtime.ns_tm3_shared_buffer_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
        } else if (option == "-egress_buffer_bytes") {
            options.runtime.ns_tm3_egress_buffer_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
            egress_buffer_explicit = true;
        } else if (option == "-ecn_kmin_bytes") {
            options.runtime.ecn_kmin_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
        } else if (option == "-ecn_kmax_bytes") {
            options.runtime.ecn_kmax_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
        } else if (option == "-ecn_pmax_ppm") {
            options.runtime.ecn_pmax_ppm =
                checkedValue<std::uint32_t>(option, parseUnsigned(option, value));
        } else if (option == "-ecn_seed") {
            options.runtime.ecn_seed = parseUnsigned(option, value);
            ecn_seed_explicit = true;
        } else if (option == "-pfc_low_bytes") {
            options.runtime.pfc_low_threshold_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
        } else if (option == "-pfc_high_bytes") {
            options.runtime.pfc_high_threshold_bytes =
                checkedValue<mem_b>(option, parseUnsigned(option, value));
        } else if (option == "-pfc") {
            if (value == "on") {
                options.runtime.pfc_enabled = true;
            } else if (value == "off") {
                options.runtime.pfc_enabled = false;
            } else {
                throw std::invalid_argument("-pfc: expected on or off");
            }
        } else if (option == "-recovery") {
            if (value == "gbn") {
                options.runtime.selective_repeat = false;
            } else if (value == "sr") {
                options.runtime.selective_repeat = true;
            } else {
                throw std::invalid_argument("-recovery: expected gbn or sr");
            }
        } else if (option == "-sr_window_packets") {
            options.runtime.sr_window_packets =
                checkedValue<std::uint32_t>(option, parseUnsigned(option, value));
        } else if (option == "-loss_rate_cut") {
            if (value == "on") {
                options.runtime.loss_rate_cut = true;
            } else if (value == "off") {
                options.runtime.loss_rate_cut = false;
            } else {
                throw std::invalid_argument("-loss_rate_cut: expected on or off");
            }
        } else if (option == "-silent_rto_us") {
            const std::uint64_t us = parseUnsigned(option, value);
            if (us > std::numeric_limits<simtime_picosec>::max() / UINT64_C(1000000)) {
                throw std::out_of_range(option + ": time overflows ps");
            }
            options.runtime.silent_loss_rto_ps = us * UINT64_C(1000000);
        } else if (option == "-dcqcn_min_rate_bps") {
            options.runtime.dcqcn_min_rate_bps = parseUnsigned(option, value);
        } else {
            throw std::invalid_argument("unknown option " + option);
        }
    }
    if (options.goal_file.empty()) {
        throw std::invalid_argument("missing required option -goal");
    }
    if (options.runtime.topology_file.empty()) {
        throw std::invalid_argument("missing required option -topology");
    }
    if (!ecn_seed_explicit) {
        options.runtime.ecn_seed = options.runtime.ecmp_seed;
    }
    // Preserve the historical single-buffer CLI contract. The new
    // per-egress domain is independent only when its option is explicit;
    // otherwise it inherits the final shared-pool value regardless of option
    // order.
    if (!egress_buffer_explicit) {
        options.runtime.ns_tm3_egress_buffer_bytes = options.runtime.ns_tm3_shared_buffer_bytes;
    }
    if (options.runtime.goodput_trace_csv.has_value() !=
        (options.runtime.goodput_trace_bin_ps != 0)) {
        throw std::invalid_argument(
            "DCQCN goodput trace requires -goodput_trace_csv and positive "
            "-goodput_trace_bin_ps together");
    }
    return options;
}

const char* dcqcnGoalRankMappingName(DcqcnGoalRankMapping mapping) {
    switch (mapping) {
        case DcqcnGoalRankMapping::Auto:
            return "auto";
        case DcqcnGoalRankMapping::GpuRank:
            return "gpu-rank";
        case DcqcnGoalRankMapping::UniqueNic:
            return "unique-nic";
    }
    throw std::invalid_argument("invalid DCQCN GOAL rank mapping");
}

std::string dcqcnAtlahsCliUsage(const std::string& program_name) {
    return "Usage: " + program_name +
           " -goal FILE -topology FILE"
           " [-completion_csv FILE]"
           " [-state_trace_csv FILE]"
           " [-goodput_trace_csv FILE -goodput_trace_bin_ps PS]"
           " [-goal_rank_mapping auto|gpu-rank|unique-nic]"
           " [-seed N] [-link_bps N] [-lgs_o NS]"
           " [-max_wire_packet_bytes N] [-data_header_bytes N]"
           " [-shared_buffer_bytes N] [-egress_buffer_bytes N]"
           " [-ecn_kmin_bytes N] [-ecn_kmax_bytes N]"
           " [-ecn_pmax_ppm N] [-ecn_seed N]"
           " [-pfc_low_bytes N] [-pfc_high_bytes N]"
           " [-pfc on|off] [-recovery gbn|sr]"
           " [-sr_window_packets N] [-loss_rate_cut on|off]"
           " [-silent_rto_us N] [-dcqcn_min_rate_bps N]";
}
