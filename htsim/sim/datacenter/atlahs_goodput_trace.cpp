// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "atlahs_goodput_trace.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace {

using Wide = unsigned __int128;

std::uint64_t checkedBinBoundary(std::uint64_t bin_index,
                                 std::uint64_t bin_width_ps) {
    const Wide boundary = static_cast<Wide>(bin_index) * bin_width_ps;
    if (boundary > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("ATLAHS goodput trace bin boundary overflow");
    }
    return static_cast<std::uint64_t>(boundary);
}

std::uint64_t goodputBps(std::uint64_t payload_bytes,
                         std::uint64_t bin_width_ps) {
    constexpr Wide picoseconds_per_second = UINT64_C(1000000000000);
    const Wide bits_per_bin = static_cast<Wide>(payload_bytes) * 8;
    const Wide rate = bits_per_bin * picoseconds_per_second / bin_width_ps;
    if (rate > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("ATLAHS goodput trace rate overflow");
    }
    return static_cast<std::uint64_t>(rate);
}

}  // namespace

bool AtlahsGoodputTrace::Key::operator<(const Key& other) const noexcept {
    return std::tie(bin_index, flow_id, source, destination) <
           std::tie(other.bin_index, other.flow_id, other.source, other.destination);
}

void AtlahsGoodputTrace::record(std::uint64_t delivery_time_ps,
                                AtlahsFlowId flow_id,
                                std::uint32_t source,
                                std::uint32_t destination,
                                std::uint64_t delivered_payload_bytes) {
    if (!enabled() || delivered_payload_bytes == 0) {
        return;
    }
    const Key key{delivery_time_ps / _bin_width_ps, flow_id, source, destination};
    std::uint64_t& total = _bins[key];
    if (delivered_payload_bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::overflow_error("ATLAHS goodput trace payload counter overflow");
    }
    total += delivered_payload_bytes;
}

void AtlahsGoodputTrace::writeCsvAtomically(const std::string& path) const {
    if (!enabled()) {
        throw std::logic_error("cannot write a disabled ATLAHS goodput trace");
    }
    if (path.empty()) {
        throw std::invalid_argument("ATLAHS goodput trace CSV path must be nonempty");
    }

    const std::filesystem::path final_path(path);
    std::filesystem::path temporary_path = final_path;
    temporary_path += ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
    error.clear();

    try {
        {
            std::ofstream output(temporary_path, std::ios::out | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error(
                    "cannot open ATLAHS goodput trace temporary CSV '" +
                    temporary_path.string() + "'");
            }
            output << "bin_start_ps,bin_end_ps,flow_id,source,destination,"
                      "delivered_payload_bytes,goodput_bps\n";
            for (const auto& [key, payload_bytes] : _bins) {
                if (key.bin_index == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("ATLAHS goodput trace bin end overflow");
                }
                const std::uint64_t start = checkedBinBoundary(key.bin_index, _bin_width_ps);
                const std::uint64_t end = checkedBinBoundary(key.bin_index + 1, _bin_width_ps);
                output << start << ',' << end << ',' << key.flow_id << ',' << key.source << ','
                       << key.destination << ',' << payload_bytes << ','
                       << goodputBps(payload_bytes, _bin_width_ps) << '\n';
            }
            output.flush();
            if (!output) {
                throw std::runtime_error(
                    "failed while writing ATLAHS goodput trace temporary CSV '" +
                    temporary_path.string() + "'");
            }
        }
        std::filesystem::rename(temporary_path, final_path, error);
        if (error) {
            throw std::runtime_error("cannot install ATLAHS goodput trace CSV '" + path +
                                     "': " + error.message());
        }
    } catch (...) {
        std::filesystem::remove(temporary_path, error);
        throw;
    }
}
