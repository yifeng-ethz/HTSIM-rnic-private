// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SIMLLM_ATLAHS_FLOW_RUNTIME_H
#define SIMLLM_ATLAHS_FLOW_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atlahs_flow_runtime.h"
#include "eventlist.h"
#include "simllm_htsim_network_port.h"
#include "simllm/rnic/rnic_device.h"
#include "simllm/rnic/session_record.h"

namespace htsim::simllm_rnic {

inline constexpr std::uint32_t kSimllmAtlahsRuntimeConfigVersion = 1;
inline constexpr std::uint32_t kSimllmAtlahsRunRecordVersion = 1;

struct SimllmAtlahsRuntimeConfig {
    std::uint32_t version{kSimllmAtlahsRuntimeConfigVersion};
    std::string session_id;
    std::string transport_policy;
    std::uint64_t seed{0};
    std::string topology_identity;
    std::string htsim_source_revision;
    std::string simllm_source_revision;
    simllm::rnic::RnicDeviceConfig device;
    HtsimNetworkPortConfig port;
    simllm::rnic::RnicAuthoritySelection authority{
        true,
        false};
};

struct SimllmAtlahsRunRecord {
    std::uint32_t version{kSimllmAtlahsRunRecordVersion};
    std::string schema{"htsim-simllm-atlahs-run-v1"};
    std::string session_id;
    simllm::rnic::RnicHardwareMode hardware_mode{
        simllm::rnic::RnicHardwareMode::Structural};
    simllm::rnic::RnicWqeAuthority authority{
        simllm::rnic::RnicWqeAuthority::SimllmNativeRnicSession};
    std::string hardware_config_sha256;
    std::string transport_policy;
    std::uint64_t seed{0};
    std::string topology_identity;
    std::string htsim_source_revision;
    std::string simllm_source_revision;
    simllm::rnic::RnicAuthorityCounters authority_counters;
    std::uint64_t submitted_flows{0};
    std::uint64_t completed_flows{0};
    bool quiescent{false};
};

simllm::rnic::RnicDeviceConfig defaultSimllmAtlahsDeviceConfig();

// One structural ATLAHS session. RnicDevice owns all hardware lifecycle
// state. This class only pumps deterministic event boundaries and correlates
// opaque network tokens with the endpoint that submitted them.
class SimllmAtlahsFlowRuntime final : public AtlahsFlowRuntime,
                                     private EventSource {
public:
    SimllmAtlahsFlowRuntime(
        EventList& event_list,
        SimllmAtlahsRuntimeConfig config);
    ~SimllmAtlahsFlowRuntime() override;

    SimllmAtlahsFlowRuntime(const SimllmAtlahsFlowRuntime&) = delete;
    SimllmAtlahsFlowRuntime& operator=(
        const SimllmAtlahsFlowRuntime&) = delete;

    void setup(
        std::uint32_t node_count,
        CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;
    AtlahsTransportKind transportKind() const noexcept override;
    AtlahsWqeAuthorityMode wqeAuthorityMode() const noexcept override {
        return AtlahsWqeAuthorityMode::NativeRuntime;
    }
    std::optional<AtlahsWqeCompletionProjection> completionProjection(
        AtlahsFlowId flow_id) const override;

    bool isSetup() const noexcept { return setup_; }
    std::uint32_t nodeCount() const noexcept { return node_count_; }
    const simllm::rnic::RnicAuthorityAudit& authorityAudit() const noexcept {
        return authority_audit_;
    }
    const simllm::rnic::RnicSessionConfigRecord& sessionConfigRecord() const;
    const SimllmAtlahsRunRecord& runRecord() const noexcept {
        return run_record_;
    }
    const HtsimNetworkPort& networkPort() const noexcept { return port_; }
    const simllm::rnic::RnicDevice& device(std::uint32_t endpoint) const;
    void validateQuiescent() const;

private:
    struct PendingFlow {
        AtlahsFlowRequest request;
        std::uint32_t endpoint{0};
        simllm::rnic::WqeId wqe_id{0};
    };

    static simllm::rnic::RnicAuthorityAudit makeAuthorityAudit(
        const simllm::rnic::RnicAuthoritySelection& selection);
    static void validateConfig(const SimllmAtlahsRuntimeConfig& config);
    void doNextEvent() override;
    bool isTraffic() override { return true; }
    void driveAt(simllm::rnic::Picoseconds now_ps);
    void retireCompletion(
        std::uint32_t endpoint,
        const simllm::rnic::CompletionEntry& completion);
    std::optional<simllm::rnic::Picoseconds> nextEventTime() const;
    void reschedule();
    void refreshRunRecord();

    SimllmAtlahsRuntimeConfig config_;
    simllm::rnic::RnicAuthorityAudit authority_audit_;
    HtsimNetworkPort port_;
    CompletionHandler complete_flow_;
    bool setup_{false};
    std::uint32_t node_count_{0};
    std::uint64_t next_wr_id_{1};
    std::vector<std::unique_ptr<simllm::rnic::RnicDevice>> devices_;
    std::optional<simllm::rnic::RnicSessionConfigRecord> session_config_;
    SimllmAtlahsRunRecord run_record_;
    std::map<AtlahsFlowId, PendingFlow> pending_flows_;
    std::map<std::pair<std::uint32_t, simllm::rnic::WqeId>, AtlahsFlowId>
        flow_by_wqe_;
    std::map<AtlahsFlowId, AtlahsWqeCompletionProjection>
        completion_projections_;
    std::vector<simllm::rnic::CompletionEntry> polled_completions_;
    std::optional<EventList::Handle> event_handle_;
    std::optional<simllm::rnic::Picoseconds> scheduled_at_ps_;
};

}  // namespace htsim::simllm_rnic

#endif  // SIMLLM_ATLAHS_FLOW_RUNTIME_H
