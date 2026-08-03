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

std::invalid_argument optionError(const std::string& option, const std::string& message) {
    return std::invalid_argument(option + ": " + message);
}

std::uint64_t parseUnsigned(const std::string& option, const std::string& text) {
    if (text.empty()) {
        throw optionError(option, "value is empty");
    }
    if (text.front() == '-') {
        throw optionError(option, "negative values are not permitted");
    }

    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec == std::errc::result_out_of_range) {
        throw optionError(option, "value exceeds uint64_t");
    }
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw optionError(option, "expected one unsigned base-10 integer");
    }
    return value;
}

std::uint32_t parseUnsigned32(const std::string& option, const std::string& text) {
    const std::uint64_t value = parseUnsigned(option, text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw optionError(option, "value exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(value);
}

RnicAtlahsGoalRankMapping parseGoalRankMapping(const std::string& value) {
    if (value == "auto") {
        return RnicAtlahsGoalRankMapping::Auto;
    }
    if (value == "gpu-rank") {
        return RnicAtlahsGoalRankMapping::GpuRank;
    }
    if (value == "unique-nic") {
        return RnicAtlahsGoalRankMapping::UniqueNic;
    }
    throw std::invalid_argument("-goal_rank_mapping: expected auto, gpu-rank, or unique-nic");
}

bool isPacketProfile(RnicProfile profile) noexcept {
    return profile == RnicProfile::CollectiveNetwork || profile == RnicProfile::SlingshotLike ||
           profile == RnicProfile::PacketizedManifold;
}

bool isPhysicalClosProfile(RnicProfile profile) noexcept {
    return profile == RnicProfile::CollectiveNetwork || profile == RnicProfile::SlingshotLike;
}

bool isManifoldProfile(RnicProfile profile) noexcept {
    return profile == RnicProfile::PacketizedManifold || profile == RnicProfile::FluidManifold;
}

void requirePositive(std::uint64_t value, const std::string& option) {
    if (value == 0) {
        throw optionError(option, "value must be positive");
    }
}

void validatePacketOptions(const RnicAtlahsCliOptions& options) {
    requirePositive(options.packet.max_wire_packet_bytes, "-rnic_max_wire_bytes");
    if (options.packet.data_header_bytes >= options.packet.max_wire_packet_bytes) {
        throw std::invalid_argument(
            "RNIC DATA header must be smaller than its maximum wire extent");
    }

    constexpr Wide picoseconds_per_second = UINT64_C(1000000000000);
    const Wide full_envelope_numerator =
        static_cast<Wide>(options.packet.max_wire_packet_bytes) * 8 * picoseconds_per_second;
    if (full_envelope_numerator < options.link_capacity_bps) {
        throw std::invalid_argument("RNIC full DATA envelope is shorter than one simulator tick");
    }
}

void validateCollectiveOptions(const RnicAtlahsCliOptions& options) {
    constexpr std::uint64_t htsim_one_byte_per_ps_bps = UINT64_C(8000000000000);
    if (options.link_capacity_bps > htsim_one_byte_per_ps_bps) {
        throw std::invalid_argument("rnic-cn link capacity exceeds HTSIM's physical queue clock");
    }
    if (options.node_count != 0 && !options.collective.topology_file.has_value() &&
        !isRnicGeneratedTwoTierClosNodeCount(options.node_count)) {
        throw std::invalid_argument("generated rnic-cn topology requires nodes = K^2/2 for even K");
    }
    if (options.collective.topology_file.has_value() &&
        (options.explicitly_supplied.hop_latency_ps ||
         options.explicitly_supplied.switch_latency_ps)) {
        throw std::invalid_argument(
            "-topo cannot be combined with explicit RNIC hop/switch latency");
    }

    const Wide diameter = static_cast<Wide>(options.collective.hop_latency_ps) * 4 +
                          static_cast<Wide>(options.collective.switch_latency_ps) * 3;
    if (diameter > std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("rnic-cn two-tier Clos diameter latency overflows uint64_t");
    }

    requirePositive(options.collective.control_deadline_ps, "-rnic_cn_control_deadline_ps");
    if (options.collective.margin_ppm == 0 || options.collective.margin_ppm > 1000000) {
        throw std::invalid_argument("-rnic_cn_margin_ppm: value must be in [1, 1000000]");
    }
    requirePositive(options.collective.control_wire_bytes, "-rnic_cn_control_wire_bytes");
    if (options.collective.control_wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("-rnic_cn_control_wire_bytes: value exceeds uint16_t");
    }
    if (options.packet.max_wire_packet_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("-rnic_max_wire_bytes: rnic-cn extent exceeds uint16_t");
    }
    requirePositive(options.collective.ring_delay_window_ps, "-rnic_cn_ring_window_ps");
    requirePositive(options.collective.ring_release_tick_ps, "-rnic_cn_ring_tick_ps");
    requirePositive(options.collective.ring_wire_capacity_bytes, "-rnic_cn_ring_capacity_bytes");
    requirePositive(options.collective.ns_tm3_shared_buffer_bytes, "-rnic_cn_ns_tm3_buffer_bytes");
    if (options.collective.maximum_retransmissions == 0) {
        throw std::invalid_argument("-rnic_cn_max_retransmissions: value must be positive");
    }
    requirePositive(options.collective.retransmission_rto_ps, "-rnic_cn_retransmission_rto_ps");
    if (options.collective.ns_tm3_shared_buffer_bytes >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("-rnic_cn_ns_tm3_buffer_bytes: value exceeds HTSIM mem_b");
    }

    constexpr Wide wire_byte_denominator = static_cast<Wide>(8) * UINT64_C(1000000000000);
    const Wide window_numerator =
        static_cast<Wide>(options.link_capacity_bps) * options.collective.ring_delay_window_ps;
    const Wide window_bytes =
        (window_numerator + wire_byte_denominator - 1) / wire_byte_denominator;
    const Wide required_ring_capacity = window_bytes + options.packet.max_wire_packet_bytes;
    if (required_ring_capacity > std::numeric_limits<std::uint64_t>::max() ||
        options.collective.ring_wire_capacity_bytes <
            static_cast<std::uint64_t>(required_ring_capacity)) {
        throw std::invalid_argument("rnic-cn Ring-CAM requires W >= ceil(C*Delta/8) + M");
    }
}

void validateSlingshotOptions(const RnicAtlahsCliOptions& options) {
    constexpr std::uint64_t htsim_one_byte_per_ps_bps = UINT64_C(8000000000000);
    if (options.link_capacity_bps > htsim_one_byte_per_ps_bps) {
        throw std::invalid_argument("rnic-ss link capacity exceeds HTSIM's physical queue clock");
    }
    if (options.node_count != 0 && !options.collective.topology_file.has_value() &&
        !isRnicGeneratedTwoTierClosNodeCount(options.node_count)) {
        throw std::invalid_argument("generated rnic-ss topology requires nodes = K^2/2 for even K");
    }
    if (options.collective.topology_file.has_value() &&
        (options.explicitly_supplied.hop_latency_ps ||
         options.explicitly_supplied.switch_latency_ps)) {
        throw std::invalid_argument(
            "-topo cannot be combined with explicit RNIC hop/switch latency");
    }
    const Wide diameter = static_cast<Wide>(options.collective.hop_latency_ps) * 4 +
                          static_cast<Wide>(options.collective.switch_latency_ps) * 3;
    if (diameter > std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("rnic-ss two-tier Clos diameter latency overflows uint64_t");
    }
    if (options.packet.max_wire_packet_bytes > std::numeric_limits<std::uint16_t>::max() ||
        options.slingshot.control_wire_bytes == 0 ||
        options.slingshot.control_wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("rnic-ss DATA/control wire extents must fit uint16_t");
    }
    requirePositive(options.slingshot.ns_rosetta_shared_buffer_bytes,
                    "-rnic_ss_ns_rosetta_buffer_bytes");
    requirePositive(options.slingshot.q_hi_bytes, "-rnic_ss_q_hi_bytes");
    if (options.slingshot.q_lo_bytes >= options.slingshot.q_hi_bytes) {
        throw std::invalid_argument("rnic-ss requires Q_lo strictly below Q_hi");
    }
    if (options.slingshot.q_hi_bytes > options.slingshot.ns_rosetta_shared_buffer_bytes) {
        throw std::invalid_argument("rnic-ss Q_hi cannot exceed the ns-rosetta shared buffer");
    }
    requirePositive(options.slingshot.maximum_sample_age_ps, "-rnic_ss_sample_age_ps");
    if (options.slingshot.credit_quantum_packets == 0 ||
        options.slingshot.outstanding_window_packets == 0 ||
        options.slingshot.outstanding_window_packets > 128 ||
        options.slingshot.maximum_retransmissions == 0) {
        throw std::invalid_argument("rnic-ss credit/window/retry counts are outside their domains");
    }
    requirePositive(options.slingshot.rto_ps, "-rnic_ss_rto_ps");
    requirePositive(options.slingshot.maximum_credit_ahead_bytes,
                    "-rnic_ss_max_credit_ahead_bytes");
    const Wide initial_credit = static_cast<Wide>(options.slingshot.credit_quantum_packets) *
                                options.packet.max_wire_packet_bytes;
    if (initial_credit > options.slingshot.maximum_credit_ahead_bytes) {
        throw std::invalid_argument("rnic-ss credit quantum exceeds the credit-ahead bound");
    }
    for (const std::uint64_t value : {options.slingshot.ns_rosetta_shared_buffer_bytes,
                                      options.slingshot.q_hi_bytes, options.slingshot.q_lo_bytes}) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("rnic-ss byte threshold exceeds HTSIM mem_b");
        }
    }
}

void rejectCrossProfileOptions(const RnicAtlahsCliOptions& options) {
    const RnicAtlahsExplicitCliOptions& supplied = options.explicitly_supplied;
    if (!isPacketProfile(options.profile) &&
        (supplied.max_wire_packet_bytes || supplied.data_header_bytes)) {
        throw std::invalid_argument("packetization options are invalid for rnic-nn-fluid");
    }
    if (!isManifoldProfile(options.profile) && supplied.fixed_propagation_delay_ps) {
        throw std::invalid_argument("-rnic_nn_propagation_ps is valid only for NN profiles");
    }

    const bool supplied_physical =
        supplied.topology_file || supplied.hop_latency_ps || supplied.switch_latency_ps;
    if (!isPhysicalClosProfile(options.profile) && supplied_physical) {
        throw std::invalid_argument("physical Clos options are valid only for rnic-cn or rnic-ss");
    }

    const bool supplied_collective =
        supplied.global_prbs_seed || supplied.control_deadline_ps || supplied.margin_ppm ||
        supplied.control_wire_bytes || supplied.ring_delay_window_ps ||
        supplied.ring_release_tick_ps || supplied.ring_wire_capacity_bytes ||
        supplied.ns_tm3_shared_buffer_bytes || supplied.cn_fractional_nflow ||
        supplied.cn_maximum_retransmissions ||
        supplied.cn_retransmission_rto_ps;
    if (options.profile != RnicProfile::CollectiveNetwork && supplied_collective) {
        throw std::invalid_argument("physical Clos/control options are valid only for rnic-cn");
    }

    const bool supplied_ss = supplied.ss_control_wire_bytes ||
                             supplied.ns_rosetta_shared_buffer_bytes || supplied.ss_q_hi_bytes ||
                             supplied.ss_q_lo_bytes || supplied.ss_telemetry_delay_ps ||
                             supplied.ss_path_hysteresis_ps || supplied.ss_maximum_sample_age_ps ||
                             supplied.ss_credit_quantum_packets ||
                             supplied.ss_outstanding_window_packets || supplied.ss_rto_ps ||
                             supplied.ss_maximum_retransmissions ||
                             supplied.ss_maximum_credit_ahead_bytes || supplied.ss_routing_seed ||
                             supplied.ss_routing || supplied.ss_loss_stress;
    if (options.profile != RnicProfile::SlingshotLike && supplied_ss) {
        throw std::invalid_argument("rnic-ss options are valid only for the rnic-ss profile");
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
    } else if (options.profile == RnicProfile::SlingshotLike) {
        validateSlingshotOptions(options);
    }
}

}  // namespace

RnicAtlahsCliOptions parseRnicAtlahsCli(int argc, const char* const argv[]) {
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        throw std::invalid_argument("RNIC CLI requires a valid program-name argument");
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
        } else if (option == "-completion_csv") {
            if (value.empty()) {
                throw optionError(option, "path must be nonempty");
            }
            options.completion_csv = value;
        } else if (option == "-goal_rank_mapping") {
            options.goal_rank_mapping = parseGoalRankMapping(value);
            options.explicitly_supplied.goal_rank_mapping = true;
        } else if (option == "-nodes") {
            options.node_count = parseUnsigned32(option, value);
            if (options.node_count == 0) {
                throw optionError(option, "explicit validation override must be positive");
            }
            if (options.node_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                throw optionError(option, "value exceeds the ATLAHS API node domain");
            }
            options.explicitly_supplied.node_count = true;
        } else if (option == "-linkspeed_bps") {
            options.link_capacity_bps = parseUnsigned(option, value);
            required.linkspeed = true;
        } else if (option == "-rnic_profile") {
            options.profile = parseRnicProfile(value);
            required.profile = true;
        } else if (option == "-rnic_max_wire_bytes") {
            options.packet.max_wire_packet_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.max_wire_packet_bytes = true;
        } else if (option == "-rnic_data_header_bytes") {
            options.packet.data_header_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.data_header_bytes = true;
        } else if (option == "-rnic_nn_propagation_ps") {
            options.manifold.fixed_propagation_delay_ps = parseUnsigned(option, value);
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
            options.collective.switch_latency_ps = parseUnsigned(option, value);
            options.explicitly_supplied.switch_latency_ps = true;
        } else if (option == "-rnic_cn_prbs_seed") {
            options.collective.global_prbs_seed = parseUnsigned(option, value);
            options.explicitly_supplied.global_prbs_seed = true;
        } else if (option == "-rnic_cn_control_deadline_ps") {
            options.collective.control_deadline_ps = parseUnsigned(option, value);
            options.explicitly_supplied.control_deadline_ps = true;
        } else if (option == "-rnic_cn_margin_ppm") {
            options.collective.margin_ppm = parseUnsigned32(option, value);
            options.explicitly_supplied.margin_ppm = true;
        } else if (option == "-rnic_cn_control_wire_bytes") {
            options.collective.control_wire_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.control_wire_bytes = true;
        } else if (option == "-rnic_cn_ring_window_ps") {
            options.collective.ring_delay_window_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ring_delay_window_ps = true;
        } else if (option == "-rnic_cn_ring_tick_ps") {
            options.collective.ring_release_tick_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ring_release_tick_ps = true;
        } else if (option == "-rnic_cn_ring_capacity_bytes") {
            options.collective.ring_wire_capacity_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ring_wire_capacity_bytes = true;
        } else if (option == "-rnic_cn_ns_tm3_buffer_bytes") {
            options.collective.ns_tm3_shared_buffer_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ns_tm3_shared_buffer_bytes = true;
        } else if (option == "-rnic_cn_fractional_nflow") {
            if (value == "on") {
                options.collective.fractional_nflow = true;
            } else if (value == "off") {
                options.collective.fractional_nflow = false;
            } else {
                throw optionError(option, "value must be on or off");
            }
            options.explicitly_supplied.cn_fractional_nflow = true;
        } else if (option == "-rnic_cn_max_retransmissions") {
            options.collective.maximum_retransmissions = parseUnsigned32(option, value);
            options.explicitly_supplied.cn_maximum_retransmissions = true;
        } else if (option == "-rnic_cn_retransmission_rto_ps") {
            options.collective.retransmission_rto_ps = parseUnsigned(option, value);
            options.explicitly_supplied.cn_retransmission_rto_ps = true;
        } else if (option == "-rnic_ss_control_wire_bytes") {
            options.slingshot.control_wire_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ss_control_wire_bytes = true;
        } else if (option == "-rnic_ss_ns_rosetta_buffer_bytes") {
            options.slingshot.ns_rosetta_shared_buffer_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ns_rosetta_shared_buffer_bytes = true;
        } else if (option == "-rnic_ss_q_hi_bytes") {
            options.slingshot.q_hi_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ss_q_hi_bytes = true;
        } else if (option == "-rnic_ss_q_lo_bytes") {
            options.slingshot.q_lo_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ss_q_lo_bytes = true;
        } else if (option == "-rnic_ss_telemetry_delay_ps") {
            options.slingshot.telemetry_delay_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ss_telemetry_delay_ps = true;
        } else if (option == "-rnic_ss_path_hysteresis_ps") {
            options.slingshot.path_hysteresis_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ss_path_hysteresis_ps = true;
        } else if (option == "-rnic_ss_sample_age_ps") {
            options.slingshot.maximum_sample_age_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ss_maximum_sample_age_ps = true;
        } else if (option == "-rnic_ss_credit_quantum_packets") {
            options.slingshot.credit_quantum_packets = parseUnsigned32(option, value);
            options.explicitly_supplied.ss_credit_quantum_packets = true;
        } else if (option == "-rnic_ss_window_packets") {
            options.slingshot.outstanding_window_packets = parseUnsigned32(option, value);
            options.explicitly_supplied.ss_outstanding_window_packets = true;
        } else if (option == "-rnic_ss_rto_ps") {
            options.slingshot.rto_ps = parseUnsigned(option, value);
            options.explicitly_supplied.ss_rto_ps = true;
        } else if (option == "-rnic_ss_max_retransmissions") {
            options.slingshot.maximum_retransmissions = parseUnsigned32(option, value);
            options.explicitly_supplied.ss_maximum_retransmissions = true;
        } else if (option == "-rnic_ss_max_credit_ahead_bytes") {
            options.slingshot.maximum_credit_ahead_bytes = parseUnsigned(option, value);
            options.explicitly_supplied.ss_maximum_credit_ahead_bytes = true;
        } else if (option == "-rnic_ss_routing_seed") {
            options.slingshot.routing_seed = parseUnsigned(option, value);
            options.explicitly_supplied.ss_routing_seed = true;
        } else if (option == "-rnic_ss_routing") {
            if (value == "unordered") {
                options.slingshot.unordered_packet_routing = true;
            } else if (value == "ordered") {
                options.slingshot.unordered_packet_routing = false;
            } else {
                throw optionError(option, "expected unordered or ordered");
            }
            options.explicitly_supplied.ss_routing = true;
        } else if (option == "-rnic_ss_loss_stress") {
            if (value == "off") {
                options.slingshot.allow_loss_stress = false;
            } else if (value == "on") {
                options.slingshot.allow_loss_stress = true;
            } else {
                throw optionError(option, "expected off or on");
            }
            options.explicitly_supplied.ss_loss_stress = true;
        } else {
            throw optionError(option, "unknown option");
        }
    }

    if (!required.goal) {
        throw std::invalid_argument("missing required option -goal");
    }
    if (!required.linkspeed) {
        throw std::invalid_argument("missing required option -linkspeed_bps");
    }
    if (!required.profile) {
        throw std::invalid_argument("missing required option -rnic_profile");
    }

    validateResolvedOptions(options);
    return options;
}

bool isRnicGeneratedTwoTierClosNodeCount(std::uint32_t node_count) noexcept {
    // Search in integer arithmetic to avoid a floating square-root boundary.
    const std::uint64_t doubled_node_count = static_cast<std::uint64_t>(node_count) * 2;
    for (std::uint64_t radix = 2; radix <= doubled_node_count / radix; radix += 2) {
        if (radix * radix == doubled_node_count) {
            return true;
        }
    }
    return false;
}

const char* rnicAtlahsGoalRankMappingName(RnicAtlahsGoalRankMapping mapping) {
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
    usage << "Usage: " << program_name
          << " -goal FILE"
             " [-completion_csv FILE]"
             " [-goal_rank_mapping auto|gpu-rank|unique-nic]"
             " [-nodes N] -linkspeed_bps BPS"
             " -rnic_profile rnic-cn|rnic-ss|rnic-nn|rnic-nn-fluid\n"
          << "Packet profiles:"
             " [-rnic_max_wire_bytes BYTES]"
             " [-rnic_data_header_bytes BYTES]\n"
          << "Physical profiles:"
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
             " [-rnic_cn_ns_tm3_buffer_bytes BYTES]"
             " [-rnic_cn_fractional_nflow on|off]"
             " [-rnic_cn_max_retransmissions N]"
             " [-rnic_cn_retransmission_rto_ps PS]\n"
          << "rnic-ss: [-topo FILE]"
             " [-rnic_hop_latency_ps PS]"
             " [-rnic_switch_latency_ps PS]"
             " [-rnic_ss_control_wire_bytes BYTES]"
             " [-rnic_ss_ns_rosetta_buffer_bytes BYTES]"
             " [-rnic_ss_q_hi_bytes BYTES]"
             " [-rnic_ss_q_lo_bytes BYTES]"
             " [-rnic_ss_telemetry_delay_ps PS]"
             " [-rnic_ss_path_hysteresis_ps PS]"
             " [-rnic_ss_sample_age_ps PS]"
             " [-rnic_ss_credit_quantum_packets N]"
             " [-rnic_ss_window_packets N]"
             " [-rnic_ss_rto_ps PS]"
             " [-rnic_ss_max_retransmissions N]"
             " [-rnic_ss_max_credit_ahead_bytes BYTES]"
             " [-rnic_ss_routing_seed UINT64]"
             " [-rnic_ss_routing unordered|ordered]"
             " [-rnic_ss_loss_stress off|on]\n";
    return usage.str();
}
