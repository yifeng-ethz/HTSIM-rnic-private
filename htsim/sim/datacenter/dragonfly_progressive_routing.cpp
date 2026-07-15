// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dragonfly_progressive_routing.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace htsim {
namespace {

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

bool isAdaptive(DragonflyRoutingControl control) noexcept {
    return control == DragonflyRoutingControl::Adaptive0 ||
           control == DragonflyRoutingControl::Adaptive1 ||
           control == DragonflyRoutingControl::Adaptive2 ||
           control == DragonflyRoutingControl::Adaptive3;
}

DragonflyVirtualChannel dependencyPhaseForHop(std::uint8_t router_hops) {
    if (router_hops > 7) {
        throw std::out_of_range("Dragonfly route exceeded its logical dependency phases");
    }
    return static_cast<DragonflyVirtualChannel>(router_hops);
}

}  // namespace

std::uint8_t DragonflyCongestionComponents::compose() const {
    if (near_end > 15 || far_end > 15 || downstream > 15) {
        throw std::invalid_argument("Dragonfly congestion inputs must be four-bit values");
    }
    const unsigned int total = static_cast<unsigned int>(near_end) + far_end + downstream;
    return static_cast<std::uint8_t>(std::min(total, 15U));
}

void DragonflyAdaptiveBias::validate() const {
    const auto validate_class = [](const DragonflyClassBias& bias) {
        if (bias.shift > 2 || bias.additive > 63) {
            throw std::invalid_argument("Dragonfly bias exceeds its disclosed six-bit domain");
        }
    };
    validate_class(minimal);
    validate_class(non_minimal);
}

void DragonflyProgressiveConfig::validate() const {
    if (routers_per_group == 0 || global_links_per_router == 0) {
        throw std::invalid_argument("Dragonfly requires nonzero a and h");
    }
    if (maximum_router_hops != 8) {
        throw std::invalid_argument(
            "progressive Dragonfly logical dependency mapping requires eight hops");
    }
    if (minimum_route_update_delay_ps == 0) {
        throw std::invalid_argument("Dragonfly route updates require a nonzero physical delay");
    }
    for (const DragonflyAdaptiveBias& bias : adaptive_biases) {
        bias.validate();
    }
}

DragonflyCanonicalTopology::DragonflyCanonicalTopology(std::uint16_t routers_per_group,
                                                       std::uint16_t global_links_per_router)
    : _a(routers_per_group), _h(global_links_per_router), _groups(0) {
    if (_a == 0 || _h == 0) {
        throw std::invalid_argument("canonical Dragonfly requires nonzero a and h");
    }
    const std::uint64_t groups = static_cast<std::uint64_t>(_a) * _h + 1;
    const std::uint64_t routers = groups * _a;
    if (groups > std::numeric_limits<DragonflyGroupId>::max() ||
        routers > std::numeric_limits<DragonflyRouterId>::max() ||
        static_cast<std::uint64_t>(_a - 1) + _h > std::numeric_limits<DragonflyPortId>::max()) {
        throw std::overflow_error("canonical Dragonfly dimensions exceed ID domains");
    }
    _groups = static_cast<DragonflyGroupId>(groups);
    _ports.resize(static_cast<std::size_t>(routers));

    for (DragonflyRouterId router = 0; router < routerCount(); ++router) {
        const DragonflyGroupId group = groupOf(router);
        const DragonflyRouterId group_base = static_cast<DragonflyRouterId>(group) * _a;
        for (std::uint16_t local = 0; local < _a; ++local) {
            const DragonflyRouterId next = group_base + local;
            if (next == router) {
                continue;
            }
            const DragonflyPortId port = static_cast<DragonflyPortId>(_ports[router].size());
            _ports[router].push_back({port, next, DragonflyLinkKind::Local});
        }

        const std::uint16_t local_index = static_cast<std::uint16_t>(router - group_base);
        for (std::uint16_t global = 0; global < _h; ++global) {
            const std::uint32_t right_steps =
                static_cast<std::uint32_t>(local_index) * _h + global + 1;
            const DragonflyGroupId target_group = static_cast<DragonflyGroupId>(
                (static_cast<std::uint32_t>(group) + right_steps) % _groups);
            const DragonflyRouterId next = static_cast<DragonflyRouterId>(target_group) * _a +
                                           static_cast<DragonflyRouterId>(_a - 1 - local_index);
            const DragonflyPortId port = static_cast<DragonflyPortId>(_ports[router].size());
            _ports[router].push_back({port, next, DragonflyLinkKind::Global});
        }
    }

    for (DragonflyRouterId router = 0; router < routerCount(); ++router) {
        for (const DragonflyPort& output : _ports[router]) {
            const auto reverse = portTo(output.next_router, router);
            if (!reverse.has_value() || port(output.next_router, *reverse).kind != output.kind) {
                throw std::logic_error("canonical Dragonfly constructed a non-reciprocal link");
            }
        }
    }
}

void DragonflyCanonicalTopology::validateRouter(DragonflyRouterId router) const {
    if (router >= routerCount()) {
        throw std::out_of_range("Dragonfly router ID is outside the topology");
    }
}

void DragonflyCanonicalTopology::validateGroup(DragonflyGroupId group) const {
    if (group >= _groups) {
        throw std::out_of_range("Dragonfly group ID is outside the topology");
    }
}

DragonflyGroupId DragonflyCanonicalTopology::groupOf(DragonflyRouterId router) const {
    validateRouter(router);
    return static_cast<DragonflyGroupId>(router / _a);
}

const std::vector<DragonflyPort>& DragonflyCanonicalTopology::ports(
    DragonflyRouterId router) const {
    validateRouter(router);
    return _ports[router];
}

const DragonflyPort& DragonflyCanonicalTopology::port(DragonflyRouterId router,
                                                      DragonflyPortId port_id) const {
    validateRouter(router);
    if (port_id >= _ports[router].size()) {
        throw std::out_of_range("Dragonfly port ID is outside the router");
    }
    return _ports[router][port_id];
}

std::optional<DragonflyPortId> DragonflyCanonicalTopology::portTo(
    DragonflyRouterId router,
    DragonflyRouterId next_router) const {
    validateRouter(router);
    validateRouter(next_router);
    for (const DragonflyPort& output : _ports[router]) {
        if (output.next_router == next_router) {
            return output.id;
        }
    }
    return std::nullopt;
}

std::optional<DragonflyPortId> DragonflyCanonicalTopology::globalPortToGroup(
    DragonflyRouterId router,
    DragonflyGroupId group) const {
    validateRouter(router);
    validateGroup(group);
    for (const DragonflyPort& output : _ports[router]) {
        if (output.kind == DragonflyLinkKind::Global && groupOf(output.next_router) == group) {
            return output.id;
        }
    }
    return std::nullopt;
}

DragonflyRouterId DragonflyCanonicalTopology::groupConnector(DragonflyGroupId source_group,
                                                             DragonflyGroupId target_group) const {
    validateGroup(source_group);
    validateGroup(target_group);
    if (source_group == target_group) {
        throw std::invalid_argument("a group has no global connector to itself");
    }
    const std::uint32_t right_steps =
        source_group < target_group
            ? static_cast<std::uint32_t>(target_group - source_group)
            : static_cast<std::uint32_t>(_groups + target_group - source_group);
    return static_cast<DragonflyRouterId>(source_group) * _a + (right_steps - 1) / _h;
}

DragonflyPortId DragonflyCanonicalTopology::reciprocalPort(DragonflyRouterId router,
                                                           DragonflyPortId port_id) const {
    const DragonflyPort& output = port(router, port_id);
    const auto reverse = portTo(output.next_router, router);
    if (!reverse.has_value()) {
        throw std::logic_error("Dragonfly link has no reciprocal port");
    }
    return *reverse;
}

DragonflyProgressiveRouting::DragonflyProgressiveRouting(DragonflyProgressiveConfig config)
    : _config(std::move(config)),
      _topology(_config.routers_per_group, _config.global_links_per_router) {
    _config.validate();
    _congestion.resize(_topology.routerCount());
    _link_alive.resize(_topology.routerCount());
    for (DragonflyRouterId router = 0; router < _topology.routerCount(); ++router) {
        const std::size_t count = _topology.ports(router).size();
        _congestion[router].resize(count);
        _link_alive[router].assign(count, true);
    }
    const std::size_t routers = _topology.routerCount();
    const std::size_t groups = _topology.groupCount();
    _destination_available.assign(routers, std::vector<bool>(groups, true));
    _safe_groups.assign(routers, std::vector<bool>(groups, true));
    _local_minimal_diverged.assign(routers, std::vector<bool>(routers, false));
    _destination_update_sequences.assign(routers,
                                         std::vector<std::optional<std::uint64_t>>(groups));
    _safe_group_update_sequences.assign(routers, std::vector<std::optional<std::uint64_t>>(groups));
    _latest_route_update_time_ps.resize(routers);
}

DragonflyRouteState DragonflyProgressiveRouting::initialize(DragonflyRouterId source,
                                                            DragonflyRouterId target,
                                                            DragonflyRoutingControl control,
                                                            std::uint64_t route_hash) const {
    (void)_topology.groupOf(source);
    (void)_topology.groupOf(target);
    DragonflyRouteState state;
    state.source_router = source;
    state.target_router = target;
    state.expected_current_router = source;
    state.control = control;
    state.route_hash = route_hash;
    return state;
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::pathToGroup(
    DragonflyTableFamily family,
    DragonflyRouterId current,
    DragonflyGroupId target_group,
    bool require_current_connector,
    std::optional<DragonflyGroupId> intermediate_group) const {
    const DragonflyGroupId current_group = _topology.groupOf(current);
    if (current_group == target_group) {
        return {};
    }
    // This is the route-table view installed at the current router, not a
    // query of a remote connector's live link state.
    if (!destinationAvailable(current, target_group)) {
        return {};
    }
    const DragonflyRouterId connector = _topology.groupConnector(current_group, target_group);
    if (require_current_connector && current != connector) {
        return {};
    }
    DragonflyPortId output;
    if (current == connector) {
        const auto global = _topology.globalPortToGroup(current, target_group);
        if (!global.has_value()) {
            throw std::logic_error("Dragonfly connector lacks its global port");
        }
        output = *global;
    } else {
        const auto local = _topology.portTo(current, connector);
        if (!local.has_value()) {
            throw std::logic_error("Dragonfly group is not a local full mesh");
        }
        output = *local;
    }

    return {{family, output, _topology.port(current, output).next_router, intermediate_group,
             std::nullopt, require_current_connector, false}};
}

bool DragonflyProgressiveRouting::physicalLinkAlive(DragonflyRouterId router,
                                                    DragonflyPortId port_id) const {
    // Port state is directed.  A reverse-direction failure is not an oracle
    // for this output and reaches its route table only through an update.
    return portAlive(router, port_id);
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::legalEntries(
    DragonflyRouterId current,
    const std::vector<DragonflyTableEntry>& entries) const {
    std::vector<DragonflyTableEntry> result;
    result.reserve(entries.size());
    for (const DragonflyTableEntry& entry : entries) {
        if (entry.root_detect ||
            (entry.output_port.has_value() && physicalLinkAlive(current, *entry.output_port))) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::globalMinimal(
    DragonflyRouterId current,
    DragonflyGroupId target_group,
    bool restricted) const {
    return legalEntries(current, pathToGroup(DragonflyTableFamily::GlobalMinimal, current,
                                             target_group, restricted, std::nullopt));
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::globalNonMinimal(
    DragonflyRouterId current,
    DragonflyGroupId final_target_group,
    bool require_current_global_output) const {
    const DragonflyGroupId source_group = _topology.groupOf(current);
    std::vector<DragonflyTableEntry> result;
    for (DragonflyGroupId group = 0; group < _topology.groupCount(); ++group) {
        if (group == source_group || group == final_target_group || !groupSafe(current, group) ||
            !destinationAvailable(current, group)) {
            continue;
        }
        std::vector<DragonflyTableEntry> entry =
            pathToGroup(DragonflyTableFamily::GlobalNonMinimal, current, group,
                        require_current_global_output, group);
        result.insert(result.end(), entry.begin(), entry.end());
    }
    return legalEntries(current, result);
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::localMinimal(
    DragonflyRouterId current,
    DragonflyRouterId target) const {
    if (_topology.groupOf(current) != _topology.groupOf(target)) {
        return {};
    }
    if (current == target) {
        return {{DragonflyTableFamily::LocalMinimal, std::nullopt, current, std::nullopt,
                 std::nullopt, false, true, _local_minimal_diverged[current][target]}};
    }
    const auto output = _topology.portTo(current, target);
    if (!output.has_value()) {
        throw std::logic_error("Dragonfly local minimal route is missing");
    }
    return legalEntries(
        current, {{DragonflyTableFamily::LocalMinimal, *output, target, std::nullopt, std::nullopt,
                   false, false, _local_minimal_diverged[current][target]}});
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::adaptiveLocalMinimal(
    DragonflyRouterId current,
    DragonflyRouterId target) const {
    std::vector<DragonflyTableEntry> entries = localMinimal(current, target);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const DragonflyTableEntry& entry) { return entry.diverged; }),
                  entries.end());
    return entries;
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::localNonMinimal(
    DragonflyRouterId current,
    DragonflyRouterId target,
    std::optional<DragonflyRouterId> committed_root) const {
    if (_topology.groupOf(current) != _topology.groupOf(target)) {
        throw std::invalid_argument("local non-minimal table target is in another group");
    }
    if (committed_root.has_value()) {
        if (_topology.groupOf(*committed_root) != _topology.groupOf(current)) {
            throw std::invalid_argument("local non-minimal root is in another group");
        }
        if (current == *committed_root) {
            return {{DragonflyTableFamily::LocalNonMinimal, std::nullopt, current, std::nullopt,
                     committed_root, false, true}};
        }
        const auto output = _topology.portTo(current, *committed_root);
        if (!output.has_value()) {
            throw std::logic_error("Dragonfly local root is unreachable");
        }
        return legalEntries(current,
                            {{DragonflyTableFamily::LocalNonMinimal, *output, *committed_root,
                              std::nullopt, committed_root, false, false}});
    }

    const DragonflyRouterId base =
        static_cast<DragonflyRouterId>(_topology.groupOf(current)) * _topology.routersPerGroup();
    std::vector<DragonflyTableEntry> result;
    for (std::uint16_t index = 0; index < _topology.routersPerGroup(); ++index) {
        const DragonflyRouterId root = base + index;
        if (root == current) {
            result.push_back({DragonflyTableFamily::LocalNonMinimal, std::nullopt, current,
                              std::nullopt, root, false, true});
            continue;
        }
        const auto output = _topology.portTo(current, root);
        if (!output.has_value()) {
            throw std::logic_error("Dragonfly local non-minimal root is unreachable");
        }
        result.push_back({DragonflyTableFamily::LocalNonMinimal, *output, root, std::nullopt, root,
                          false, false});
    }
    return legalEntries(current, result);
}

void DragonflyProgressiveRouting::setPortCongestion(DragonflyRouterId router,
                                                    DragonflyPortId port,
                                                    DragonflyCongestionComponents congestion) {
    (void)congestion.compose();
    (void)_topology.port(router, port);
    _congestion[router][port] = congestion;
}

DragonflyCongestionComponents DragonflyProgressiveRouting::portCongestion(
    DragonflyRouterId router,
    DragonflyPortId port) const {
    (void)_topology.port(router, port);
    return _congestion[router][port];
}

void DragonflyProgressiveRouting::setPortAlive(DragonflyRouterId router,
                                               DragonflyPortId port,
                                               bool alive) {
    (void)_topology.port(router, port);
    _link_alive[router][port] = alive;
}

void DragonflyProgressiveRouting::setPhysicalLinkAlive(DragonflyRouterId router,
                                                       DragonflyPortId port,
                                                       bool alive) {
    const DragonflyPort& output = _topology.port(router, port);
    const DragonflyPortId reverse = _topology.reciprocalPort(router, port);
    setPortAlive(router, port, alive);
    setPortAlive(output.next_router, reverse, alive);
}

void DragonflyProgressiveRouting::setLocalMinimalDiverged(DragonflyRouterId current,
                                                          DragonflyRouterId target,
                                                          bool diverged) {
    if (_topology.groupOf(current) != _topology.groupOf(target)) {
        throw std::invalid_argument("Dragonfly LM diverged bit requires a local target");
    }
    _local_minimal_diverged[current][target] = diverged;
}

bool DragonflyProgressiveRouting::portAlive(DragonflyRouterId router, DragonflyPortId port) const {
    (void)_topology.port(router, port);
    return _link_alive[router][port];
}

DragonflyRouteUpdateDisposition DragonflyProgressiveRouting::consumeRouteUpdate(
    const DragonflyRouteUpdateAdvertisement& advertisement,
    std::uint64_t current_time_ps) {
    (void)_topology.groupOf(advertisement.observer_router);
    if (advertisement.group >= _topology.groupCount()) {
        throw std::out_of_range("Dragonfly route-update group is outside the topology");
    }
    if (advertisement.physical_arrival_time_ps < advertisement.observation_time_ps ||
        advertisement.physical_arrival_time_ps - advertisement.observation_time_ps <
            _config.minimum_route_update_delay_ps) {
        throw std::invalid_argument("Dragonfly route update violates its physical-delay floor");
    }
    if (current_time_ps < advertisement.observation_time_ps) {
        throw std::invalid_argument(
            "Dragonfly route update cannot be consumed before its observation");
    }
    std::optional<std::uint64_t>& latest_time =
        _latest_route_update_time_ps[advertisement.observer_router];
    if (latest_time.has_value() && current_time_ps < *latest_time) {
        throw std::invalid_argument(
            "Dragonfly route-update time cannot move backwards at a router");
    }
    latest_time = current_time_ps;
    if (current_time_ps < advertisement.physical_arrival_time_ps) {
        return DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived;
    }

    auto& sequences = advertisement.kind == DragonflyRouteUpdateKind::DestinationAvailable
                          ? _destination_update_sequences
                          : _safe_group_update_sequences;
    std::optional<std::uint64_t>& installed =
        sequences[advertisement.observer_router][advertisement.group];
    if (installed.has_value() && advertisement.sequence <= *installed) {
        return DragonflyRouteUpdateDisposition::StaleOrDuplicate;
    }

    installed = advertisement.sequence;
    if (advertisement.kind == DragonflyRouteUpdateKind::DestinationAvailable) {
        _destination_available[advertisement.observer_router][advertisement.group] =
            advertisement.usable;
    } else {
        _safe_groups[advertisement.observer_router][advertisement.group] = advertisement.usable;
    }
    return DragonflyRouteUpdateDisposition::Accepted;
}

bool DragonflyProgressiveRouting::destinationAvailable(DragonflyRouterId observer_router,
                                                       DragonflyGroupId destination_group) const {
    (void)_topology.groupOf(observer_router);
    if (destination_group >= _topology.groupCount()) {
        throw std::out_of_range("Dragonfly destination-availability group is outside the topology");
    }
    return _destination_available[observer_router][destination_group];
}

bool DragonflyProgressiveRouting::groupSafe(DragonflyRouterId observer_router,
                                            DragonflyGroupId group) const {
    (void)_topology.groupOf(observer_router);
    if (group >= _topology.groupCount()) {
        throw std::out_of_range("Dragonfly safe-group ID is outside the topology");
    }
    return _safe_groups[observer_router][group];
}

std::vector<DragonflyTableEntry> DragonflyProgressiveRouting::sampleTwo(
    const std::vector<DragonflyTableEntry>& entries,
    std::uint64_t random_value) const {
    if (entries.empty()) {
        return {};
    }
    const std::size_t first = static_cast<std::size_t>(random_value % entries.size());
    std::vector<std::size_t> different_outputs;
    different_outputs.reserve(entries.size() - 1);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != first && entries[index].output_port != entries[first].output_port) {
            different_outputs.push_back(index);
        }
    }
    // Candidate slots represent physical outputs.  If this table has only
    // one legal output, duplicate the exact entry instead of presenting two
    // different intermediate groups that begin on the same wire.
    if (different_outputs.empty()) {
        return {entries[first], entries[first]};
    }
    const std::size_t second = different_outputs[static_cast<std::size_t>(
        splitmix64(random_value) % different_outputs.size())];
    return {entries[first], entries[second]};
}

DragonflyAdaptiveBias DragonflyProgressiveRouting::biasFor(DragonflyRoutingControl control) const {
    if (!isAdaptive(control)) {
        return {};
    }
    return _config.adaptive_biases[static_cast<std::size_t>(control)];
}

std::uint8_t DragonflyProgressiveRouting::adjustedScore(std::uint8_t congestion,
                                                        DragonflyClassBias bias) const {
    if (congestion > 15 || bias.shift > 2 || bias.additive > 63) {
        throw std::invalid_argument("Dragonfly adjusted score input is out of range");
    }
    const unsigned int shifted = static_cast<unsigned int>(congestion) << bias.shift;
    return static_cast<std::uint8_t>(
        std::min(63U, shifted + static_cast<unsigned int>(bias.additive)));
}

DragonflyCandidate DragonflyProgressiveRouting::scoreCandidate(
    DragonflyRouterId current,
    DragonflyRouteClass route_class,
    const DragonflyTableEntry& entry,
    const DragonflyRouteState& state) const {
    const std::uint8_t congestion =
        entry.output_port.has_value() ? portCongestion(current, *entry.output_port).compose() : 0;
    const DragonflyAdaptiveBias bias = biasFor(state.control);
    const DragonflyClassBias class_bias =
        route_class == DragonflyRouteClass::Minimal ? bias.minimal : bias.non_minimal;
    return {entry, route_class, congestion, adjustedScore(congestion, class_bias)};
}

DragonflyProgressiveRouting::SelectedEntry DragonflyProgressiveRouting::selectOneClass(
    DragonflyRouterId current,
    DragonflyRouteState& state,
    DragonflyRouteClass route_class,
    const std::vector<DragonflyTableEntry>& entries,
    bool adaptive_pair) const {
    if (entries.empty()) {
        throw std::logic_error("cannot select from an empty Dragonfly table");
    }
    const std::uint64_t random_value = splitmix64(
        state.route_hash ^ (static_cast<std::uint64_t>(current) << 32) ^ state.decision_count++);
    const std::vector<DragonflyTableEntry> considered =
        adaptive_pair ? sampleTwo(entries, random_value)
                      : std::vector<DragonflyTableEntry>{
                            entries[static_cast<std::size_t>(random_value % entries.size())]};
    SelectedEntry result;
    for (const DragonflyTableEntry& entry : considered) {
        result.candidates.push_back(scoreCandidate(current, route_class, entry, state));
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < result.candidates.size(); ++index) {
        if (result.candidates[index].adjusted_score < result.candidates[best].adjusted_score) {
            best = index;
        }
    }
    result.entry = result.candidates[best].entry;
    return result;
}

DragonflyProgressiveRouting::SelectedEntry DragonflyProgressiveRouting::selectInitial(
    DragonflyRouterId current,
    DragonflyRouteState& state,
    const std::vector<DragonflyTableEntry>& minimal,
    const std::vector<DragonflyTableEntry>& non_minimal) const {
    if (state.control == DragonflyRoutingControl::DeterministicMinimal) {
        return selectOneClass(current, state, DragonflyRouteClass::Minimal, minimal, false);
    }
    if (state.control == DragonflyRoutingControl::DeterministicNonMinimal) {
        return selectOneClass(current, state, DragonflyRouteClass::NonMinimal, non_minimal, false);
    }

    const std::uint64_t random_value = splitmix64(
        state.route_hash ^ (static_cast<std::uint64_t>(current) << 32) ^ state.decision_count++);
    SelectedEntry result;
    for (const DragonflyTableEntry& entry : sampleTwo(minimal, random_value)) {
        result.candidates.push_back(
            scoreCandidate(current, DragonflyRouteClass::Minimal, entry, state));
    }
    for (const DragonflyTableEntry& entry : sampleTwo(non_minimal, splitmix64(random_value))) {
        result.candidates.push_back(
            scoreCandidate(current, DragonflyRouteClass::NonMinimal, entry, state));
    }
    if (result.candidates.empty()) {
        throw std::logic_error("cannot select from empty Dragonfly candidate sets");
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < result.candidates.size(); ++index) {
        const DragonflyCandidate& candidate = result.candidates[index];
        const DragonflyCandidate& incumbent = result.candidates[best];
        if (candidate.adjusted_score < incumbent.adjusted_score ||
            (candidate.adjusted_score == incumbent.adjusted_score &&
             candidate.route_class == DragonflyRouteClass::Minimal &&
             incumbent.route_class == DragonflyRouteClass::NonMinimal)) {
            best = index;
        }
    }
    result.entry = result.candidates[best].entry;
    return result;
}

void DragonflyProgressiveRouting::validateState(DragonflyRouterId current,
                                                const DragonflyRouteState& state) const {
    const DragonflyGroupId current_group = _topology.groupOf(current);
    const DragonflyGroupId source_group = _topology.groupOf(state.source_router);
    const DragonflyGroupId target_group = _topology.groupOf(state.target_router);
    if (current != state.expected_current_router) {
        throw std::invalid_argument(
            "Dragonfly packet arrived at a router other than its committed next hop");
    }
    if (state.phase == DragonflyRoutePhase::Dropped) {
        return;
    }

    if (state.router_hops > _config.maximum_router_hops || state.global_hops > state.router_hops ||
        state.global_hops > 2 || state.local_hops_in_phase > state.router_hops) {
        throw std::invalid_argument("Dragonfly hop counters contradict route state");
    }
    if (state.router_hops == 0) {
        if (state.previous_router.has_value() || state.previous_output_port.has_value() ||
            state.last_virtual_channel.has_value()) {
            throw std::invalid_argument("zero-hop Dragonfly state carries hop metadata");
        }
    } else {
        if (!state.previous_router.has_value() || !state.previous_output_port.has_value() ||
            !state.last_virtual_channel.has_value() ||
            static_cast<std::uint8_t>(*state.last_virtual_channel) != state.router_hops - 1 ||
            _topology.port(*state.previous_router, *state.previous_output_port).next_router !=
                current) {
            throw std::invalid_argument(
                "Dragonfly previous-hop or dependency-phase metadata is inconsistent");
        }
    }
    if (state.intermediate_group.has_value() &&
        (*state.intermediate_group == source_group || *state.intermediate_group == target_group)) {
        throw std::invalid_argument("Dragonfly intermediate group is not a legal detour");
    }
    if (state.intermediate_root_router.has_value() &&
        (!state.intermediate_group.has_value() ||
         _topology.groupOf(*state.intermediate_root_router) != *state.intermediate_group)) {
        throw std::invalid_argument("Dragonfly intermediate root is outside its group");
    }
    if (state.target_root_router.has_value() &&
        _topology.groupOf(*state.target_root_router) != target_group) {
        throw std::invalid_argument("Dragonfly target root is outside the target group");
    }
    if (state.control == DragonflyRoutingControl::DeterministicMinimal &&
        state.route_class == DragonflyRouteClass::NonMinimal) {
        throw std::invalid_argument("deterministic-minimal state changed route class");
    }
    if (state.control == DragonflyRoutingControl::DeterministicNonMinimal &&
        state.route_class == DragonflyRouteClass::Minimal) {
        throw std::invalid_argument("deterministic-non-minimal state changed route class");
    }

    if (state.phase == DragonflyRoutePhase::Delivered) {
        if (current != state.target_router) {
            throw std::invalid_argument("delivered Dragonfly state is not at the target router");
        }
        return;
    }
    if (state.phase == DragonflyRoutePhase::Injection) {
        if (current != state.source_router || state.router_hops != 0 || state.global_hops != 0 ||
            state.local_hops_in_phase != 0 || state.route_class != DragonflyRouteClass::Undecided ||
            state.intermediate_group.has_value() || state.intermediate_root_router.has_value() ||
            state.target_root_router.has_value() || state.previous_output_port.has_value() ||
            state.last_virtual_channel.has_value() || state.diverged) {
            throw std::invalid_argument("Dragonfly injection state is inconsistent");
        }
        return;
    }

    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::invalid_argument(message);
        }
    };
    switch (state.phase) {
        case DragonflyRoutePhase::SourceAdaptiveUp:
            require(isAdaptive(state.control) &&
                        state.route_class == DragonflyRouteClass::Undecided &&
                        current_group == source_group && source_group != target_group &&
                        state.global_hops == 0 && state.local_hops_in_phase == 1 &&
                        !state.intermediate_group.has_value() &&
                        !state.intermediate_root_router.has_value() &&
                        !state.target_root_router.has_value(),
                    "source adaptive-UP state is inconsistent");
            break;
        case DragonflyRoutePhase::MinimalToTargetGroup:
            require(state.route_class == DragonflyRouteClass::Minimal &&
                        current_group == source_group && source_group != target_group &&
                        state.global_hops == 0 && state.local_hops_in_phase == 1 &&
                        !state.intermediate_group.has_value() &&
                        !state.intermediate_root_router.has_value() &&
                        !state.target_root_router.has_value(),
                    "source minimal phase is inconsistent");
            break;
        case DragonflyRoutePhase::NonMinimalToIntermediate:
            require(state.control == DragonflyRoutingControl::DeterministicNonMinimal &&
                        state.route_class == DragonflyRouteClass::NonMinimal &&
                        current_group == source_group && source_group != target_group &&
                        state.global_hops == 0 && state.local_hops_in_phase == 1 &&
                        !state.intermediate_group.has_value() &&
                        !state.intermediate_root_router.has_value() &&
                        !state.target_root_router.has_value(),
                    "source non-minimal phase is inconsistent");
            break;
        case DragonflyRoutePhase::IntermediateAdaptiveUp:
            require(state.route_class == DragonflyRouteClass::NonMinimal &&
                        state.intermediate_group.has_value() &&
                        current_group == *state.intermediate_group && state.global_hops == 1 &&
                        state.local_hops_in_phase <= 1 &&
                        !state.intermediate_root_router.has_value() &&
                        !state.target_root_router.has_value() &&
                        state.control != DragonflyRoutingControl::DeterministicMinimal,
                    "intermediate adaptive-UP state is inconsistent");
            break;
        case DragonflyRoutePhase::IntermediateRoot:
            require(state.route_class == DragonflyRouteClass::NonMinimal &&
                        state.intermediate_group.has_value() &&
                        current_group == *state.intermediate_group && state.global_hops == 1 &&
                        state.local_hops_in_phase >= 1 && state.local_hops_in_phase <= 2 &&
                        state.intermediate_root_router == current &&
                        !state.target_root_router.has_value(),
                    "intermediate LN-root state is inconsistent");
            break;
        case DragonflyRoutePhase::IntermediateToTargetGroup:
            require(state.route_class == DragonflyRouteClass::NonMinimal &&
                        state.intermediate_group.has_value() &&
                        current_group == *state.intermediate_group && state.global_hops == 1 &&
                        state.local_hops_in_phase == 1 &&
                        state.intermediate_root_router.has_value() &&
                        !state.target_root_router.has_value(),
                    "intermediate downrouting state is inconsistent");
            break;
        case DragonflyRoutePhase::TargetAdaptiveUp:
            require(current_group == target_group && state.local_hops_in_phase == 0 &&
                        !state.target_root_router.has_value() &&
                        ((state.route_class == DragonflyRouteClass::Minimal &&
                          state.global_hops == 1 && !state.intermediate_group.has_value() &&
                          !state.intermediate_root_router.has_value()) ||
                         (state.route_class == DragonflyRouteClass::NonMinimal &&
                          state.global_hops == 2 && state.intermediate_group.has_value())),
                    "target adaptive-UP state is inconsistent");
            break;
        case DragonflyRoutePhase::TargetRoot:
            require(state.route_class == DragonflyRouteClass::NonMinimal &&
                        current_group == target_group && state.local_hops_in_phase == 1 &&
                        state.target_root_router == current &&
                        ((state.global_hops < 2 && !state.intermediate_group.has_value()) ||
                         (state.global_hops == 2 && state.intermediate_group.has_value())),
                    "target LN-root state is inconsistent");
            break;
        case DragonflyRoutePhase::TargetLocal:
            require(state.route_class != DragonflyRouteClass::Undecided &&
                        current_group == target_group && state.local_hops_in_phase <= 1 &&
                        (current == state.target_router || state.local_hops_in_phase == 0) &&
                        ((state.global_hops < 2 && !state.intermediate_group.has_value()) ||
                         (state.global_hops == 2 && state.intermediate_group.has_value())),
                    "target downrouting state is inconsistent");
            break;
        case DragonflyRoutePhase::Injection:
        case DragonflyRoutePhase::Delivered:
        case DragonflyRoutePhase::Dropped:
            throw std::logic_error("Dragonfly terminal phase escaped early validation");
    }
}

DragonflyRoutingDecision DragonflyProgressiveRouting::drop(
    DragonflyRouteState& state,
    DragonflyDropReason reason,
    std::vector<DragonflyCandidate> candidates) const {
    state.phase = DragonflyRoutePhase::Dropped;
    return {DragonflyDecisionOutcome::Dropped,
            reason,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::move(candidates)};
}

DragonflyRoutingDecision DragonflyProgressiveRouting::delivered(DragonflyRouteState& state) const {
    state.phase = DragonflyRoutePhase::Delivered;
    return {DragonflyDecisionOutcome::Delivered,
            DragonflyDropReason::None,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {}};
}

DragonflyRoutingDecision DragonflyProgressiveRouting::forward(DragonflyRouterId current,
                                                              DragonflyRouteState& state,
                                                              SelectedEntry selected) const {
    if (!selected.entry.output_port.has_value()) {
        return drop(state, DragonflyDropReason::InvalidRouteState, std::move(selected.candidates));
    }
    const DragonflyPortId output_port = *selected.entry.output_port;
    if (!physicalLinkAlive(current, output_port)) {
        return drop(state, DragonflyDropReason::FailedCommittedLink,
                    std::move(selected.candidates));
    }
    if (state.router_hops >= _config.maximum_router_hops) {
        return drop(state, DragonflyDropReason::HopBudgetExceeded, std::move(selected.candidates));
    }

    const DragonflyVirtualChannel virtual_channel = dependencyPhaseForHop(state.router_hops);
    if (state.last_virtual_channel.has_value() &&
        static_cast<std::uint8_t>(virtual_channel) <=
            static_cast<std::uint8_t>(*state.last_virtual_channel)) {
        return drop(state, DragonflyDropReason::InvalidRouteState, std::move(selected.candidates));
    }
    const DragonflyPort& output = _topology.port(current, output_port);
    state.previous_router = current;
    state.previous_output_port = output_port;
    state.last_virtual_channel = virtual_channel;
    state.router_hops++;
    if (output.kind == DragonflyLinkKind::Local) {
        state.local_hops_in_phase++;
    } else {
        state.global_hops++;
        state.local_hops_in_phase = 0;
        const DragonflyGroupId next_group = _topology.groupOf(output.next_router);
        if (state.phase == DragonflyRoutePhase::MinimalToTargetGroup) {
            if (next_group != _topology.groupOf(state.target_router) || state.global_hops != 1) {
                return drop(state, DragonflyDropReason::InvalidRouteState,
                            std::move(selected.candidates));
            }
            state.phase = isAdaptive(state.control) ? DragonflyRoutePhase::TargetAdaptiveUp
                                                    : DragonflyRoutePhase::TargetLocal;
            state.diverged = false;
            state.target_root_router.reset();
        } else if (state.phase == DragonflyRoutePhase::NonMinimalToIntermediate) {
            if (next_group == _topology.groupOf(state.source_router) ||
                next_group == _topology.groupOf(state.target_router) || state.global_hops != 1) {
                return drop(state, DragonflyDropReason::InvalidRouteState,
                            std::move(selected.candidates));
            }
            // A local GN choice did not commit an intermediate.  The first
            // physical GN global crossing is the irreversible boundary.
            state.intermediate_group = next_group;
            state.intermediate_root_router.reset();
            state.phase = DragonflyRoutePhase::IntermediateAdaptiveUp;
            state.diverged = false;
        } else if (state.phase == DragonflyRoutePhase::IntermediateAdaptiveUp ||
                   state.phase == DragonflyRoutePhase::IntermediateToTargetGroup) {
            if (next_group != _topology.groupOf(state.target_router) || state.global_hops != 2) {
                return drop(state, DragonflyDropReason::InvalidRouteState,
                            std::move(selected.candidates));
            }
            state.phase = state.control == DragonflyRoutingControl::DeterministicMinimal
                              ? DragonflyRoutePhase::TargetLocal
                              : DragonflyRoutePhase::TargetAdaptiveUp;
            state.diverged = false;
            state.target_root_router.reset();
        } else {
            return drop(state, DragonflyDropReason::InvalidRouteState,
                        std::move(selected.candidates));
        }
    }

    state.expected_current_router = output.next_router;
    return {DragonflyDecisionOutcome::Forward,
            DragonflyDropReason::None,
            output.next_router,
            output_port,
            virtual_channel,
            std::move(selected.candidates)};
}

DragonflyRoutingDecision DragonflyProgressiveRouting::nextHop(DragonflyRouterId current,
                                                              DragonflyRouteState& state) const {
    try {
        validateState(current, state);
    } catch (const std::exception&) {
        return drop(state, DragonflyDropReason::InvalidRouteState);
    }
    if (state.phase == DragonflyRoutePhase::Dropped) {
        return drop(state, DragonflyDropReason::InvalidRouteState);
    }
    if (state.phase == DragonflyRoutePhase::Delivered) {
        return delivered(state);
    }
    if (current == state.target_router) {
        if (state.phase != DragonflyRoutePhase::Injection &&
            state.phase != DragonflyRoutePhase::TargetAdaptiveUp &&
            state.phase != DragonflyRoutePhase::TargetRoot &&
            state.phase != DragonflyRoutePhase::TargetLocal) {
            return drop(state, DragonflyDropReason::InvalidRouteState);
        }
        return delivered(state);
    }
    if (state.router_hops >= _config.maximum_router_hops) {
        return drop(state, DragonflyDropReason::HopBudgetExceeded);
    }

    while (true) {
        const DragonflyGroupId current_group = _topology.groupOf(current);
        const DragonflyGroupId target_group = _topology.groupOf(state.target_router);
        switch (state.phase) {
            case DragonflyRoutePhase::Injection: {
                const bool local = current_group == target_group;
                const std::vector<DragonflyTableEntry> minimal =
                    local ? (isAdaptive(state.control)
                                 ? adaptiveLocalMinimal(current, state.target_router)
                                 : localMinimal(current, state.target_router))
                          : globalMinimal(current, target_group, false);
                const std::vector<DragonflyTableEntry> non_minimal =
                    local ? localNonMinimal(current, state.target_router)
                          : globalNonMinimal(current, target_group);
                if ((state.control == DragonflyRoutingControl::DeterministicMinimal &&
                     minimal.empty()) ||
                    (state.control == DragonflyRoutingControl::DeterministicNonMinimal &&
                     non_minimal.empty()) ||
                    (isAdaptive(state.control) && minimal.empty() && non_minimal.empty())) {
                    return drop(state, DragonflyDropReason::NoLegalCandidate);
                }
                SelectedEntry selected = selectInitial(current, state, minimal, non_minimal);
                const bool chose_minimal =
                    selected.entry.family == DragonflyTableFamily::GlobalMinimal ||
                    selected.entry.family == DragonflyTableFamily::LocalMinimal;
                state.diverged = minimal.empty();
                if (local) {
                    if (chose_minimal) {
                        state.route_class = DragonflyRouteClass::Minimal;
                        state.phase = DragonflyRoutePhase::TargetLocal;
                        return forward(current, state, std::move(selected));
                    }
                    if (!selected.entry.root_router.has_value()) {
                        return drop(state, DragonflyDropReason::InvalidRouteState,
                                    std::move(selected.candidates));
                    }
                    state.route_class = DragonflyRouteClass::NonMinimal;
                    state.target_root_router = selected.entry.root_router;
                    if (selected.entry.root_detect) {
                        state.phase = DragonflyRoutePhase::TargetLocal;
                        state.local_hops_in_phase = 0;
                        state.diverged = false;
                        continue;
                    }
                    state.phase = DragonflyRoutePhase::TargetRoot;
                    return forward(current, state, std::move(selected));
                }

                if (!selected.entry.output_port.has_value()) {
                    return drop(state, DragonflyDropReason::InvalidRouteState,
                                std::move(selected.candidates));
                }
                const bool local_output =
                    _topology.port(current, *selected.entry.output_port).kind ==
                    DragonflyLinkKind::Local;
                // A local adaptive GN or GM output remains in the source UP
                // episode.  The next input queue re-arbitrates; neither route
                // class nor intermediate group has been committed yet.
                if (isAdaptive(state.control) && local_output) {
                    state.route_class = DragonflyRouteClass::Undecided;
                    state.phase = DragonflyRoutePhase::SourceAdaptiveUp;
                    return forward(current, state, std::move(selected));
                }
                state.route_class =
                    chose_minimal ? DragonflyRouteClass::Minimal : DragonflyRouteClass::NonMinimal;
                state.phase = chose_minimal ? DragonflyRoutePhase::MinimalToTargetGroup
                                            : DragonflyRoutePhase::NonMinimalToIntermediate;
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::SourceAdaptiveUp: {
                if (!isAdaptive(state.control) || state.global_hops != 0 ||
                    state.local_hops_in_phase == 0) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto minimal = globalMinimal(current, target_group, true);
                const auto non_minimal = globalNonMinimal(current, target_group, true);
                if (minimal.empty() && non_minimal.empty()) {
                    return drop(state, DragonflyDropReason::NoLegalCandidate);
                }
                state.diverged = minimal.empty();
                SelectedEntry selected = selectInitial(current, state, minimal, non_minimal);
                const bool chose_minimal =
                    selected.entry.family == DragonflyTableFamily::GlobalMinimal;
                state.route_class =
                    chose_minimal ? DragonflyRouteClass::Minimal : DragonflyRouteClass::NonMinimal;
                state.phase = chose_minimal ? DragonflyRoutePhase::MinimalToTargetGroup
                                            : DragonflyRoutePhase::NonMinimalToIntermediate;
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::MinimalToTargetGroup: {
                const auto entries =
                    globalMinimal(current, target_group, state.local_hops_in_phase != 0);
                if (entries.empty()) {
                    return drop(state, DragonflyDropReason::FailedCommittedLink);
                }
                SelectedEntry selected =
                    selectOneClass(current, state, DragonflyRouteClass::Minimal, entries,
                                   isAdaptive(state.control));
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::NonMinimalToIntermediate: {
                const auto entries =
                    globalNonMinimal(current, target_group, state.local_hops_in_phase != 0);
                if (entries.empty()) {
                    return drop(state, DragonflyDropReason::FailedCommittedLink);
                }
                SelectedEntry selected =
                    selectOneClass(current, state, DragonflyRouteClass::NonMinimal, entries,
                                   isAdaptive(state.control));
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::IntermediateAdaptiveUp: {
                if (!state.intermediate_group.has_value() ||
                    current_group != *state.intermediate_group || state.global_hops != 1) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto minimal =
                    globalMinimal(current, target_group, state.local_hops_in_phase != 0);
                const auto non_minimal = localNonMinimal(current, current);
                if ((state.control == DragonflyRoutingControl::DeterministicNonMinimal &&
                     non_minimal.empty()) ||
                    (isAdaptive(state.control) && minimal.empty() && non_minimal.empty())) {
                    return drop(state, DragonflyDropReason::NoLegalCandidate);
                }
                state.diverged = minimal.empty();
                SelectedEntry selected =
                    state.control == DragonflyRoutingControl::DeterministicNonMinimal
                        ? selectOneClass(current, state, DragonflyRouteClass::NonMinimal,
                                         non_minimal, false)
                        : selectInitial(current, state, minimal, non_minimal);
                if (selected.entry.family == DragonflyTableFamily::GlobalMinimal) {
                    // GM is still an UP choice here. A local GM output keeps
                    // the packet adaptive; a global output ends this episode.
                    return forward(current, state, std::move(selected));
                }
                if (!selected.entry.root_router.has_value()) {
                    return drop(state, DragonflyDropReason::InvalidRouteState,
                                std::move(selected.candidates));
                }
                state.route_class = DragonflyRouteClass::NonMinimal;
                state.intermediate_root_router = selected.entry.root_router;
                if (selected.entry.root_detect) {
                    state.phase = DragonflyRoutePhase::IntermediateToTargetGroup;
                    state.local_hops_in_phase = 0;
                    state.diverged = false;
                    continue;
                }
                state.phase = DragonflyRoutePhase::IntermediateRoot;
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::IntermediateRoot: {
                if (!state.intermediate_root_router.has_value() ||
                    current != *state.intermediate_root_router) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto root = localNonMinimal(current, current, state.intermediate_root_router);
                if (root.size() != 1 || !root.front().root_detect) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                state.phase = DragonflyRoutePhase::IntermediateToTargetGroup;
                state.local_hops_in_phase = 0;
                state.diverged = false;
                continue;
            }

            case DragonflyRoutePhase::IntermediateToTargetGroup: {
                const auto entries =
                    globalMinimal(current, target_group, state.local_hops_in_phase != 0);
                if (entries.empty()) {
                    return drop(state, DragonflyDropReason::FailedCommittedLink);
                }
                SelectedEntry selected =
                    selectOneClass(current, state, DragonflyRouteClass::NonMinimal, entries,
                                   isAdaptive(state.control));
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::TargetAdaptiveUp: {
                if (current_group != target_group) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto minimal = adaptiveLocalMinimal(current, state.target_router);
                const auto non_minimal = localNonMinimal(current, state.target_router);
                if ((state.control == DragonflyRoutingControl::DeterministicNonMinimal &&
                     non_minimal.empty()) ||
                    (isAdaptive(state.control) && minimal.empty() && non_minimal.empty())) {
                    return drop(state, DragonflyDropReason::NoLegalCandidate);
                }
                state.diverged = minimal.empty();
                SelectedEntry selected =
                    state.control == DragonflyRoutingControl::DeterministicNonMinimal
                        ? selectOneClass(current, state, DragonflyRouteClass::NonMinimal,
                                         non_minimal, false)
                        : selectInitial(current, state, minimal, non_minimal);
                if (selected.entry.family == DragonflyTableFamily::LocalMinimal) {
                    state.phase = DragonflyRoutePhase::TargetLocal;
                    return forward(current, state, std::move(selected));
                }
                if (!selected.entry.root_router.has_value()) {
                    return drop(state, DragonflyDropReason::InvalidRouteState,
                                std::move(selected.candidates));
                }
                state.route_class = DragonflyRouteClass::NonMinimal;
                state.target_root_router = selected.entry.root_router;
                if (selected.entry.root_detect) {
                    state.phase = DragonflyRoutePhase::TargetLocal;
                    state.local_hops_in_phase = 0;
                    state.diverged = false;
                    continue;
                }
                state.phase = DragonflyRoutePhase::TargetRoot;
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::TargetRoot: {
                if (!state.target_root_router.has_value() || current != *state.target_root_router) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto root = localNonMinimal(current, current, state.target_root_router);
                if (root.size() != 1 || !root.front().root_detect) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                state.phase = DragonflyRoutePhase::TargetLocal;
                state.local_hops_in_phase = 0;
                state.diverged = false;
                continue;
            }

            case DragonflyRoutePhase::TargetLocal: {
                if (current_group != target_group) {
                    return drop(state, DragonflyDropReason::InvalidRouteState);
                }
                const auto entries = localMinimal(current, state.target_router);
                if (entries.empty()) {
                    return drop(state, DragonflyDropReason::FailedCommittedLink);
                }
                SelectedEntry selected = selectOneClass(current, state, state.route_class, entries,
                                                        isAdaptive(state.control));
                return forward(current, state, std::move(selected));
            }

            case DragonflyRoutePhase::Delivered:
                return delivered(state);
            case DragonflyRoutePhase::Dropped:
                return drop(state, DragonflyDropReason::InvalidRouteState);
        }
    }
}

}  // namespace htsim
