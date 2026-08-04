// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ATLAHS_STATE_TRACE_H
#define ATLAHS_STATE_TRACE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atlahs_flow_runtime.h"

// Sparse, event-driven sender state shared by the physical comparison
// profiles.  A missing value is emitted as an empty CSV field; profiles must
// not invent samples or substitute unrelated state merely to fill a column.
struct AtlahsStateTraceRow {
    std::uint64_t time_ps{0};
    AtlahsFlowId flow_id{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    std::string event;
    std::optional<std::uint64_t> configured_rate_bps;
    std::optional<std::uint64_t> effective_rate_bps;
    std::optional<double> alpha;
    std::optional<bool> paused;
    std::optional<std::uint64_t> new_packets_sent;
    std::optional<std::uint64_t> rtx_packets_sent;
    std::optional<std::uint64_t> acked_packets;
};

class AtlahsStateTrace final {
public:
    explicit AtlahsStateTrace(bool enabled = false) noexcept
        : _enabled(enabled) {}

    bool enabled() const noexcept { return _enabled; }
    std::size_t size() const noexcept { return _rows.size(); }
    const std::vector<AtlahsStateTraceRow>& rows() const noexcept {
        return _rows;
    }

    void append(AtlahsStateTraceRow row);

    // Write only after the simulation has proved physical quiescence.  The
    // final pathname is installed by rename after the stream has closed, so a
    // failed simulation never leaves a seemingly complete trace artifact.
    void writeCsvAtomically(const std::string& path) const;

private:
    bool _enabled;
    std::vector<AtlahsStateTraceRow> _rows;
};

#endif  // ATLAHS_STATE_TRACE_H
