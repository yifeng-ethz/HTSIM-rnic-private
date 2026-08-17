// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Sanity-study driver for the Slingshot-class dragonfly fabric.  Open-loop
// line-rate sources in the rnic-nn spirit (no congestion control) inject
// fixed wire packets over the ss-dragonfly fabric; the receiver bins
// delivered payload per interval per flow.  This binary produces sanity
// evidence for the fabric mechanisms, not calibration: the fabric is
// hosted with calibration pending against the Merlin captures.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eventlist.h"
#include "network.h"
#include "ss_dragonfly_fabric.h"

namespace {

struct SanityFlow {
    std::uint32_t flow_id{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    simtime_picosec start_ps{0};
};

struct SanityOptions {
    std::string topology_file;
    std::string pattern;  // incast or join
    std::uint32_t receiver{0};
    std::uint32_t degree{1};
    simtime_picosec join_interval_ps{0};
    simtime_picosec duration_ps{0};
    simtime_picosec drain_ps{50000000};
    std::uint64_t bin_ps{0};
    std::string out_csv;
    std::string routing{"adaptive"};
    std::optional<std::uint64_t> seed_override;
    std::uint64_t wire_bytes{4160};
    std::uint64_t header_bytes{64};
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
        << " -topo FILE -pattern incast|join -duration_ps PS -bin_ps PS -out FILE\n"
           "  [-receiver HOST] [-degree N] [-join_interval_ps PS]\n"
           "  [-routing adaptive|minimal|nonminimal] [-seed UINT64]\n"
           "  [-wire_bytes B] [-header_bytes B] [-drain_ps PS]\n";
    return out.str();
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
        } else {
            throw std::invalid_argument(option + ": unknown option");
        }
    }
    if (options.topology_file.empty() || options.pattern.empty() || options.out_csv.empty() ||
        options.duration_ps == 0 || options.bin_ps == 0) {
        throw std::invalid_argument("missing a required option");
    }
    if (options.pattern != "incast" && options.pattern != "join") {
        throw std::invalid_argument("-pattern expects incast or join");
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
        } else {
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
        }
        for (const SanityFlow& flow : flows) {
            if (flow.source == flow.destination) {
                throw std::invalid_argument("a sanity flow collapsed onto its receiver");
            }
        }

        const htsim::DragonflyRoutingControl control = routingControl(options.routing);
        const bool per_packet_hash = options.routing == "adaptive";

        // bins[bin][flow] = delivered payload bytes.
        std::map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> bins;
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

        std::vector<std::unique_ptr<OpenLoopSource>> sources;
        sources.reserve(flows.size());
        for (const SanityFlow& flow : flows) {
            sources.push_back(std::make_unique<OpenLoopSource>(
                event_list, fabric, flow, control, per_packet_hash, options.wire_bytes,
                options.header_bytes, options.duration_ps));
        }

        while (EventList::doNextEvent()) {
        }

        fabric.validateQuiescent();
        const SsDragonflyStatistics& statistics = fabric.statistics();

        std::ofstream out(options.out_csv, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("cannot open output CSV '" + options.out_csv + "'");
        }
        out << "bin_start_ps,bin_end_ps,flow_id,source,destination,"
               "delivered_payload_bytes,goodput_bps\n";
        for (const auto& [bin, per_flow] : bins) {
            for (const auto& [flow_id, bytes] : per_flow) {
                const SanityFlow& flow = flows[flow_id];
                out << bin * options.bin_ps << ',' << (bin + 1) * options.bin_ps << ','
                    << flow_id << ',' << flow.source << ',' << flow.destination << ','
                    << bytes << ','
                    << bytes * 8000000ULL * 1000000ULL / options.bin_ps << '\n';
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n' << usage(program);
        return 2;
    }
}
