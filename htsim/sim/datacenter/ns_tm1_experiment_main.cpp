// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm1_experiment.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        NsTm1ExperimentConfig config;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Opt-in, uncalibrated store-forward TM1 UDP component study.\n"
                    "--policy quota_dd|quota_prbs|prbs|unpaced --source 4x25|1x100 --receiver 4x25|1x100\n"
                    "--routing balanced|collision|hash --mtu N --window-us N --tick-ns N --fraction 0.95\n"
                    "--seed N --payload-bytes N --rate-scale 1|0.5 --internal-overhead N --xpe-mask 1..15\n"
                    "--unavailable-cells N --admission static|dynamic --source-queue-bytes N\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[i];
            if (option == "--policy") config.policy = value;
            else if (option == "--source") config.source = value;
            else if (option == "--receiver") config.receiver = value;
            else if (option == "--routing") config.routing = value;
            else if (option == "--mtu") config.mtu = std::stoul(value);
            else if (option == "--window-us") config.window_ps = std::stoull(value) * 1000000;
            else if (option == "--tick-ns") config.switch_config.tick_ps = std::stoull(value) * 1000;
            else if (option == "--fraction") config.fraction_ppm = std::llround(std::stod(value) * 1000000);
            else if (option == "--rate-scale") config.rate_scale_ppm = std::llround(std::stod(value) * 1000000);
            else if (option == "--seed") config.seed = std::stoull(value);
            else if (option == "--payload-bytes") config.payload_bytes = std::stoull(value);
            else if (option == "--source-queue-bytes") config.source_queue_bytes = std::stoull(value);
            else if (option == "--internal-overhead") config.switch_config.internal_overhead_bytes = std::stoull(value);
            else if (option == "--xpe-mask") {
                const auto mask = std::stoul(value);
                if (!mask || mask > 15) throw std::invalid_argument("XPE mask must be in [1,15]");
                config.xpe_mask = mask;
            } else if (option == "--unavailable-cells") config.switch_config.unavailable_cells.fill(std::stoull(value));
            else if (option == "--admission") {
                if (value != "static" && value != "dynamic") throw std::invalid_argument("admission must be static or dynamic");
                config.switch_config.admission = value == "static" ? NsTm1Admission::Static : NsTm1Admission::Dynamic;
            } else throw std::invalid_argument("unknown option " + option);
        }
        const auto result = runNsTm1Experiment(config);
        std::cout << nsTm1CsvHeader() << '\n' << nsTm1CsvRow(config, result) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "htsim_ns_tm1: " << error.what() << '\n';
        return 1;
    }
}
