// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "atlahs_state_trace.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <system_error>

namespace {

template <typename Value>
void writeOptional(std::ostream& output,
                   const std::optional<Value>& value) {
    if (value.has_value()) {
        output << *value;
    }
}

void validateEvent(const std::string& event) {
    if (event.empty() || event.find_first_of(",\r\n") != std::string::npos) {
        throw std::invalid_argument(
            "ATLAHS state trace event must be a nonempty CSV token");
    }
}

}  // namespace

void AtlahsStateTrace::append(AtlahsStateTraceRow row) {
    if (!_enabled) {
        return;
    }
    validateEvent(row.event);
    _rows.push_back(std::move(row));
}

void AtlahsStateTrace::writeCsvAtomically(const std::string& path) const {
    if (!_enabled) {
        throw std::logic_error(
            "cannot write a disabled ATLAHS state trace");
    }
    if (path.empty()) {
        throw std::invalid_argument(
            "ATLAHS state trace CSV path must be nonempty");
    }

    const std::filesystem::path final_path(path);
    std::filesystem::path temporary_path = final_path;
    temporary_path += ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
    error.clear();

    try {
        {
            std::ofstream output(
                temporary_path, std::ios::out | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error(
                    "cannot open ATLAHS state trace temporary CSV '"
                    + temporary_path.string() + "'");
            }
            output
                << "time_ps,flow_id,source,destination,event,"
                   "configured_rate_bps,effective_rate_bps,alpha,paused,"
                   "new_packets_sent,rtx_packets_sent,acked_packets\n";
            output << std::setprecision(17);
            for (const AtlahsStateTraceRow& row : _rows) {
                output << row.time_ps << ',' << row.flow_id << ','
                       << row.source << ',' << row.destination << ','
                       << row.event << ',';
                writeOptional(output, row.configured_rate_bps);
                output << ',';
                writeOptional(output, row.effective_rate_bps);
                output << ',';
                writeOptional(output, row.alpha);
                output << ',';
                if (row.paused.has_value()) {
                    output << (*row.paused ? "true" : "false");
                }
                output << ',';
                writeOptional(output, row.new_packets_sent);
                output << ',';
                writeOptional(output, row.rtx_packets_sent);
                output << ',';
                writeOptional(output, row.acked_packets);
                output << '\n';
            }
            output.flush();
            if (!output) {
                throw std::runtime_error(
                    "failed while writing ATLAHS state trace temporary CSV '"
                    + temporary_path.string() + "'");
            }
        }
        std::filesystem::rename(temporary_path, final_path, error);
        if (error) {
            throw std::runtime_error(
                "cannot install ATLAHS state trace CSV '" + path
                + "': " + error.message());
        }
    } catch (...) {
        std::filesystem::remove(temporary_path, error);
        throw;
    }
}
