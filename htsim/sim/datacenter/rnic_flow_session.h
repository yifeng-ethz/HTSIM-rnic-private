// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_FLOW_SESSION_H
#define RNIC_FLOW_SESSION_H

#include <iosfwd>
#include <string>

inline constexpr const char* kRnicFlowSessionSchema =
    "simllm-htsim-flow-session-v1";
inline constexpr const char* kRnicFlowSessionOption = "--flow-session";
inline constexpr unsigned int kRnicFlowSessionMaximumFrameBytes = 1U << 20;

// Run one stdin/stdout framed session. A clean close followed by EOF returns
// zero. Protocol, framing, runtime, and premature-disconnect errors return 2.
int runRnicFlowSession(
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    const std::string& htsim_source_revision,
    const std::string& simllm_source_revision);

#endif  // RNIC_FLOW_SESSION_H
