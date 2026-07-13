// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_atlahs_cli.h"

#include <charconv>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using Wide = unsigned __int128;

struct RequiredOptions {
    bool goal = false;
    bool linkspeed = false;
    bool profile = false;
};

std::invalid_argument optionError(
        const std::string& option, const std::string& message) {
    return std::invalid_argument(option + ": " + message);
}

std::uint64_t parseUnsigned(
        const std::string& option, const std::string& text) {
    if (text.empty()) {
        throw optionError(option, "value is empty");
    }
    if (text.front() == '-') {
        throw optionError(option, "negative values are not permitted");
    }

    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed =
        std::from_chars(begin, end, value, 10);
    if (parsed.ec == std::errc::result_out_of_range) {
        throw optionError(option, "value exceeds uint64_t");
    }
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw optionError(option, "expected one unsigned base-10 integer");
    }
    return value;
}

std::uint32_t parseUnsigned32(
        const std::string& option, const std::string& text) {
    const std::uint64_t value = parseUnsigned(option, text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw optionError(option, "value exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(value);
}

RnicAtlahsGoalRankMapping parseGoalRankMapping(
        const std::string& value) {
    if (value == "auto") {
        return RnicAtlahsGoalRankMapping::Auto;
    }
    if (value == "gpu-rank") {
        return RnicAtlahsGoalRankMapping::GpuRank;
    }
    if (value == "unique-nic") {
        return RnicAtlahsGoalRankMapping::UniqueNic;
    }
    throw std::invalid_argument(
        "-goal_rank_mapping: expected auto, gpu-rank, or unique-nic");
}

bool isPacketProfile(RnicProfile profile) noexcept {
    return profile == RnicProfile::CollectiveNetwork
           || profile == RnicProfile::PacketizedManifold;
}

bool isManifoldProfile(RnicProfile profile) noexcept {
    return profile == RnicProfile::PacketizedManifold
           || profile == RnicProfile::FluidManifold;
}

void requirePositive(
        std::uint64_t value, const std::string& option) {
    if (value == 0) {
        throw optionError(option, "value must be positive");
    }
}

void validatePacketOptions(const RnicAtlahsCliOptions& options) {
    requirePositive(options.packet.max_wire_packet_bytes,
                    "-rnic_max_wire_bytes");
    if (options.packet.data_header_bytes
        >= options.packet.max_wire_packet_bytes) {
        throw std::invalid_argument(
            "RNIC DATA header must be smaller than its maximum wire extent");
    }

    constexpr Wide picoseconds_per_second = UINT64_C(1000000000000);
    const Wide full_envelope_numerator =
        static_cast<Wide>(options.packet.max_wire_packet_bytes)
        * 8 * picoseconds_per_second;
    if (full_envelope_numerator < options.link_capacity_bps) {
        throw std::invalid_argument(
            "RNIC full DATA envelope is shorter than one simulator tick");
    }
}

bool isGeneratedTwoTierNodeCount(std::uint32_t node_count) noexcept {
    // HTSIM's generated two-tier Clos has N = K^2/2 with an even radix K.
    // Search in integer arithmetic to avoid a floating square-root boundary.
    for (std::uint64_t radix = 2;
         radix <= static_cast<std::uint64_t>(node_count) * 2 / radix;
         radix += 2) {
        if (radix * radix == static_cast<std::uint64_t>(node_count) * 2) {
            return true;
        }
    }
    return false;
}

void validateCollectiveOptions(const RnicAtlahsCliOptions& options) {
    constexpr std::uint64_t htsim_one_byte_per_ps_bps =
        UINT64_C(8000000000000);
    if (options.link_capacity_bps > htsim_one_byte_per_ps_bps) {
        throw std::invalid_argument(
            "rnic-cn link capacity exceeds HTSIM's physical queue clock");
    }
    if (options.node_count != 0
        && !options.collective.topology_file.has_value()
        && !isGeneratedTwoTierNodeCount(options.node_count)) {
        throw std::invalid_argument(
            "generated rnic-cn topology requires nodes = K^2/2 for even K");
    }
    if (options.collective.topology_file.has_value()
        && (options.explicitly_supplied.hop_latency_ps
            || options.explicitly_supplied.switch_latency_ps)) {
        throw std::invalid_argument(
            "-topo cannot be combined with explicit RNIC hop/switch latency");
    }

    const Wide diameter =
        static_cast<Wide>(options.collective.hop_latency_ps) * 4
        + static_cast<Wide>(options.collective.switch_latency_ps) * 3;
    if (diameter > std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument(
            "rnic-cn two-tier Clos diameter latency overflows uint64_t");
    }

    requirePositive(options.collective.control_deadline_ps,
                    "-rnic_cn_control_deadline_ps");
    if (options.collective.margin_ppm == 0
        || options.collective.margin_ppm > 1000000) {
        throw std::invalid_argument(
            "-rnic_cn_margin_ppm: value must be in [1, 1000000]");
    }
    requirePositive(options.collective.control_wire_bytes,
                    "-rnic_cn_control_wire_bytes");
    if (options.collective.control_wire_bytes
        > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument(
            "-rnic_cn_control_wire_bytes: value exceeds uint16_t");
    }
    if (options.packet.max_wire_packet_bytes
        > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument(
            "-rnic_max_wire_bytes: rnic-cn extent exceeds uint16_t");
    }
    requirePositive(options.collective.ring_delay_window_ps,
                    "-rnic_cn_ring_window_ps");
    requirePositive(options.collective.ring_release_tick_ps,
                    "-rnic_cn_ring_tick_ps");
    requirePositive(options.collective.ring_wire_capacity_bytes,
                    "-rnic_cn_ring_capacity_bytes");
    requirePositive(options.collective.tomahawk3_shared_buffer_bytes,
                    "-rnic_cn_tomahawk3_buffer_bytes");
    if (options.collective.tomahawk3_shared_buffer_bytes
        > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument(
            "-rnic_cn_tomahawk3_buffer_bytes: value exceeds HTSIM mem_b");
    }

    constexpr Wide wire_byte_denominator =
        static_cast<Wide>(8) * UINT64_C(1000000000000);
    const Wide window_numerator =
        static_cast<Wide>(options.link_capacity_bps)
        * options.collective.ring_delay_window_ps;
    const Wide window_bytes =
        (window_numerator + wire_byte_denominator - 1)
        / wire_byte_denominator;
    const Wide required_ring_capacity =
        window_bytes + options.packet.max_wire_packet_bytes;
    if (required_ring_capacity > std::numeric_limits<std::uint64_t>::max()
        || options.collective.ring_wire_capacity_bytes
               < static_cast<std::uint64_t>(required_ring_capacity)) {
        throw std::invalid_argument(
            "rnic-cn Ring-CAM requires W >= ceil(C*Delta/8) + M");
    }
}

void rejectCrossProfileOptions(const RnicAtlahsCliOptions& options) {
    const RnicAtlahsExplicitCliOptions& supplied =
        options.explicitly_supplied;
    if (!isPacketProfile(options.profile)
        && (supplied.max_wire_packet_bytes || supplied.data_header_bytes)) {
        throw std::invalid_argument(
            "packetization options are invalid for rnic-nn-fluid");
    }
    if (!isManifoldProfile(options.profile)
        && supplied.fixed_propagation_delay_ps) {
        throw std::invalid_argument(
            "-rnic_nn_propagation_ps is valid only for NN profiles");
    }

    const bool supplied_collective =
        supplied.topology_file
        || supplied.hop_latency_ps
        || supplied.switch_latency_ps
        || supplied.global_prbs_seed
        || supplied.control_deadline_ps
        || supplied.margin_ppm
        || supplied.control_wire_bytes
        || supplied.ring_delay_window_ps
        || supplied.ring_release_tick_ps
        || supplied.ring_wire_capacity_bytes
        || supplied.tomahawk3_shared_buffer_bytes;
    if (options.profile != RnicProfile::CollectiveNetwork
        && supplied_collective) {
        throw std::invalid_argument(
            "physical Clos/control options are valid only for rnic-cn");
    }
}

void validateResolvedOptions(const RnicAtlahsCliOptions& options) {
    if (options.goal_file.empty()) {
        throw optionError("-goal", "path must be nonempty");
    }
    requirePositive(options.link_capacity_bps, "-linkspeed_bps");

    rejectCrossProfileOptions(options);
    if (isPacketProfile(options.profile)) {
        validatePacketOptions(options);
    }
    if (options.profile == RnicProfile::CollectiveNetwork) {
        validateCollectiveOptions(options);
    }
}

}  // namespace

RnicAtlahsCliOptions parseRnicAtlahsCli(
        int argc, const char* const argv[]) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        throw std::invalid_argument(
            "RNIC CLI requires a valid program-name argument");
    }

    RnicAtlahsCliOptions options{};
    RequiredOptions required;
    std::set<std::string> seen_options;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            throw std::invalid_argument("RNIC CLI contains a null argument");
        }
        const std::string option(argv[index]);
        if (!seen_options.insert(option).second) {
            throw optionError(option, "duplicate option");
        }
        if (index + 1 >= argc || argv[index + 1] == nullptr) {
            throw optionError(option, "missing value");
        }
        const std::string value(argv[++index]);

        if (option == "-goal") {
            options.goal_file = value;
            required.goal = true;
        } else if (option == "-goal_rank_mapping") {
            options.goal_rank_mapping = parseGoalRankMapping(value);
            options.explicitly_supplied.goal_rank_mapping = true;
        } else if (option == "-nodes") {
            options.node_count = parseUnsigned32(option, value);
            if (options.node_count == 0) {
                throw optionError(
                    option, "explicit validation override must be positive");
            }
            options.explicitly_supplied.node_count = true;
        } else if (option == "-linkspeed_bps") {
            options.link_capacity_bps = parseUnsigned(option, value);
            required.linkspeed = true;
        } else if (option == "-rnic_profile") {
            options.profile = parseRnicProfile(value);
            required.profile = true;
        } else if (option == "-rnic_max_wire_bytes") {
            options.packet.max_wire_packet_bytes =
                parseUnsigned(option, value);
            options.explicitly_supplied.max_wire_packet_bytes = true;
        } else if (option == "-rnic_data_header_bytes") {
            options.packet.data_header_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.data_header_bytes = true;
        } else if (option == "-rnic_nn_propagation_ps") {
            options.manifold.fixed_propagation_delay_ps =
                parseUnsigned(option, value);
            options.explicitly_supplied.fixed_propagation_delay_ps = true;
        } else if (option == "-topo") {
            if (value.empty()) {
                throw optionError(option, "path must be nonempty");
            }
            options.collective.topology_file = value;
            options.explicitly_supplied.topology_file = true;
        } else if (option == "-rnic_hop_latency_ps") {
            options.collective.hop_latency_ps = parseUnsigned(option, value);
            options.explicitly_supplied.hop_latency_ps = true;
        } else if (option == "-rnic_switch_latency_ps") {
            options.collective.switch_latency_ps =
                parseUnsigned(option, value);
            options.explicitly_supplied.switch_latency_ps = true;
        } else if (option == "-rnic_cn_prbs_seed") {
            options.collective.global_prbs_seed = parseUnsigned(option, value);
            options.explicitly_supplied.global_prbs_seed = true;
        } else if (option == "-rnic_cn_control_deadline_ps") {
            options.collective.control_deadline_ps =
                parseUnsigned(option, value);
            options.explicitly_supplied.control_deadline_ps = true;
        } else if (option == "-rnic_cn_margin_ppm") {
            options.collective.margin_ppm = parseUnsigned32(option, value);
            options.explicitly_supplied.margin_ppm = true;
        } else if (option == "-rnic_cn_control_wire_bytes") {
            options.collective.control_wire_bytes =
                parseUnsigned(option, value);
            options.explicitly_supplied.control_wire_bytes = true;
        } else if (option == "-rnic_cn_ring_window_ps") {
            options.collective.ring_delay_window_ps =
                parseUnsigned(option, value);
            options.explicitly_supplied.ring_delay_window_ps = true;
        } else if (option == "-rnic_cn_ring_tick_ps") {
            options.collective.ring_release_tick_ps =
                parseUnsigned(option, value);
            options.explicitly_supplied.ring_release_tick_ps = true;
        } else if (option == "-rnic_cn_ring_capacity_bytes") {
            options.collective.ring_wire_capacity_bytes =
                parseUnsigned(option, value);
            options.explicitly_supplied.ring_wire_capacity_bytes = true;
        } else if (option == "-rnic_cn_tomahawk3_buffer_bytes") {
            options.collective.tomahawk3_shared_buffer_bytes =
                parseUnsigned(option, value);
            options.explicitly_supplied.tomahawk3_shared_buffer_bytes = true;
        } else {
            throw optionError(option, "unknown option");
        }
    }

    if (!required.goal) {
        throw std::invalid_argument("missing required option -goal");
    }
    if (!required.linkspeed) {
        throw std::invalid_argument(
            "missing required option -linkspeed_bps");
    }
    if (!required.profile) {
        throw std::invalid_argument(
            "missing required option -rnic_profile");
    }

    validateResolvedOptions(options);
    return options;
}

const char* rnicAtlahsGoalRankMappingName(
        RnicAtlahsGoalRankMapping mapping) {
    switch (mapping) {
    case RnicAtlahsGoalRankMapping::Auto:
        return "auto";
    case RnicAtlahsGoalRankMapping::GpuRank:
        return "gpu-rank";
    case RnicAtlahsGoalRankMapping::UniqueNic:
        return "unique-nic";
    }
    throw std::invalid_argument("invalid GOAL rank-mapping enum");
}

std::string rnicAtlahsCliUsage(const std::string& program_name) {
    std::ostringstream usage;
    usage
        << "Usage: " << program_name
        << " -goal FILE"
           " [-goal_rank_mapping auto|gpu-rank|unique-nic]"
           " [-nodes N] -linkspeed_bps BPS"
           " -rnic_profile rnic-cn|rnic-nn|rnic-nn-fluid\n"
        << "Packet profiles:"
           " [-rnic_max_wire_bytes BYTES]"
           " [-rnic_data_header_bytes BYTES]\n"
        << "NN profiles: [-rnic_nn_propagation_ps PS]\n"
        << "rnic-cn: [-topo FILE]"
           " [-rnic_hop_latency_ps PS]"
           " [-rnic_switch_latency_ps PS]"
           " [-rnic_cn_prbs_seed UINT64]"
           " [-rnic_cn_control_deadline_ps PS]"
           " [-rnic_cn_margin_ppm PPM]"
           " [-rnic_cn_control_wire_bytes BYTES]"
           " [-rnic_cn_ring_window_ps PS]"
           " [-rnic_cn_ring_tick_ps PS]"
           " [-rnic_cn_ring_capacity_bytes BYTES]"
           " [-rnic_cn_tomahawk3_buffer_bytes BYTES]";
    return usage.str();
}
