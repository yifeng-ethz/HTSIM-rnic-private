// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
#include "config.h"
#include "network.h"
#include "queue_lossless_input.h"
#include "randomqueue.h"
#include <iostream>
#include <math.h>

#include <sstream>
#include <string.h>
// #include "subflow_control.h"
#include "clock.h"
#include "compositequeue.h"
#include "connection_matrix.h"
#include "eventlist.h"
#include "firstfit.h"
#include "logfile.h"
#include "loggers.h"
#include "logsim-interface.h"
#include "pipe.h"
#include "topology.h"
#include "fat_tree_topology.h"
#include "fat_tree_switch.h"
#include "paper_uec.h"
#include <filesystem>
#include <memory>
#include <optional>
// #include "vl2_topology.h"

// Fat Tree topology was modified to work with this script, others won't work
// correctly
// #include "oversubscribed_fat_tree_topology.h"
// #include "multihomed_fat_tree_topology.h"
// #include "star_topology.h"
// #include "bcube_topology.h"
#include <list>

// Simulation params

#define PRINT_PATHS 0

#define PERIODIC 0
#include "main.h"

// int RTT = 10; // this is per link delay; identical RTT microseconds = 0.02 ms
uint32_t RTT = 400; // this is per link delay in ns; identical RTT microseconds
                    // = 0.02 ms
int DEFAULT_NODES = 128;
#define DEFAULT_QUEUE_SIZE 100000000 // ~100MB, just a large value so we can ignore queues
// int N=128;

FirstFit *ff = NULL;
unsigned int subflow_count = 1;

string ntoa_uec(double n);
string itoa_uec(uint64_t n);

// #define SWITCH_BUFFER (SERVICE * RTT / 1000)
#define USE_FIRST_FIT 0
#define FIRST_FIT_INTERVAL 100

EventList eventlist;

Logfile *lg;

void exit_error(char *progr) {
    cout << "Usage " << progr
         << " [UNCOUPLED(DEFAULT)|COUPLED_INC|FULLY_COUPLED|COUPLED_EPSILON] "
            "[epsilon][COUPLED_SCALABLE_TCP"
         << endl;
    exit(1);
}

int main(int argc, char **argv) {
    Packet::set_packet_size(4096);
    // eventlist.setEndtime(timeFromSec(1));
    Clock c(timeFromSec(5 / 100.), eventlist);
    mem_b queuesize = 100;
    int no_of_conns = 0, cwnd = 10, no_of_nodes = DEFAULT_NODES;
    stringstream filename(ios_base::out);
    RouteStrategy route_strategy = NOT_SET;
    std::string goal_filename;
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    simtime_picosec hop_latency = timeFromNs((uint32_t)RTT);
    simtime_picosec switch_latency = timeFromNs((uint32_t)0);
    simtime_picosec pacing_delay = 1000;
    int packet_size = 2048;
    int kmin = -1;
    int kmax = -1;
    int bts_threshold = -1;
    int seed = -1;
    bool reuse_entropy = false;
    int number_entropies = 256;
    queue_type queue_choice = COMPOSITE;
    bool ignore_ecn_data = true;
    bool ignore_ecn_ack = true;
    PaperUecSrc::set_fast_drop(false);
    bool do_jitter = false;
    bool do_exponential_gain = false;
    bool use_fast_increase = false;
    double gain_value_med_inc = 1;
    double jitter_value_med_inc = 1;
    double delay_gain_value_med_inc = 5;
    int target_rtt_percentage_over_base = 50;
    bool collect_data = false;
    int fat_tree_k = 1; // 1:1 default
    bool use_super_fast_increase = false;
    double y_gain = 1;
    double x_gain = 0.15;
    double z_gain = 1;
    double w_gain = 1;
    bool collect_flow_info = false;
    double bonus_drop = 1;
    double drop_value_buffer = 1;
    double starting_cwnd_ratio = 0;
    uint64_t explicit_starting_cwnd = 0;
    uint64_t explicit_starting_buffer = 0;
    uint64_t explicit_base_rtt = 0;
    uint64_t explicit_target_rtt = 0;
    uint64_t explicit_bdp = 0;
    double queue_size_ratio = 0;
    bool disable_case_3 = false;
    bool disable_case_4 = false;
    int ratio_os_stage_1 = 1;
    int pfc_low = 0;
    int pfc_high = 0;
    int pfc_marking = 0;
    double quickadapt_lossless_rtt = 2.0;
    int reaction_delay = 1;
    bool stop_after_quick = false;
    char *tm_file = NULL;
    char* topo_file = NULL;
    bool use_pacing = false;
    int precision_ts = 1;
    int once_per_rtt = 0;
    bool use_mixed = false;
    int phantom_size;
    int phantom_slowdown = 10;
    bool use_phantom = false;
    double exp_avg_ecn_value = .3;
    double exp_avg_rtt_value = .3;
    double exp_avg_alpha = 0.125;
    bool use_exp_avg_ecn = false;
    bool use_exp_avg_rtt = false;
    int percentage_lgs = 0;
    int jump_to = 0;
    int stop_pacing_after_rtt = 0;
    int num_failed_links = 0;
    bool topology_normal = true;
    uint64_t interdc_delay = 0;
    uint64_t max_queue_size = 0;
    int linkspeed_gbps = 0;
    int lgs_o = 0;
    int lgs_O = 0;
    int lgs_g = 0;
    std::optional<AtlahsHtsimApi::GoalRankMapping> goal_rank_mapping_override;

    int i = 1;
    filename << "logout.dat";

    while (i < argc) {
        if (!strcmp(argv[i], "-o")) {
            filename.str(std::string());
            filename << argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-sub")) {
            subflow_count = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-conns")) {
            no_of_conns = atoi(argv[i + 1]);
            cout << "no_of_conns " << no_of_conns << endl;
            cout << "!!currently hardcoded to 8, value will be ignored!!" << endl;
            i++;
        } else if (!strcmp(argv[i], "-nodes")) {
            no_of_nodes = atoi(argv[i + 1]);
            cout << "no_of_nodes " << no_of_nodes << endl;
            i++;
        } else if (!strcmp(argv[i], "-cwnd")) {
            cwnd = atoi(argv[i + 1]);
            cout << "cwnd " << cwnd << endl;
            i++;
        } else if (!strcmp(argv[i], "-q")) {
            queuesize = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-once_per_rtt")) {
            once_per_rtt = atoi(argv[i + 1]);
            PaperUecSrc::set_once_per_rtt(once_per_rtt);
            printf("OnceRTTDecrease: %d\n", once_per_rtt);
            i++;
        } else if (!strcmp(argv[i],"-topo")){
            topo_file = argv[i+1];
            cout << "FatTree topology input file: "<< topo_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-stop_pacing_after_rtt")) {
            stop_pacing_after_rtt = atoi(argv[i + 1]);
            PaperUecSrc::set_stop_pacing(stop_pacing_after_rtt);
            i++;
        } else if (!strcmp(argv[i], "-linkspeed")) {
            // linkspeed specified is in Mbps
            linkspeed = speedFromMbps(atof(argv[i + 1]));
            linkspeed_gbps = atof(argv[i + 1]) / 1000;
            // Saving this for UEC reference, Gbps
            i++;
        } else if (!strcmp(argv[i], "-kmin")) {
            // kmin as percentage of queue size (0..100)
            kmin = atoi(argv[i + 1]);
            printf("KMin: %d\n", atoi(argv[i + 1]));
            // /CompositeQueue::set_kMin(kmin);
            PaperUecSrc::set_kmin(kmin / 100.0);
            i++;
        } else if (!strcmp(argv[i], "-k")) {
            fat_tree_k = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-ratio_os_stage_1")) {
            ratio_os_stage_1 = atoi(argv[i + 1]);
            PaperUecSrc::set_os_ratio_stage_1(ratio_os_stage_1);
            i++;
        } else if (!strcmp(argv[i], "-kmax")) {
            // kmin as percentage of queue size (0..100)
            kmax = atoi(argv[i + 1]);
            printf("KMax: %d\n", atoi(argv[i + 1]));
            //CompositeQueue::set_kMax(kmax);
            PaperUecSrc::set_kmax(kmax / 100.0);
            i++;
        } else if (!strcmp(argv[i], "-pfc_marking")) {
            pfc_marking = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-quickadapt_lossless_rtt")) {
            quickadapt_lossless_rtt = std::stod(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-bts_trigger")) {
            bts_threshold = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-mtu")) {
            packet_size = atoi(argv[i + 1]);
            //PKT_SIZE_MODERN = packet_size; // Saving this for UEC reference, Bytes
            i++;
        } else if (!strcmp(argv[i], "-reuse_entropy")) {
            reuse_entropy = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-disable_case_3")) {
            disable_case_3 = atoi(argv[i + 1]);
            PaperUecSrc::set_disable_case_3(disable_case_3);
            printf("DisableCase3: %d\n", disable_case_3);
            i++;
        } else if (!strcmp(argv[i], "-jump_to")) {
            PaperUecSrc::jump_to = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-reaction_delay")) {
            reaction_delay = atoi(argv[i + 1]);
            PaperUecSrc::set_reaction_delay(reaction_delay);
            printf("ReactionDelay: %d\n", reaction_delay);
            i++;
        } else if (!strcmp(argv[i], "-precision_ts")) {
            precision_ts = atoi(argv[i + 1]);
            //FatTreeSwitch::set_precision_ts(precision_ts * 1000);
            PaperUecSrc::set_precision_ts(precision_ts * 1000);
            printf("Precision: %d\n", precision_ts * 1000);
            i++;
        } else if (!strcmp(argv[i], "-disable_case_4")) {
            disable_case_4 = atoi(argv[i + 1]);
            PaperUecSrc::set_disable_case_4(disable_case_4);
            printf("DisableCase4: %d\n", disable_case_4);
            i++;
        } else if (!strcmp(argv[i], "-stop_after_quick")) {
            PaperUecSrc::set_stop_after_quick(true);
            printf("StopAfterQuick: %d\n", true);
        } else if (!strcmp(argv[i], "-lgs_flow_stats")) {
            collect_flow_info = true;
            printf("Flow collection: %d\n", true);
        } else if (!strcmp(argv[i], "-paths")) {
            number_entropies = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-switch_latency")) {
            switch_latency = timeFromNs(atof(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-hop_latency")) {
            hop_latency = timeFromNs(atof(argv[i + 1]));
            //LINK_DELAY_MODERN = hop_latency / 1000; // Saving this for UEC reference, ps to ns
            i++;
        } else if (!strcmp(argv[i], "-ignore_ecn_ack")) {
            ignore_ecn_ack = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lgs_o")) {
            lgs_o = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lgs_g")) {
            lgs_g = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lgs_O")) {
            lgs_O = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-ignore_ecn_data")) {
            ignore_ecn_data = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-pacing_delay")) {
            pacing_delay = atoi(argv[i + 1]);
            PaperUecSrc::set_pacing_delay(pacing_delay);
            i++;
        } else if (!strcmp(argv[i], "-use_pacing")) {
            use_pacing = atoi(argv[i + 1]);
            PaperUecSrc::set_use_pacing(use_pacing);
            i++;
        } else if (!strcmp(argv[i], "-fast_drop")) {
            PaperUecSrc::set_fast_drop(atoi(argv[i + 1]));
            printf("FastDrop: %d\n", atoi(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-seed")) {
            seed = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-interdc_delay")) {
            interdc_delay = atoi(argv[i + 1]);
            interdc_delay *= 1000;
            i++;
        } else if (!strcmp(argv[i], "-max_queue_size")) {
            max_queue_size = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-percentage_lgs")) {
            percentage_lgs = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-pfc_low")) {
            pfc_low = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-pfc_high")) {
            pfc_high = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-collect_data")) {
            collect_data = atoi(argv[i + 1]);
            // /COLLECT_DATA = collect_data;
            i++;
        } else if (!strcmp(argv[i], "-do_jitter")) {
            do_jitter = atoi(argv[i + 1]);
            PaperUecSrc::set_do_jitter(do_jitter);
            printf("DoJitter: %d\n", do_jitter);
            i++;
        } else if (!strcmp(argv[i], "-do_exponential_gain")) {
            do_exponential_gain = atoi(argv[i + 1]);
            PaperUecSrc::set_do_exponential_gain(do_exponential_gain);
            printf("DoExpGain: %d\n", do_exponential_gain);
            i++;
        } else if (!strcmp(argv[i], "-use_fast_increase")) {
            use_fast_increase = atoi(argv[i + 1]);
            PaperUecSrc::set_use_fast_increase(use_fast_increase);
            printf("FastIncrease: %d\n", use_fast_increase);
            i++;
        } else if (!strcmp(argv[i], "-use_super_fast_increase")) {
            use_super_fast_increase = atoi(argv[i + 1]);
            PaperUecSrc::set_use_super_fast_increase(use_super_fast_increase);
            printf("FastIncreaseSuper: %d\n", use_super_fast_increase);
            i++;
        } else if (!strcmp(argv[i], "-gain_value_med_inc")) {
            gain_value_med_inc = std::stod(argv[i + 1]);
            // PaperUecSrc::set_gain_value_med_inc(gain_value_med_inc);
            printf("GainValueMedIncrease: %f\n", gain_value_med_inc);
            i++;
        } else if (!strcmp(argv[i], "-jitter_value_med_inc")) {
            jitter_value_med_inc = std::stod(argv[i + 1]);
            // PaperUecSrc::set_jitter_value_med_inc(jitter_value_med_inc);
            printf("JitterValue: %f\n", jitter_value_med_inc);
            i++;
        } else if (!strcmp(argv[i], "-decrease_on_nack")) {
            double decrease_on_nack = std::stod(argv[i + 1]);
            PaperUecSrc::set_decrease_on_nack(decrease_on_nack);
            i++;
        } else if (!strcmp(argv[i], "-phantom_in_series")) {
            //CompositeQueue::set_use_phantom_in_series();
            printf("PhantomQueueInSeries: %d\n", 1);
            // i++;
        } else if (!strcmp(argv[i], "-phantom_both_queues")) {
            //CompositeQueue::set_use_both_queues();
            printf("PhantomUseBothForECNMarking: %d\n", 1);
        } else if (!strcmp(argv[i], "-delay_gain_value_med_inc")) {
            delay_gain_value_med_inc = std::stod(argv[i + 1]);
            // PaperUecSrc::set_delay_gain_value_med_inc(delay_gain_value_med_inc);
            printf("DelayGainValue: %f\n", delay_gain_value_med_inc);
            i++;
        } else if (!strcmp(argv[i], "-tm")) {
            tm_file = argv[i + 1];
            cout << "traffic matrix input file: " << tm_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-target_rtt_percentage_over_base")) {
            target_rtt_percentage_over_base = atoi(argv[i + 1]);
            PaperUecSrc::set_target_rtt_percentage_over_base(target_rtt_percentage_over_base);
            printf("TargetRTT: %d\n", target_rtt_percentage_over_base);
            i++;
        } else if (!strcmp(argv[i], "-num_failed_links")) {
            num_failed_links = atoi(argv[i + 1]);
            //FatTreeTopology::set_failed_links(num_failed_links);
            i++;
        } else if (!strcmp(argv[i], "-fast_drop_rtt")) {
            PaperUecSrc::set_fast_drop_rtt(atoi(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-y_gain")) {
            y_gain = std::stod(argv[i + 1]);
            PaperUecSrc::set_y_gain(y_gain);
            printf("YGain: %f\n", y_gain);
            i++;
        } else if (!strcmp(argv[i], "-x_gain")) {
            x_gain = std::stod(argv[i + 1]);
            PaperUecSrc::set_x_gain(x_gain);
            printf("XGain: %f\n", x_gain);
            i++;
        } else if (!strcmp(argv[i], "-z_gain")) {
            z_gain = std::stod(argv[i + 1]);
            PaperUecSrc::set_z_gain(z_gain);
            printf("ZGain: %f\n", z_gain);
            i++;
        } else if (!strcmp(argv[i], "-w_gain")) {
            w_gain = std::stod(argv[i + 1]);
            PaperUecSrc::set_w_gain(w_gain);
            printf("WGain: %f\n", w_gain);
            i++;
        } else if (!strcmp(argv[i], "-starting_cwnd_ratio")) {
            starting_cwnd_ratio = std::stod(argv[i + 1]);
            printf("StartingWindowRatio: %f\n", starting_cwnd_ratio);
            i++;
        } else if (!strcmp(argv[i], "-explicit_starting_cwnd")) {
            explicit_starting_cwnd = atoi(argv[i + 1]);
            printf("StartingWindowForced: %d\n", explicit_starting_cwnd);
            i++;
        } else if (!strcmp(argv[i], "-explicit_starting_buffer")) {
            explicit_starting_buffer = atoi(argv[i + 1]);
            printf("StartingBufferForced: %d\n", explicit_starting_buffer);
            explicit_bdp = explicit_starting_buffer;
            i++;
        } else if (!strcmp(argv[i], "-explicit_base_rtt")) {
            explicit_base_rtt = ((uint64_t)atoi(argv[i + 1])) * 1000;
            printf("BaseRTTForced: %d\n", explicit_base_rtt);
            PaperUecSrc::set_explicit_rtt(explicit_base_rtt);
            i++;
        } else if (!strcmp(argv[i], "-explicit_target_rtt")) {
            explicit_target_rtt = ((uint64_t)atoi(argv[i + 1])) * 1000;
            printf("TargetRTTForced: %lu\n", explicit_target_rtt);
            PaperUecSrc::set_explicit_target_rtt(explicit_target_rtt);
            i++;
        } else if (!strcmp(argv[i], "-queue_size_ratio")) {
            queue_size_ratio = std::stod(argv[i + 1]);
            printf("QueueSizeRatio: %f\n", queue_size_ratio);
            i++;
        } else if (!strcmp(argv[i], "-bonus_drop")) {
            bonus_drop = std::stod(argv[i + 1]);
            PaperUecSrc::set_bonus_drop(bonus_drop);
            printf("BonusDrop: %f\n", bonus_drop);
            i++;
        } else if (!strcmp(argv[i], "-drop_value_buffer")) {
            drop_value_buffer = std::stod(argv[i + 1]);
            PaperUecSrc::set_buffer_drop(drop_value_buffer);
            printf("BufferDrop: %f\n", drop_value_buffer);
            i++;
        } else if (!strcmp(argv[i], "-goal")) {
            goal_filename = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-goal_rank_mapping")) {
            // Not in the artifact main (it always auto-infers); accepted here
            // so Step-3 comparison runs can pin the mapping on both sides.
            if (!strcmp(argv[i + 1], "auto")) {
                goal_rank_mapping_override.reset();
            } else if (!strcmp(argv[i + 1], "gpu-rank")) {
                goal_rank_mapping_override = AtlahsHtsimApi::GoalRankMapping::GpuRank;
            } else if (!strcmp(argv[i + 1], "unique-nic")) {
                goal_rank_mapping_override = AtlahsHtsimApi::GoalRankMapping::UniqueNic;
            } else {
                printf("-goal_rank_mapping: expected auto, gpu-rank, or unique-nic\n");
                exit(0);
            }
            i++;
        } else if (!strcmp(argv[i], "-use_phantom")) {
            use_phantom = atoi(argv[i + 1]);
            printf("UsePhantomQueue: %d\n", use_phantom);
            //CompositeQueue::set_use_phantom_queue(use_phantom);
            i++;
        } else if (!strcmp(argv[i], "-use_exp_avg_ecn")) {
            use_exp_avg_ecn = atoi(argv[i + 1]);
            printf("UseExpAvgEcn: %d\n", use_exp_avg_ecn);
            PaperUecSrc::set_exp_avg_ecn(use_exp_avg_ecn);
            i++;
        } else if (!strcmp(argv[i], "-use_exp_avg_rtt")) {
            use_exp_avg_rtt = atoi(argv[i + 1]);
            printf("UseExpAvgRtt: %d\n", use_exp_avg_rtt);
            PaperUecSrc::set_exp_avg_rtt(use_exp_avg_rtt);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_rtt_value")) {
            exp_avg_rtt_value = std::stod(argv[i + 1]);
            printf("UseExpAvgRttValue: %d\n", exp_avg_rtt_value);
            PaperUecSrc::set_exp_avg_rtt_value(exp_avg_rtt_value);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_ecn_value")) {
            exp_avg_ecn_value = std::stod(argv[i + 1]);
            printf("UseExpAvgecn_value: %d\n", exp_avg_ecn_value);
            PaperUecSrc::set_exp_avg_ecn_value(exp_avg_ecn_value);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_alpha")) {
            exp_avg_alpha = std::stod(argv[i + 1]);
            printf("UseExpAvgalpha: %d\n", exp_avg_alpha);
            PaperUecSrc::set_exp_avg_alpha(exp_avg_alpha);
            i++;
        } else if (!strcmp(argv[i], "-phantom_size")) {
            phantom_size = atoi(argv[i + 1]);
            printf("PhantomQueueSize: %d\n", phantom_size);
            //CompositeQueue::set_phantom_queue_size(phantom_size);
            i++;
        } else if (!strcmp(argv[i], "-os_border")) {
            int os_b = atoi(argv[i + 1]);
            //FatTreeInterDCTopology::set_os_ratio_border(os_b);
            i++;
        } else if (!strcmp(argv[i], "-phantom_slowdown")) {
            phantom_slowdown = atoi(argv[i + 1]);
            printf("PhantomQueueSize: %d\n", phantom_slowdown);
            //CompositeQueue::set_phantom_queue_slowdown(phantom_slowdown);
            i++;
        } else if (!strcmp(argv[i], "-strat")) {
            if (!strcmp(argv[i + 1], "perm")) {
                route_strategy = SCATTER_PERMUTE;
            } else if (!strcmp(argv[i + 1], "rand")) {
                route_strategy = SCATTER_RANDOM;
            } else if (!strcmp(argv[i + 1], "pull")) {
                route_strategy = PULL_BASED;
            } else if (!strcmp(argv[i + 1], "single")) {
                route_strategy = SINGLE_PATH;
            } else if (!strcmp(argv[i + 1], "ecmp_host")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                //FatTreeInterDCSwitch::set_strategy(FatTreeInterDCSwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "ecmp_host_random_ecn")) {
                //route_strategy = ECMP_RANDOM_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                //FatTreeInterDCSwitch::set_strategy(FatTreeInterDCSwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "ecmp_host_random2_ecn")) {
                //route_strategy = ECMP_RANDOM2_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                //FatTreeInterDCSwitch::set_strategy(FatTreeInterDCSwitch::ECMP);
            }
            i++;
        } else if (!strcmp(argv[i], "-topology")) {
            if (!strcmp(argv[i + 1], "normal")) {
                topology_normal = true;
            } else if (!strcmp(argv[i + 1], "interdc")) {
                topology_normal = false;
            }
            i++;
        } else if (!strcmp(argv[i], "-queue_type")) {
            if (!strcmp(argv[i + 1], "composite")) {
                queue_choice = COMPOSITE;
                PaperUecSrc::set_queue_type("composite");
            } else if (!strcmp(argv[i + 1], "composite_bts")) {
                //queue_choice = COMPOSITE_BTS;
                PaperUecSrc::set_queue_type("composite_bts");
                printf("Name Running: UEC BTS\n");
            } else if (!strcmp(argv[i + 1], "lossless_input")) {
                queue_choice = LOSSLESS_INPUT;
                PaperUecSrc::set_queue_type("lossless_input");
                printf("Name Running: UEC Queueless\n");
            }
            i++;
        } else if (!strcmp(argv[i], "-algorithm")) {
            if (!strcmp(argv[i + 1], "delayA")) {
                PaperUecSrc::set_alogirthm("delayA");
                printf("Name Running: UEC Version A\n");
            } else if (!strcmp(argv[i + 1], "smartt")) {
                PaperUecSrc::set_alogirthm("smartt");
                printf("Name Running: SMaRTT\n");
            } else if (!strcmp(argv[i + 1], "mprdma")) {
                PaperUecSrc::set_alogirthm("mprdma");
                printf("Name Running: SMaRTT Per RTT\n");
            } else if (!strcmp(argv[i + 1], "min_cc")) {
                PaperUecSrc::set_alogirthm("min_cc");
            } else if (!strcmp(argv[i + 1], "no_cc")) {
                PaperUecSrc::set_alogirthm("no_cc");
                printf("Name Running: STrack\n");
            } else if (!strcmp(argv[i + 1], "swift_like")) {
                PaperUecSrc::set_alogirthm("swift_like");
                printf("Name Running: swift_like\n");
            } else if (!strcmp(argv[i + 1], "rtt")) {
                PaperUecSrc::set_alogirthm("rtt");
                printf("Name Running: SMaRTT RTT Only\n");
            } else if (!strcmp(argv[i + 1], "ecn")) {
                PaperUecSrc::set_alogirthm("ecn");
                printf("Name Running: SMaRTT ECN Only Constant\n");
            } else if (!strcmp(argv[i + 1], "custom")) {
                PaperUecSrc::set_alogirthm("custom");
                printf("Name Running: SMaRTT ECN Only Variable\n");
            } else if (!strcmp(argv[i + 1], "intersmartt")) {
                PaperUecSrc::set_alogirthm("intersmartt");
                printf("Name Running: SMaRTT InterDataCenter\n");
            } else if (!strcmp(argv[i + 1], "intersmartt_new")) {
                PaperUecSrc::set_alogirthm("intersmartt_new");
                printf("Name Running: SMaRTT InterDataCenter\n");
            } else if (!strcmp(argv[i + 1], "intersmartt_simple")) {
                PaperUecSrc::set_alogirthm("intersmartt_simple");
                printf("Name Running: SMaRTT InterDataCenter\n");
            } else if (!strcmp(argv[i + 1], "intersmartt")) {
                PaperUecSrc::set_alogirthm("intersmartt");
                printf("Name Running: SMaRTT InterDataCenter\n");
            } else if (!strcmp(argv[i + 1], "intersmartt_composed")) {
                PaperUecSrc::set_alogirthm("intersmartt_composed");
                printf("Name Running: SMaRTT InterDataCenter\n");
            } else if (!strcmp(argv[i + 1], "smartt_2")) {
                PaperUecSrc::set_alogirthm("smartt_2");
                printf("Name Running: SMaRTT smartt_2\n");
            } else {
                printf("Wrong Algorithm Name\n");
                exit(0);
            }
            i++;
        } else
            exit_error(argv[0]);

        i++;
    }

    //SINGLE_PKT_TRASMISSION_TIME_MODERN = packet_size * 8 / (LINK_SPEED_MODERN);

    // Initialize Seed, Logging and Other variables
    if (seed != -1) {
        srand(seed);
        srandom(seed);
    } else {
        srand(time(NULL));
        srandom(time(NULL));
    }
    Packet::set_packet_size(packet_size);
    if (route_strategy == NOT_SET) {
        fprintf(stderr, "Route Strategy not set.  Use the -strat param.  "
                        "\nValid values are perm, rand, pull, rg and single\n");
        exit(1);
    }

    eventlist.setEndtime(timeFromSec((uint32_t)60));

    // Restore the ATLAHS artifact's hardcoded CompositeQueue constants (the
    // fork made them stricter/different for the new designs):
    //   header-queue overflow bounds 20000x/200000x, ECN marking on header
    //   dequeue, and a 10000:1 high/low service ratio.
    CompositeQueue::set_header_bound_factors(20000, 200000);
    CompositeQueue::set_ecn_on_deque_headers(true);
    CompositeQueue::set_high_prio_ratio(10000);
    // The artifact main never printed per-event LGS lines; the Llama trace
    // would otherwise print millions of them.
    LogSimInterface::lgs_print_event_stats = false;

    // Calculate Network Info
    int hops = 4; // hardcoded for now
    uint64_t actual_starting_cwnd = 0;
    uint64_t base_rtt_max_hops = (hops * 1000) + (4096 * 8 / (linkspeed_gbps) * hops) +
                                 (hops * 1000) + (64 * 8 / (linkspeed_gbps) * hops);
    uint64_t bdp_local = base_rtt_max_hops * (linkspeed_gbps) / 8;

    if (starting_cwnd_ratio == 0) {
        actual_starting_cwnd = bdp_local; // Equal to BDP if not other info
    } else {
        actual_starting_cwnd = bdp_local * starting_cwnd_ratio;
    }
    if (queue_size_ratio == 0) {
        queuesize = bdp_local; // Equal to BDP if not other info
    } else {
        queuesize = bdp_local * queue_size_ratio;
    }

    // Artifact's llama_rand hack skipped: it only triggers on the filename
    // "llama_random.bin", which none of the in-scope workloads use, and the
    // fork's AtlahsHtsimApi has no such switch.

    if (explicit_starting_buffer != 0) {
        queuesize = explicit_starting_buffer;
    }
    if (explicit_starting_cwnd != 0) {
        actual_starting_cwnd = explicit_starting_cwnd;
        PaperUecSrc::set_explicit_bdp(explicit_bdp);
    }

    PaperUecSrc::set_starting_cwnd(actual_starting_cwnd);
    if (max_queue_size != 0) {
        queuesize = max_queue_size;
        PaperUecSrc::set_switch_queue_size(max_queue_size);
    }

    printf("Using BDP of %lu - Queue is %lld - Starting Window is %lu - RTT "
           "%lu - Bandwidth %lu\n",
           bdp_local, queuesize, actual_starting_cwnd, base_rtt_max_hops, linkspeed_gbps);

    cout << "Using subflow count " << subflow_count << endl;

    // prepare the loggers

    cout << "Logging to " << filename.str() << endl;
    // Logfile
    Logfile logfile(filename.str(), eventlist);

#if PRINT_PATHS
    filename << ".paths";
    cout << "Logging path choices to " << filename.str() << endl;
    std::ofstream paths(filename.str().c_str());
    if (!paths) {
        cout << "Can't open for writing paths file!" << endl;
        exit(1);
    }
#endif

    lg = &logfile;

    logfile.setStartTime(timeFromSec(0));

    // UecLoggerSimple uecLogger;
    // logfile.addLogger(uecLogger);
    TrafficLoggerSimple traffic_logger = TrafficLoggerSimple();
    logfile.addLogger(traffic_logger);

    // PaperUecSrc *uecSrc;
    // PaperUecSink *uecSink;

    PaperUecSrc::setRouteStrategy(route_strategy);
    PaperUecSink::setRouteStrategy(route_strategy);

    // Route *routeout, *routein;
    // double extrastarttime;

    int dest;

    if (topology_normal) {

    } else {
    }

#if USE_FIRST_FIT
    if (subflow_count == 1) {
        ff = new FirstFit(timeFromMs(FIRST_FIT_INTERVAL), eventlist);
    }
#endif

#ifdef FAT_TREE
#endif

#ifdef FAT_TREE_INTERDC_TOPOLOGY_H

#endif

#ifdef OV_FAT_TREE
    OversubscribedFatTreeTopology *top = new OversubscribedFatTreeTopology(&logfile, &eventlist, ff);
#endif

#ifdef MH_FAT_TREE
    MultihomedFatTreeTopology *top = new MultihomedFatTreeTopology(&logfile, &eventlist, ff);
#endif

#ifdef STAR
    StarTopology *top = new StarTopology(&logfile, &eventlist, ff);
#endif

#ifdef BCUBE
    BCubeTopology *top = new BCubeTopology(&logfile, &eventlist, ff);
    cout << "BCUBE " << K << endl;
#endif

#ifdef VL2
    VL2Topology *top = new VL2Topology(&logfile, &eventlist, ff);
#endif

#if USE_FIRST_FIT
    if (ff)
        ff->net_paths = net_paths;
#endif

    map<int, vector<int> *>::iterator it;

    // used just to print out stats data at the end
    list<const Route *> routes;

    int connID = 0;
    dest = 1;
    // int receiving_node = 127;
    vector<int> subflows_chosen;

    ConnectionMatrix *conns = NULL;
    LogSimInterface *lgs = NULL;

    queue_type snd_type = FAIR_PRIO;
    queue_type qt = COMPOSITE;
    FatTreeTopology *top = NULL;

    unique_ptr<FatTreeTopologyCfg> topo_cfg;

    if (tm_file != NULL) {
        // The artifact's connection-matrix mode is not ported: every in-scope
        // workload replays a GOAL binary via -goal.
        fprintf(stderr, "htsim_uec_paper: -tm mode is not ported; use -goal\n");
        exit(1);
    } else if (goal_filename.size() > 0) {
        printf("Starting LGS Interface");

        if (topo_file) {
            topo_cfg = FatTreeTopologyCfg::load(topo_file, queuesize, qt, snd_type);
        } else {
            topo_cfg = make_unique<FatTreeTopologyCfg>(3u, (uint32_t)no_of_nodes, linkspeed,
                                                       queuesize, hop_latency, switch_latency,
                                                       qt, snd_type);
        }
        // The artifact's CompositeQueue::decide_ECN() hardcodes marking
        // thresholds at 20%/80% of the (uniform) queue size on every queue;
        // the fork moved thresholds into the topology config, so restore them
        // here (including on ToR downlinks).
        topo_cfg->set_ecn_parameters(true, true, queuesize * 0.2, queuesize * 0.8);
        topo_cfg->check_consistency();
        top = new FatTreeTopology(topo_cfg.get(), NULL, &eventlist, nullptr);

        AtlahsHtsimApi *api = new AtlahsHtsimApi();
        api->setTopology(top);
        api->setTopologyCfg(topo_cfg.get());
        api->setGoalRankMappingOverride(goal_rank_mapping_override);
        api->setEventList(&eventlist);
        api->setComputeEvent(new ComputeEvent(eventlist));
        api->setNullEvent(new NullEvent(eventlist));
        lgs = new LogSimInterface(NULL, NULL, eventlist, top, nullptr);
        api->setLogSimInterface(lgs);
        lgs->htsim_api = api;
        lgs->set_protocol(SENDER_PROTOCOL);
        lgs->htsim_api->linkspeed = linkspeed;
        lgs->percentage_lgs = percentage_lgs;
        lgs->print_stats_flows = collect_flow_info;
        lgs->htsim_api->print_stats_flows = collect_flow_info;
        

        double linkSpeedBytesPerSec = (linkspeed/1000000000 * 1e9) / 8.0;

        // Calculate LGS parameters
        lgs->htsim_api->htsim_G  = 1e9 / linkSpeedBytesPerSec;
        lgs->htsim_api->linkspeed_gbps = linkspeed_gbps;
        lgs->lgs_O = lgs_O;
        lgs->lgs_o = lgs_o;
        lgs->lgs_g = lgs_g;

        lgs->htsim_api->total_nodes = no_of_nodes;
        lgs->htsim_api->setSenderCwnd(bdp_local);
        lgs->htsim_api->setSenderBdp(bdp_local);
        lgs->htsim_api->setSenderRtt(base_rtt_max_hops);
        lgs->htsim_api->Setup();
        printf("Started LGS\n");
        
        
        start_lgs(goal_filename, *lgs);
        printf("Finished all\n");
        fflush(stdout);
        return 0;
    }
}

string ntoa_uec(double n) {
    stringstream s;
    s << n;
    return s.str();
}

string itoa_uec(uint64_t n) {
    stringstream s;
    s << n;
    return s.str();
}
