// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Sanity-study driver for the Slingshot-class dragonfly fabric.  Open-loop
// line-rate sources in the rnic-nn spirit (no congestion control) inject
// fixed wire packets over the ss-dragonfly fabric; the receiver bins
// delivered payload per interval per flow.  This binary produces sanity
// evidence for the fabric mechanisms, not calibration: the fabric is
// hosted with calibration pending against the Merlin captures.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../rnic_wide_integer.h"
#include "eventlist.h"
#include "network.h"
#include "ss_dragonfly_fabric.h"
#include "ss_dragonfly_load.h"

namespace {

struct SanityFlow {
    std::uint32_t flow_id{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    simtime_picosec start_ps{0};
};

struct SanityOptions {
    std::string topology_file;
    std::string pattern;  // incast, join, or explicit
    std::uint32_t receiver{0};
    std::uint32_t degree{1};
    simtime_picosec join_interval_ps{0};
    simtime_picosec duration_ps{0};
    // The drain window must exceed the worst-case receiver-leaf buffer
    // drain (shared buffer over the host link rate); 50 ms dwarfs it.
    simtime_picosec drain_ps{50000000000};
    std::uint64_t bin_ps{0};
    std::string out_csv;
    std::string routing{"adaptive"};
    std::optional<std::uint64_t> seed_override;
    std::uint64_t wire_bytes{4160};
    std::uint64_t header_bytes{64};
    // Load-harness options (HTSIM-29, HTSIM-30); every default keeps the
    // legacy open-loop line-rate behavior byte-identical.
    SsDragonflySourceMode source_mode{SsDragonflySourceMode::OpenLoop};
    std::vector<SsDragonflyLoadFlowSpec> flow_specs;
    std::optional<std::uint64_t> offered_bps;
    std::optional<std::uint64_t> think_ps;
    std::uint64_t chunk_payload_bytes{0};
    std::string chunk_out_csv;
};

std::uint64_t parseUnsigned(const std::string& option, const std::string& text) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(option + ": requires an unsigned value");
    }
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
        throw std::invalid_argument(option + ": malformed value '" + text + "'");
    }
    return static_cast<std::uint64_t>(value);
}

std::string usage(const std::string& program) {
    std::ostringstream out;
    out << "Usage: " << program
        << " -topo FILE -pattern incast|join|explicit -duration_ps PS -bin_ps PS"
           " -out FILE\n"
           "  [-receiver HOST] [-degree N] [-join_interval_ps PS]\n"
           "  [-routing adaptive|minimal|nonminimal] [-seed UINT64]\n"
           "  [-wire_bytes B] [-header_bytes B] [-drain_ps PS]\n"
           "  [-source open|paced|closed] [-offered_bps BPS] [-think_ps PS]\n"
           "  [-chunk_payload_bytes B] [-chunk_out FILE]\n"
           "  [-flow src=H,dst=H[,start_ps=PS][,offered_bps=BPS][,think_ps=PS]]...\n";
    return out.str();
}

// Mode and option consistency for the load harness.  The rules keep the
// capability surface explicit: nothing about a legacy open-loop
// incast/join invocation reaches this beyond the no-op checks.
void validateLoadOptions(const SanityOptions& options) {
    const bool explicit_pattern = options.pattern == "explicit";
    if (explicit_pattern && options.flow_specs.empty()) {
        throw std::invalid_argument("-pattern explicit requires at least one -flow");
    }
    if (!explicit_pattern && !options.flow_specs.empty()) {
        throw std::invalid_argument("-flow requires -pattern explicit");
    }
    bool any_flow_offered = false;
    bool any_flow_think = false;
    for (const SsDragonflyLoadFlowSpec& spec : options.flow_specs) {
        any_flow_offered = any_flow_offered || spec.offered_bps.has_value();
        any_flow_think = any_flow_think || spec.think_ps.has_value();
    }
    switch (options.source_mode) {
        case SsDragonflySourceMode::OpenLoop:
            if (options.offered_bps.has_value() || any_flow_offered) {
                throw std::invalid_argument("offered_bps requires -source paced");
            }
            if (options.think_ps.has_value() || any_flow_think) {
                throw std::invalid_argument("think_ps requires -source closed");
            }
            if (options.chunk_payload_bytes != 0 || !options.chunk_out_csv.empty()) {
                throw std::invalid_argument(
                    "chunk accounting requires -source paced or closed");
            }
            break;
        case SsDragonflySourceMode::Paced: {
            if (options.think_ps.has_value() || any_flow_think) {
                throw std::invalid_argument("think_ps requires -source closed");
            }
            // Without the global default, every declared flow must carry
            // its own rate (and the incast/join patterns declare none).
            if (!options.offered_bps.has_value()) {
                bool every_flow_covered = !options.flow_specs.empty();
                for (const SsDragonflyLoadFlowSpec& spec : options.flow_specs) {
                    every_flow_covered =
                        every_flow_covered && spec.offered_bps.has_value();
                }
                if (!every_flow_covered) {
                    throw std::invalid_argument(
                        "-source paced requires -offered_bps or a per-flow "
                        "offered_bps on every flow");
                }
            }
            break;
        }
        case SsDragonflySourceMode::ClosedLoop:
            if (options.offered_bps.has_value() || any_flow_offered) {
                throw std::invalid_argument("offered_bps requires -source paced");
            }
            if (options.chunk_payload_bytes == 0) {
                throw std::invalid_argument(
                    "-source closed requires -chunk_payload_bytes");
            }
            break;
    }
    if (!options.chunk_out_csv.empty() && options.chunk_payload_bytes == 0) {
        throw std::invalid_argument("-chunk_out requires -chunk_payload_bytes");
    }
}

SanityOptions parseOptions(int argc, char* argv[]) {
    SanityOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (index + 1 >= argc) {
            throw std::invalid_argument(option + ": missing value");
        }
        const std::string value = argv[++index];
        if (option == "-topo") {
            options.topology_file = value;
        } else if (option == "-pattern") {
            options.pattern = value;
        } else if (option == "-receiver") {
            options.receiver = static_cast<std::uint32_t>(parseUnsigned(option, value));
        } else if (option == "-degree") {
            options.degree = static_cast<std::uint32_t>(parseUnsigned(option, value));
        } else if (option == "-join_interval_ps") {
            options.join_interval_ps = parseUnsigned(option, value);
        } else if (option == "-duration_ps") {
            options.duration_ps = parseUnsigned(option, value);
        } else if (option == "-drain_ps") {
            options.drain_ps = parseUnsigned(option, value);
        } else if (option == "-bin_ps") {
            options.bin_ps = parseUnsigned(option, value);
        } else if (option == "-out") {
            options.out_csv = value;
        } else if (option == "-routing") {
            options.routing = value;
        } else if (option == "-seed") {
            options.seed_override = parseUnsigned(option, value);
        } else if (option == "-wire_bytes") {
            options.wire_bytes = parseUnsigned(option, value);
        } else if (option == "-header_bytes") {
            options.header_bytes = parseUnsigned(option, value);
        } else if (option == "-source") {
            options.source_mode = parseSsDragonflySourceMode(value);
        } else if (option == "-flow") {
            options.flow_specs.push_back(parseSsDragonflyFlowSpec(value));
        } else if (option == "-offered_bps") {
            options.offered_bps = parseUnsigned(option, value);
        } else if (option == "-think_ps") {
            options.think_ps = parseUnsigned(option, value);
        } else if (option == "-chunk_payload_bytes") {
            options.chunk_payload_bytes = parseUnsigned(option, value);
        } else if (option == "-chunk_out") {
            options.chunk_out_csv = value;
        } else {
            throw std::invalid_argument(option + ": unknown option");
        }
    }
    if (options.topology_file.empty() || options.pattern.empty() || options.out_csv.empty() ||
        options.duration_ps == 0 || options.bin_ps == 0) {
        throw std::invalid_argument("missing a required option");
    }
    if (options.pattern != "incast" && options.pattern != "join" &&
        options.pattern != "explicit") {
        throw std::invalid_argument("-pattern expects incast, join, or explicit");
    }
    if (options.pattern == "join" && options.join_interval_ps == 0) {
        throw std::invalid_argument("-pattern join requires -join_interval_ps");
    }
    if (options.routing != "adaptive" && options.routing != "minimal" &&
        options.routing != "nonminimal") {
        throw std::invalid_argument("-routing expects adaptive, minimal, or nonminimal");
    }
    if (options.header_bytes >= options.wire_bytes) {
        throw std::invalid_argument("-header_bytes must leave payload room");
    }
    validateLoadOptions(options);
    return options;
}

htsim::DragonflyRoutingControl routingControl(const std::string& name) {
    if (name == "adaptive") {
        return htsim::DragonflyRoutingControl::Adaptive0;
    }
    if (name == "minimal") {
        return htsim::DragonflyRoutingControl::DeterministicMinimal;
    }
    return htsim::DragonflyRoutingControl::DeterministicNonMinimal;
}

// One open-loop source: injects whole wire packets at exactly the host
// link rate from its start time until the study stop time.
class OpenLoopSource final : public EventSource {
public:
    OpenLoopSource(EventList& eventlist,
                   SsDragonflyTopology& fabric,
                   const SanityFlow& flow,
                   htsim::DragonflyRoutingControl control,
                   bool per_packet_hash,
                   std::uint64_t wire_bytes,
                   std::uint64_t header_bytes,
                   simtime_picosec stop_ps)
        : EventSource(eventlist, "ss-dragonfly-source-" + std::to_string(flow.flow_id)),
          _fabric(fabric),
          _flow(flow),
          _control(control),
          _per_packet_hash(per_packet_hash),
          _wire_bytes(wire_bytes),
          _header_bytes(header_bytes),
          _stop_ps(stop_ps),
          _packet_flow(nullptr) {
        _packet_flow.set_flowid(flow.flow_id + 1);
        const linkspeed_bps rate = fabric.config().host_link.rate_bps;
        _serialization_ps = (_wire_bytes * 8000000ULL * 1000000ULL + rate - 1) / rate;
        eventlist.sourceIsPending(*this, flow.start_ps);
    }

    void doNextEvent() override {
        const simtime_picosec now = EventList::now();
        if (now >= _stop_ps) {
            return;
        }
        const std::uint64_t hash =
            _fabric.packetRouteHash(_flow.flow_id, _per_packet_hash ? _sequence : 0);
        SsDragonflyPacket* packet = SsDragonflyPacket::newPacket(
            _packet_flow, _fabric.injectionRoute(_flow.source),
            static_cast<packetid_t>(_sequence), _flow.source, _flow.destination,
            static_cast<mem_b>(_wire_bytes),
            static_cast<mem_b>(_wire_bytes - _header_bytes));
        _fabric.registerPacket(*packet, _control, hash);
        _sequence++;
        packet->sendOn();
        eventlist().sourceIsPendingRel(*this, _serialization_ps);
    }

private:
    SsDragonflyTopology& _fabric;
    SanityFlow _flow;
    htsim::DragonflyRoutingControl _control;
    bool _per_packet_hash;
    std::uint64_t _wire_bytes;
    std::uint64_t _header_bytes;
    simtime_picosec _stop_ps;
    PacketFlow _packet_flow;
    simtime_picosec _serialization_ps{0};
    std::uint64_t _sequence{0};
};

}  // namespace

int main(int argc, char* argv[]) {
    const std::string program = argc > 0 && argv[0] != nullptr ? argv[0] : "htsim_ss_dragonfly";
    try {
        const SanityOptions options = parseOptions(argc, argv);
        SsDragonflyConfig config = loadSsDragonflyConfigFile(options.topology_file);
        if (options.seed_override.has_value()) {
            config.routing_seed = *options.seed_override;
        }
        config.maximum_wire_packet_bytes = options.wire_bytes;

        EventList event_list;
        EventList::setEndtime(options.duration_ps + options.drain_ps);
        SsDragonflyTopology fabric(event_list, config);

        if (options.receiver >= fabric.hostCount()) {
            throw std::invalid_argument("receiver host is outside the fabric");
        }
        std::vector<SanityFlow> flows;
        const std::uint32_t hosts = fabric.hostCount();
        if (options.pattern == "incast") {
            if (options.degree == 0 || options.degree >= hosts) {
                throw std::invalid_argument("incast degree must fit the remaining hosts");
            }
            for (std::uint32_t k = 0; k < options.degree; ++k) {
                flows.push_back(SanityFlow{
                    k, (options.receiver + 1 + k) % hosts, options.receiver, 0});
            }
        } else if (options.pattern == "join") {
            const std::uint32_t p = config.hosts_per_router;
            if (options.degree == 0 || options.degree >= fabric.routerCount()) {
                throw std::invalid_argument("join degree must fit the remaining routers");
            }
            for (std::uint32_t k = 0; k < options.degree; ++k) {
                // One sender per router walking away from the receiver's
                // router, so successive joiners contend from distinct
                // routers and groups.
                const std::uint32_t source = (options.receiver + (k + 1) * p) % hosts;
                flows.push_back(SanityFlow{k, source, options.receiver,
                                           static_cast<simtime_picosec>(k) *
                                               options.join_interval_ps});
            }
        } else {
            // Explicit multi-receiver cell: the declared flow list, in
            // order, with distinct destination ports allowed (HTSIM-30).
            for (std::uint32_t k = 0; k < options.flow_specs.size(); ++k) {
                const SsDragonflyLoadFlowSpec& spec = options.flow_specs[k];
                if (spec.source >= hosts || spec.destination >= hosts) {
                    throw std::invalid_argument("-flow endpoints are outside the fabric");
                }
                flows.push_back(
                    SanityFlow{k, spec.source, spec.destination, spec.start_ps});
            }
        }
        for (const SanityFlow& flow : flows) {
            if (flow.source == flow.destination) {
                throw std::invalid_argument("a sanity flow collapsed onto its receiver");
            }
        }
        for (std::size_t a = 0; a < flows.size(); ++a) {
            for (std::size_t b = a + 1; b < flows.size(); ++b) {
                if (flows[a].source == flows[b].source &&
                    flows[a].destination == flows[b].destination) {
                    throw std::invalid_argument(
                        "flows must have pairwise distinct (source, destination) pairs");
                }
            }
        }

        const htsim::DragonflyRoutingControl control = routingControl(options.routing);
        const bool per_packet_hash = options.routing == "adaptive";

        // bins[bin][flow] = delivered payload bytes.
        std::map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> bins;
        const bool load_mode = options.source_mode != SsDragonflySourceMode::OpenLoop;
        SsDragonflyLoadDispatch dispatch;
        if (!load_mode) {
            fabric.setDeliveryObserver([&](const SsDragonflyPacket& packet,
                                           const htsim::DragonflyRouteState&,
                                           simtime_picosec at_ps) {
                std::uint32_t flow_id = UINT32_MAX;
                for (const SanityFlow& flow : flows) {
                    if (flow.source == packet.src() && flow.destination == packet.dst()) {
                        flow_id = flow.flow_id;
                        break;
                    }
                }
                if (flow_id == UINT32_MAX) {
                    throw std::logic_error("delivered packet matches no sanity flow");
                }
                bins[at_ps / options.bin_ps][flow_id] +=
                    static_cast<std::uint64_t>(packet.payloadBytes());
            });
        } else {
            // Same binning, plus the chunk and drop bookkeeping the load
            // sources need.
            fabric.setDeliveryObserver([&](const SsDragonflyPacket& packet,
                                           const htsim::DragonflyRouteState&,
                                           simtime_picosec at_ps) {
                std::uint32_t flow_id = UINT32_MAX;
                for (const SanityFlow& flow : flows) {
                    if (flow.source == packet.src() && flow.destination == packet.dst()) {
                        flow_id = flow.flow_id;
                        break;
                    }
                }
                if (flow_id == UINT32_MAX) {
                    throw std::logic_error("delivered packet matches no sanity flow");
                }
                bins[at_ps / options.bin_ps][flow_id] +=
                    static_cast<std::uint64_t>(packet.payloadBytes());
                dispatch.noteDelivery(packet, at_ps);
            });
            fabric.setDropObserver(
                [&](const SsDragonflyPacket&, simtime_picosec at_ps) {
                    dispatch.noteDrop(at_ps);
                });
        }

        const std::uint64_t chunk_packets =
            options.chunk_payload_bytes == 0
                ? 0
                : ssDragonflyChunkPackets(options.chunk_payload_bytes,
                                          options.wire_bytes - options.header_bytes);
        std::vector<std::unique_ptr<OpenLoopSource>> sources;
        std::vector<std::unique_ptr<SsDragonflyLoadSource>> load_sources;
        if (!load_mode) {
            sources.reserve(flows.size());
            for (const SanityFlow& flow : flows) {
                sources.push_back(std::make_unique<OpenLoopSource>(
                    event_list, fabric, flow, control, per_packet_hash, options.wire_bytes,
                    options.header_bytes, options.duration_ps));
            }
        } else {
            load_sources.reserve(flows.size());
            for (const SanityFlow& flow : flows) {
                // Per-flow declarations override the invocation globals;
                // both are configuration, never fitted constants.
                const SsDragonflyLoadFlowSpec* spec =
                    options.pattern == "explicit" ? &options.flow_specs[flow.flow_id]
                                                  : nullptr;
                if (options.source_mode == SsDragonflySourceMode::Paced) {
                    const std::uint64_t offered =
                        spec != nullptr && spec->offered_bps.has_value()
                            ? *spec->offered_bps
                            : options.offered_bps.value();
                    load_sources.push_back(std::make_unique<SsDragonflyPacedSource>(
                        event_list, fabric, flow.flow_id, flow.source, flow.destination,
                        flow.start_ps, options.duration_ps, control, per_packet_hash,
                        options.wire_bytes, options.header_bytes, chunk_packets,
                        offered));
                } else {
                    const std::uint64_t think =
                        spec != nullptr && spec->think_ps.has_value()
                            ? *spec->think_ps
                            : options.think_ps.value_or(0);
                    load_sources.push_back(std::make_unique<SsDragonflyClosedLoopSource>(
                        event_list, fabric, flow.flow_id, flow.source, flow.destination,
                        flow.start_ps, options.duration_ps, control, per_packet_hash,
                        options.wire_bytes, options.header_bytes, chunk_packets, think));
                }
                dispatch.addSource(*load_sources.back());
            }
        }

        while (EventList::doNextEvent()) {
        }

        if (options.source_mode == SsDragonflySourceMode::ClosedLoop &&
            fabric.statistics().dropped_packets > 0) {
            throw std::runtime_error(
                "closed-loop cell dropped packets; no retransmission is modeled, so "
                "the loop would stall silently");
        }

        const SsDragonflyStatistics& statistics = fabric.statistics();
        std::cout << "[ss-dragonfly drain]";
        for (std::uint32_t router = 0; router < fabric.routerCount(); ++router) {
            std::cout << " r" << router << ".occ="
                      << fabric.router(router).shared_buffer_occupancy();
        }
        std::cout << " live="
                  << statistics.injected_packets - statistics.delivered_packets -
                         statistics.dropped_packets
                  << '\n';
        fabric.validateQuiescent();

        std::ofstream out(options.out_csv, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("cannot open output CSV '" + options.out_csv + "'");
        }
        out << "bin_start_ps,bin_end_ps,flow_id,source,destination,"
               "delivered_payload_bytes,goodput_bps\n";
        for (const auto& [bin, per_flow] : bins) {
            for (const auto& [flow_id, bytes] : per_flow) {
                const SanityFlow& flow = flows[flow_id];
                const RnicWideInteger goodput_numerator =
                    RnicWideInteger(bytes) * UINT64_C(8000000000000);
                out << bin * options.bin_ps << ',' << (bin + 1) * options.bin_ps << ','
                    << flow_id << ',' << flow.source << ',' << flow.destination << ','
                    << bytes << ','
                    << static_cast<std::uint64_t>(goodput_numerator / options.bin_ps)
                    << '\n';
            }
        }
        out.flush();
        if (!out) {
            throw std::runtime_error("failed while writing '" + options.out_csv + "'");
        }

        std::cout << "[ss-dragonfly manifest] evidence=sanity-not-calibration"
                  << " calibration_status=hosted-pending-merlin-calibration"
                  << " pattern=" << options.pattern << " routing=" << options.routing
                  << " receiver=" << options.receiver << " degree=" << options.degree
                  << " join_interval_ps=" << options.join_interval_ps
                  << " duration_ps=" << options.duration_ps << " bin_ps=" << options.bin_ps
                  << " wire_bytes=" << options.wire_bytes
                  << " header_bytes=" << options.header_bytes
                  << " routing_seed=" << fabric.config().routing_seed
                  << " topology=" << options.topology_file << '\n';
        std::cout << "[ss-dragonfly manifest] p=" << fabric.config().hosts_per_router
                  << " a=" << fabric.config().routers_per_group
                  << " h=" << fabric.config().global_links_per_router
                  << " g=" << fabric.config().group_count
                  << " routers=" << fabric.routerCount() << " hosts=" << fabric.hostCount()
                  << '\n';
        std::cout << "[ss-dragonfly manifest] injected=" << statistics.injected_packets
                  << " delivered=" << statistics.delivered_packets
                  << " delivered_payload_bytes=" << statistics.delivered_payload_bytes
                  << " dropped=" << statistics.dropped_packets
                  << " minimal=" << statistics.minimal_deliveries
                  << " non_minimal=" << statistics.non_minimal_deliveries
                  << " undecided=" << statistics.undecided_deliveries
                  << " advertisements_consumed=" << statistics.advertisements_consumed
                  << " max_router_hops="
                  << static_cast<unsigned>(statistics.maximum_router_hops_observed) << '\n';

        // Load-harness reporting; printed only when the load capabilities
        // are active, so every legacy invocation's stdout stays
        // byte-identical.
        if (load_mode) {
            std::cout << "[ss-dragonfly load] source="
                      << (options.source_mode == SsDragonflySourceMode::Paced ? "paced"
                                                                              : "closed")
                      << " flows=" << load_sources.size()
                      << " chunk_payload_bytes=" << options.chunk_payload_bytes
                      << " chunk_packets=" << chunk_packets << " first_drop_ps=";
            if (dispatch.firstDropPs().has_value()) {
                std::cout << *dispatch.firstDropPs();
            } else {
                std::cout << "none";
            }
            std::cout << '\n';
            for (const std::unique_ptr<SsDragonflyLoadSource>& source : load_sources) {
                std::cout << "[ss-dragonfly flow] flow=" << source->flowId()
                          << " source=" << source->source()
                          << " destination=" << source->destination()
                          << " start_ps=" << source->startPs() << " offered_bps=";
                if (const auto* paced =
                        dynamic_cast<const SsDragonflyPacedSource*>(source.get())) {
                    std::cout << paced->offeredBps();
                } else {
                    std::cout << "line";
                }
                std::cout << " think_ps=";
                if (const auto* closed =
                        dynamic_cast<const SsDragonflyClosedLoopSource*>(source.get())) {
                    std::cout << closed->thinkPs();
                } else {
                    std::cout << "na";
                }
                std::cout << " injected_packets=" << source->injectedPackets()
                          << " chunks_completed=" << source->completedChunks().size()
                          << '\n';
            }
            if (!options.chunk_out_csv.empty()) {
                std::ofstream chunk_out(options.chunk_out_csv,
                                        std::ios::out | std::ios::trunc);
                if (!chunk_out.is_open()) {
                    throw std::runtime_error("cannot open chunk CSV '" +
                                             options.chunk_out_csv + "'");
                }
                chunk_out << "flow_id,chunk_index,first_injection_ps,completion_ps\n";
                for (const std::unique_ptr<SsDragonflyLoadSource>& source : load_sources) {
                    std::vector<SsDragonflyChunkCompletion> completions =
                        source->completedChunks();
                    std::sort(completions.begin(), completions.end(),
                              [](const SsDragonflyChunkCompletion& lhs,
                                 const SsDragonflyChunkCompletion& rhs) {
                                  return lhs.chunk_index < rhs.chunk_index;
                              });
                    for (const SsDragonflyChunkCompletion& completion : completions) {
                        chunk_out << source->flowId() << ',' << completion.chunk_index
                                  << ',' << completion.first_injection_ps << ','
                                  << completion.completion_ps << '\n';
                    }
                }
                chunk_out.flush();
                if (!chunk_out) {
                    throw std::runtime_error("failed while writing '" +
                                             options.chunk_out_csv + "'");
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n' << usage(program);
        return 2;
    }
}
