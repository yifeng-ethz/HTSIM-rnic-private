// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ATLAHS_GOODPUT_TRACE_H
#define ATLAHS_GOODPUT_TRACE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "atlahs_flow_runtime.h"

// Sparse receive-side payload accounting shared by the physical comparison
// profiles. Each successful logical delivery contributes exactly once to one
// fixed-width, simulation-epoch-aligned bin. Empty bins are omitted.
class AtlahsGoodputTrace final {
public:
    explicit AtlahsGoodputTrace(std::uint64_t bin_width_ps = 0) noexcept
        : _bin_width_ps(bin_width_ps) {}

    bool enabled() const noexcept { return _bin_width_ps != 0; }
    std::uint64_t binWidthPs() const noexcept { return _bin_width_ps; }
    std::size_t size() const noexcept { return _bins.size(); }

    void record(std::uint64_t delivery_time_ps,
                AtlahsFlowId flow_id,
                std::uint32_t source,
                std::uint32_t destination,
                std::uint64_t delivered_payload_bytes);

    // Write only after physical quiescence. The temporary artifact is closed
    // before rename, so a failed run cannot expose a complete-looking CSV.
    void writeCsvAtomically(const std::string& path) const;

private:
    struct Key {
        std::uint64_t bin_index;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;

        bool operator<(const Key& other) const noexcept;
    };

    std::uint64_t _bin_width_ps;
    std::map<Key, std::uint64_t> _bins;
};

#endif  // ATLAHS_GOODPUT_TRACE_H
