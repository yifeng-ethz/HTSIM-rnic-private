// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "rnic_flow_session.h"
#include "simllm_atlahs_flow_runtime.h"
#include "simllm_htsim_network_port.h"
#include "simllm/rnic/rnic_device.h"
#include "simllm/rnic/session_record.h"

namespace {

using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimNetworkPortConfig;
using htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig;

std::string frame(const std::string& body) {
    const std::uint32_t size = static_cast<std::uint32_t>(body.size());
    std::string result;
    result.push_back(static_cast<char>((size >> 24U) & 0xffU));
    result.push_back(static_cast<char>((size >> 16U) & 0xffU));
    result.push_back(static_cast<char>((size >> 8U) & 0xffU));
    result.push_back(static_cast<char>(size & 0xffU));
    result += body;
    return result;
}

std::vector<std::string> responseBodies(const std::string& bytes) {
    std::vector<std::string> result;
    std::size_t position = 0;
    while (position < bytes.size()) {
        if (bytes.size() - position < 4) {
            throw std::runtime_error("truncated response prefix");
        }
        const auto byte = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes[position + offset]));
        };
        const std::uint32_t size = (byte(0) << 24U) | (byte(1) << 16U)
            | (byte(2) << 8U) | byte(3);
        position += 4;
        if (bytes.size() - position < size) {
            throw std::runtime_error("truncated response body");
        }
        result.push_back(bytes.substr(position, size));
        position += size;
    }
    return result;
}

std::string hardwareHash(std::uint32_t nodes) {
    HtsimNetworkPortConfig port_config;
    port_config.endpoint_count = nodes;
    port_config.link_rate_bps = UINT64_C(400000000000);
    HtsimNetworkPort port(port_config);
    simllm::rnic::RnicDeviceAttachments attachments;
    attachments.network_port = &port;
    simllm::rnic::RnicDevice device(
        defaultSimllmAtlahsDeviceConfig(), attachments);
    const auto record = simllm::rnic::makeStructuralSessionConfigRecord(
        "hash-probe", "rnic-nn", device);
    return *record.hardware_config_sha256;
}

std::string openFrame(std::uint32_t nodes) {
    return frame(
        "{\"effective_hardware_sha256\":\"" + hardwareHash(nodes)
        + "\",\"link_rate_bps\":400000000000,\"node_count\":"
        + std::to_string(nodes)
        + ",\"profile\":\"rnic-nn\",\"schema\":\""
        + kRnicFlowSessionSchema
        + "\",\"seed\":0,\"session_id\":\"native-test\","
          "\"topology_identity\":\"rnic-nn:nodes="
        + std::to_string(nodes)
        + "\",\"verb\":\"open\",\"wqe_authority\":"
          "\"simllm-native-rnic-session\"}");
}

std::string injectFrame(
        std::uint64_t sequence,
        std::uint64_t payload_bytes = 4096,
        std::uint64_t eligible_at_ps = 0,
        std::uint32_t source = 0,
        std::uint32_t destination = 1) {
    return frame(
        "{\"destination\":" + std::to_string(destination)
        + ",\"eligible_at_ps\":" + std::to_string(eligible_at_ps)
        + ",\"execution_id\":"
        "\"execution-" + std::to_string(sequence)
        + "\",\"flow_id\":\"flow-" + std::to_string(sequence)
        + "\",\"operation_id\":\"operation-" + std::to_string(sequence)
        + "\",\"payload_bytes\":" + std::to_string(payload_bytes)
        + ",\"policy_context_token\":9001,\"schema\":\""
        + kRnicFlowSessionSchema + "\",\"sequence\":"
        + std::to_string(sequence)
        + ",\"source\":" + std::to_string(source)
        + ",\"tag\":" + std::to_string(1000 + sequence)
        + ",\"verb\":\"inject\"}");
}

std::string advanceFrame(
        std::uint64_t through_sequence,
        std::uint64_t through_ps) {
    return frame(
        "{\"schema\":\"" + std::string(kRnicFlowSessionSchema)
        + "\",\"through_ps\":" + std::to_string(through_ps)
        + ",\"through_sequence\":" + std::to_string(through_sequence)
        + ",\"verb\":\"advance\"}");
}

std::string cursorFrame(const char* verb, std::uint64_t sequence) {
    return frame(
        "{\"schema\":\"" + std::string(kRnicFlowSessionSchema)
        + "\",\"through_sequence\":" + std::to_string(sequence)
        + ",\"verb\":\"" + verb + "\"}");
}

std::string numbers(const std::vector<std::uint64_t>& values) {
    std::string result = "[";
    for (std::uint64_t value : values) {
        if (result.size() != 1) {
            result += ',';
        }
        result += std::to_string(value);
    }
    return result + ']';
}

std::string awaitFrame(
        std::uint64_t cursor,
        const std::vector<std::uint64_t>& targets,
        bool until_quiescent = false,
        std::optional<std::uint64_t> horizon = std::nullopt,
        std::uint64_t max_time = 1000000000,
        std::uint64_t max_events = 1000000) {
    return frame(
        "{\"completion_sequences\":" + numbers(targets)
        + ",\"max_events\":" + std::to_string(max_events)
        + ",\"max_time_ps\":" + std::to_string(max_time)
        + ",\"schema\":\"" + kRnicFlowSessionSchema
        + "\",\"through_ps\":"
        + (horizon.has_value() ? std::to_string(*horizon) : "null")
        + ",\"through_sequence\":" + std::to_string(cursor)
        + ",\"until_quiescent\":" + (until_quiescent ? "true" : "false")
        + ",\"verb\":\"await_completion\"}");
}

std::string rewrite(
        const std::string& input,
        const std::string& before,
        const std::string& after) {
    std::string body = responseBodies(input).at(0);
    const auto position = body.find(before);
    if (position == std::string::npos) {
        throw std::logic_error("test frame rewrite did not find its field");
    }
    body.replace(position, before.size(), after);
    return frame(body);
}

std::string boundaryFrame(
        std::uint64_t sequence,
        std::uint64_t eligible_at_ps,
        std::uint64_t boundary_id,
        const std::vector<std::uint64_t>& predecessors,
        std::uint32_t source = 1,
        std::uint32_t destination = 0) {
    std::string result = injectFrame(sequence, 4096, eligible_at_ps, source, destination);
    result = rewrite(result, "{\"destination\":",
                     "{\"boundary_id\":" + std::to_string(boundary_id) + ",\"destination\":");
    result = rewrite(result, ",\"schema\":", ",\"predecessor_sequences\":"
                     + numbers(predecessors) + ",\"schema\":");
    return rewrite(result, "\"verb\":\"inject\"", "\"verb\":\"inject_at_boundary\"");
}

std::string finishFrames(std::uint64_t cursor) {
    return awaitFrame(cursor, {}, true) + cursorFrame("drain", cursor)
        + cursorFrame("close", cursor);
}

std::size_t occurrences(const std::string& value, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = value.find(needle); at != std::string::npos;
         at = value.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

constexpr std::uint64_t kOnePacketCompletionPs = 2000000 + 2 * 83200;

struct RunResult {
    int return_code{0};
    std::vector<std::string> responses;
    std::string error;
};

RunResult run(const std::string& input_bytes) {
    std::istringstream input(input_bytes);
    std::ostringstream output;
    std::ostringstream error;
    const int return_code = runRnicFlowSession(
        input, output, error, "htsim-test", "simllm-test");
    return RunResult{
        return_code,
        responseBodies(output.str()),
        error.str(),
    };
}

class ScriptedFrames final : public std::streambuf {
public:
    explicit ScriptedFrames(std::function<std::string()> next)
        : next_(std::move(next)) {}

protected:
    int_type underflow() override {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        current_ = next_();
        if (current_.empty()) {
            return traits_type::eof();
        }
        setg(current_.data(), current_.data(), current_.data() + current_.size());
        return traits_type::to_int_type(*gptr());
    }

private:
    std::function<std::string()> next_;
    std::string current_;
};

std::uint64_t responseUnsigned(const std::string& response, const std::string& name) {
    const std::string field = "\"" + name + "\":";
    const auto position = response.find(field);
    if (position == std::string::npos) {
        throw std::logic_error("response omitted the test field");
    }
    return std::stoull(response.substr(position + field.size()));
}

TEST(RnicFlowSessionTest, RetainsQueueStateAndDrainsConservedRows) {
    const RunResult result = run(
        openFrame(2)
        + injectFrame(1)
        + injectFrame(2)
        + advanceFrame(2, 10000000)
        + cursorFrame("drain", 2)
        + cursorFrame("close", 2));

    ASSERT_EQ(result.return_code, 0);
    EXPECT_TRUE(result.error.empty());
    ASSERT_EQ(result.responses.size(), 6U);
    EXPECT_NE(result.responses[3].find("\"kind\":\"accepted\""),
              std::string::npos);
    EXPECT_NE(result.responses[3].find("\"kind\":\"queued\""),
              std::string::npos);
    EXPECT_NE(result.responses[3].find("\"kind\":\"started\""),
              std::string::npos);
    EXPECT_NE(result.responses[3].find("\"kind\":\"completed\""),
              std::string::npos);
    EXPECT_NE(result.responses[4].find("\"sq_high_watermarks\":[2,0]"),
              std::string::npos);
    EXPECT_NE(result.responses[4].find("\"native_posts\":2"),
              std::string::npos);
    EXPECT_NE(result.responses[4].find("\"legacy_mutations\":0"),
              std::string::npos);
    EXPECT_NE(result.responses[4].find("\"quiescent\":true"),
              std::string::npos);
    EXPECT_NE(result.responses[5].find("\"terminal\":true"),
              std::string::npos);
}

TEST(RnicFlowSessionTest, DuplicateSequenceFailsBeforeNativePost) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + injectFrame(1));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 3U);
    EXPECT_NE(result.responses.back().find("\"code\":\"duplicate_sequence\""),
              std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":0"),
              std::string::npos);
}

TEST(RnicFlowSessionTest, SkippedSequenceFailsBeforeNativePost) {
    const RunResult result = run(openFrame(2) + injectFrame(2));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 2U);
    EXPECT_NE(result.responses.back().find("\"code\":\"skipped_sequence\""),
              std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":0"),
              std::string::npos);
}

TEST(RnicFlowSessionTest, StaleHorizonFailsWithoutAnotherNativePost) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + advanceFrame(1, 1)
        + advanceFrame(1, 0));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 4U);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_horizon\""),
              std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"),
              std::string::npos);
}

TEST(RnicFlowSessionTest, CompleteFrameAfterCloseIsExplicitlyRejected) {
    const RunResult result = run(
        openFrame(2) + cursorFrame("drain", 0)
        + cursorFrame("close", 0) + injectFrame(1));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 4U);
    EXPECT_NE(result.responses.back().find("\"code\":\"post_terminal\""),
              std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":0"),
              std::string::npos);
}

TEST(RnicFlowSessionTest, PartialBodyIsNeverDispatched) {
    const std::string complete = injectFrame(1);
    const RunResult result = run(
        openFrame(2) + complete.substr(0, complete.size() / 2));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 1U);
    EXPECT_NE(result.error.find("EOF interrupted the declared frame body"),
              std::string::npos);
    EXPECT_EQ(result.responses.front().find("\"accepted_sequence\""),
              std::string::npos);
}

TEST(RnicFlowSessionTest, NoncanonicalJsonFailsBeforeOpen) {
    const std::string noncanonical =
        "{\"verb\":\"open\",\"schema\":\""
        + std::string(kRnicFlowSessionSchema) + "\"}";
    const RunResult result = run(frame(noncanonical));
    ASSERT_EQ(result.return_code, 2);
    ASSERT_EQ(result.responses.size(), 1U);
    EXPECT_NE(result.responses.back().find("\"code\":\"noncanonical_json\""),
              std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":0"),
              std::string::npos);
}


TEST(RnicFlowSessionTest, ExactCompletionBoundaryReleasesDependentFlow) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + boundaryFrame(2, time, 1, {1}) + awaitFrame(2, {2})
        + finishFrames(2));
    ASSERT_EQ(result.return_code, 0) << result.error;
    ASSERT_EQ(result.responses.size(), 8U);
    const auto& first = result.responses[2];
    EXPECT_NE(first.find("\"reason\":\"completion\""), std::string::npos);
    EXPECT_NE(first.find("\"boundary_id\":1"), std::string::npos);
    EXPECT_NE(first.find("\"boundary_time_ps\":" + std::to_string(time)), std::string::npos);
    EXPECT_NE(first.find("\"fully_processed_horizon_ps\":" + std::to_string(time - 1)), std::string::npos);
    EXPECT_NE(first.find("\"ordinary_injection_floor_ps\":" + std::to_string(time)), std::string::npos);
    EXPECT_NE(first.find("\"completion_status\":\"success\""), std::string::npos);
    EXPECT_NE(result.responses[4].find("\"boundary_id\":2"), std::string::npos);
    EXPECT_NE(result.responses[4].find("\"completion_time_ps\":" + std::to_string(2 * time)), std::string::npos);
    EXPECT_NE(result.responses[4].find("\"start_time_ps\":" + std::to_string(time)), std::string::npos);
    EXPECT_EQ(result.responses[6].find("completion_status"), std::string::npos);
    EXPECT_NE(result.responses[6].find("\"native_posts\":2"), std::string::npos);
}

TEST(RnicFlowSessionTest, BoundaryTokenAcceptsMultipleInjectionsBeforeEqualTimeAdvance) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + boundaryFrame(2, time, 1, {1}) + boundaryFrame(3, time, 1, {1})
        + advanceFrame(3, time) + finishFrames(3));
    ASSERT_EQ(result.return_code, 0) << result.error;
    ASSERT_EQ(result.responses.size(), 9U);
    EXPECT_NE(result.responses[3].find("\"boundary_id\":1"), std::string::npos);
    EXPECT_NE(result.responses[4].find("\"boundary_id\":1"), std::string::npos);
    EXPECT_NE(result.responses[5].find("\"native_posts\":3"), std::string::npos);
    EXPECT_EQ(occurrences(result.responses[5], "\"kind\":\"accepted\""), 2U);
    EXPECT_NE(result.responses[7].find("\"sq_high_watermarks\":[1,2]"), std::string::npos);
}

TEST(RnicFlowSessionTest, OrdinaryInjectionCannotReuseExposedCompletionTime) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + injectFrame(2, 4096, kOnePacketCompletionPs, 1, 0));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_eligibility\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
}

TEST(RnicFlowSessionTest, EqualTimeAdvanceInvalidatesBoundary) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + advanceFrame(1, time) + boundaryFrame(2, time, 1, {1}));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_boundary\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
}

TEST(RnicFlowSessionTest, AdvancingAwaitInvalidatesPreviousBoundary) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + boundaryFrame(2, time, 1, {1}) + awaitFrame(2, {2})
        + boundaryFrame(3, 2 * time, 1, {2}));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses[4].find("\"boundary_id\":2"), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_boundary\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":2"), std::string::npos);
}

TEST(RnicFlowSessionTest, QuiescenceYieldInvalidatesBoundaryWithoutCallbacks) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + awaitFrame(1, {}, true)
        + boundaryFrame(2, kOnePacketCompletionPs, 1, {1}));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses[3].find("\"reason\":\"quiescence\""), std::string::npos);
    EXPECT_NE(result.responses[3].find("\"boundary_id\":null"), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_boundary\""), std::string::npos);
}

TEST(RnicFlowSessionTest, CompletionWinsAtInclusiveHardTimeAndNormalHorizon) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1}, false, time, time)
        + finishFrames(1));
    ASSERT_EQ(result.return_code, 0) << result.error;
    EXPECT_NE(result.responses[2].find("\"reason\":\"completion\""), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"event_time_ps\":" + std::to_string(time)), std::string::npos);
}

TEST(RnicFlowSessionTest, NormalHorizonYieldsBeforeCompletionWithoutConsumingIt) {
    const auto time = kOnePacketCompletionPs;
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1}, false, time - 1)
        + awaitFrame(1, {1}, false, time) + finishFrames(1));
    ASSERT_EQ(result.return_code, 0) << result.error;
    EXPECT_NE(result.responses[2].find("\"reason\":\"horizon\""), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"completion_rows\":[]"), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"boundary_id\":null"), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"fully_processed_horizon_ps\":" + std::to_string(time - 1)), std::string::npos);
    EXPECT_NE(result.responses[3].find("\"reason\":\"completion\""), std::string::npos);
}

TEST(RnicFlowSessionTest, HardTimeFailureDoesNotRunLaterCompletion) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1)
        + awaitFrame(1, {1}, false, std::nullopt, kOnePacketCompletionPs - 1));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"time_limit\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"terminal\":true"), std::string::npos);
    EXPECT_EQ(result.responses.back().find("completion_rows"), std::string::npos);
}

TEST(RnicFlowSessionTest, CallbackLimitIsTerminalAfterAllowedNativePost) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1)
        + awaitFrame(1, {1}, false, std::nullopt, 1000000000, 1));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"event_limit\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
}

TEST(RnicFlowSessionTest, FinalAllowedCallbackCanSatisfyCompletion) {
    std::ostringstream output, error;
    std::size_t stage = 0;
    std::uint64_t measured_callbacks = 0;
    ScriptedFrames frames([&]() {
        switch (stage++) {
        case 0: return openFrame(2) + injectFrame(1) + awaitFrame(1, {1});
        case 1: {
            const auto responses = responseBodies(output.str());
            measured_callbacks = responseUnsigned(responses.at(2), "events_executed");
            return boundaryFrame(2, kOnePacketCompletionPs, 1, {1})
                + awaitFrame(2, {2}, false, std::nullopt, 1000000000, measured_callbacks);
        }
        case 2: return finishFrames(2);
        default: return std::string{};
        }
    });
    std::istream input(&frames);
    const int code = runRnicFlowSession(input, output, error, "htsim-test", "simllm-test");
    const auto responses = responseBodies(output.str());
    ASSERT_EQ(code, 0) << output.str() << error.str();
    ASSERT_GT(measured_callbacks, 0U);
    EXPECT_EQ(responseUnsigned(responses.at(4), "events_executed"), measured_callbacks);
    EXPECT_NE(responses.at(4).find("\"reason\":\"completion\""), std::string::npos);
}

TEST(RnicFlowSessionTest, InitialQuiescenceNeedsNoCallbackOrArtificialPrefix) {
    const RunResult result = run(
        openFrame(2) + awaitFrame(0, {}, true, std::nullopt, 1, 1)
        + cursorFrame("drain", 0) + cursorFrame("close", 0));
    ASSERT_EQ(result.return_code, 0) << result.error;
    const auto& response = result.responses[1];
    EXPECT_NE(response.find("\"event_time_ps\":0"), std::string::npos);
    EXPECT_NE(response.find("\"events_executed\":0"), std::string::npos);
    EXPECT_NE(response.find("\"fully_processed_horizon_ps\":null"), std::string::npos);
    EXPECT_NE(response.find("\"ordinary_injection_floor_ps\":null"), std::string::npos);
    EXPECT_NE(response.find("\"quiescent\":true"), std::string::npos);
}

TEST(RnicFlowSessionTest, QuiescenceBudgetUsesEventTimeRatherThanAnEmptyAdvancedPrefix) {
    const RunResult result = run(
        openFrame(2) + advanceFrame(0, 10000000)
        + awaitFrame(0, {}, true, std::nullopt, 1, 1)
        + cursorFrame("drain", 0) + cursorFrame("close", 0));
    ASSERT_EQ(result.return_code, 0) << result.error;
    EXPECT_NE(result.responses[2].find("\"event_time_ps\":0"), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"events_executed\":0"), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"fully_processed_horizon_ps\":10000000"), std::string::npos);
    EXPECT_NE(result.responses[2].find("\"reason\":\"quiescence\""), std::string::npos);
}

TEST(RnicFlowSessionTest, ZeroHorizonDoesNotFireFutureAcceptedInjection) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1, 4096, 1)
        + awaitFrame(1, {1}, false, 0, 1, 1) + finishFrames(1));
    ASSERT_EQ(result.return_code, 0) << result.error;
    const auto& response = result.responses[2];
    EXPECT_NE(response.find("\"reason\":\"horizon\""), std::string::npos);
    EXPECT_NE(response.find("\"events_executed\":0"), std::string::npos);
    EXPECT_NE(response.find("\"fully_processed_horizon_ps\":0"), std::string::npos);
    EXPECT_NE(response.find("\"ordinary_injection_floor_ps\":0"), std::string::npos);
    EXPECT_NE(response.find("\"native_posts\":0"), std::string::npos);
    EXPECT_NE(response.find("\"quiescent\":false"), std::string::npos);
}

TEST(RnicFlowSessionTest, AwaitStopsAtAnyNamedTargetRatherThanTheFirstSequence) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1, 8192) + injectFrame(2, 4096, 0, 1, 0)
        + awaitFrame(2, {1, 2}) + finishFrames(2));
    ASSERT_EQ(result.return_code, 0) << result.error;
    const auto& response = result.responses[3];
    EXPECT_EQ(occurrences(response, "\"completion_status\":\"success\""), 1U);
    EXPECT_NE(response.find("\"boundary_time_ps\":" + std::to_string(kOnePacketCompletionPs)), std::string::npos);
    EXPECT_NE(response.find("\"flow_id\":\"flow-2\""), std::string::npos);
    EXPECT_NE(response.find("\"quiescent\":false"), std::string::npos);
}

TEST(RnicFlowSessionTest, SameCallbackReturnsCompletionsOutsideNamedTargets) {
    const RunResult result = run(
        openFrame(4) + injectFrame(1) + injectFrame(2, 4096, 0, 2, 3)
        + awaitFrame(2, {1}) + finishFrames(2));
    ASSERT_EQ(result.return_code, 0) << result.error;
    EXPECT_EQ(occurrences(result.responses[3], "\"completion_status\":\"success\""), 2U);
    EXPECT_NE(result.responses[3].find("\"boundary_time_ps\":" + std::to_string(kOnePacketCompletionPs)), std::string::npos);
    EXPECT_NE(result.responses[4].find("\"completion_rows\":[]"), std::string::npos);
}

TEST(RnicFlowSessionTest, TargetAlreadyCompletedByLegacyAdvanceIsRejected) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + advanceFrame(1, 10000000)
        + awaitFrame(1, {1}));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"target_not_pending\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
}

TEST(RnicFlowSessionTest, HorizonCannotMoveBackIntoAnOpenBoundary) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + awaitFrame(1, {1})
        + boundaryFrame(2, kOnePacketCompletionPs, 1, {1})
        + awaitFrame(2, {2}, false, kOnePacketCompletionPs - 1));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"stale_horizon\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
}

class InvalidAwaitTest : public ::testing::TestWithParam<int> {};

TEST_P(InvalidAwaitTest, RejectsEveryFieldBeforeFirstNativeCallback) {
    std::string request = awaitFrame(2, {1, 2});
    switch (GetParam()) {
    case 0: request = awaitFrame(1, {1}); break;
    case 1: request = awaitFrame(2, {2, 1}); break;
    case 2: request = awaitFrame(2, {1, 1}); break;
    case 3: request = awaitFrame(2, {3}); break;
    case 4: request = awaitFrame(2, {}); break;
    case 5: request = awaitFrame(2, {1}, true); break;
    case 6: request = awaitFrame(2, {1}, false, std::nullopt, 0); break;
    case 7: request = awaitFrame(2, {1}, false, std::nullopt, 1, 0); break;
    case 8: request = awaitFrame(2, {1}, false, 2, 1); break;
    case 9: request = rewrite(request, "\"until_quiescent\":false", "\"until_quiescent\":0"); break;
    case 10: request = rewrite(request, "\"max_events\":1000000", "\"max_events\":true"); break;
    case 11: request = rewrite(request, "\"completion_sequences\":[1,2]", "\"completion_sequences\":[true]"); break;
    case 12: request = rewrite(request, "\"through_ps\":null", "\"through_ps\":false"); break;
    case 13: request = rewrite(request, "\"max_time_ps\":1000000000,", ""); break;
    case 14: request = rewrite(request, "{", "{\"added\":1,"); break;
    default: FAIL() << "unknown mutation";
    }
    const RunResult result = run(openFrame(2) + injectFrame(1) + injectFrame(2) + request);
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"native_posts\":0"), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"status\":\"error\""), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(StrictFields, InvalidAwaitTest, ::testing::Range(0, 15));

class InvalidBoundaryTest : public ::testing::TestWithParam<int> {};

TEST_P(InvalidBoundaryTest, RejectsBeforeSchedulingOrConsumingAnotherSequence) {
    const auto time = kOnePacketCompletionPs;
    std::string request = boundaryFrame(2, time, 1, {1});
    switch (GetParam()) {
    case 0: request = boundaryFrame(1, time, 1, {1}); break;
    case 1: request = boundaryFrame(3, time, 1, {1}); break;
    case 2: request = boundaryFrame(2, time, 2, {1}); break;
    case 3: request = boundaryFrame(2, time + 1, 1, {1}); break;
    case 4: request = boundaryFrame(2, time, 1, {}); break;
    case 5: request = boundaryFrame(2, time, 1, {1, 1}); break;
    case 6: request = boundaryFrame(2, time, 1, {2}); break;
    case 7: request = rewrite(request, "\"boundary_id\":1", "\"boundary_id\":true"); break;
    case 8: request = rewrite(request, "\"policy_context_token\":9001", "\"policy_context_token\":1"); break;
    case 9:
        request = rewrite(request, "execution-2", "execution-1");
        request = rewrite(request, "operation-2", "operation-1");
        request = rewrite(request, "flow-2", "flow-1");
        break;
    case 10: request = boundaryFrame(2, time, 1, {1}, 1, 1); break;
    case 11: request = rewrite(request, "\"payload_bytes\":4096", "\"payload_bytes\":0"); break;
    default: FAIL() << "unknown mutation";
    }
    const RunResult result = run(openFrame(2) + injectFrame(1) + awaitFrame(1, {1}) + request);
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses[2].find("\"last_accepted_sequence\":1"), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":1"), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"status\":\"error\""), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(StrictFields, InvalidBoundaryTest, ::testing::Range(0, 12));

TEST(RnicFlowSessionTest, BoundaryPredecessorMustHaveCompleted) {
    const RunResult result = run(
        openFrame(2) + injectFrame(1) + injectFrame(2, 8192, 0, 1, 0)
        + awaitFrame(2, {1})
        + boundaryFrame(3, kOnePacketCompletionPs, 1, {2}));
    ASSERT_EQ(result.return_code, 2);
    EXPECT_NE(result.responses.back().find("\"code\":\"predecessor_pending\""), std::string::npos);
    EXPECT_NE(result.responses.back().find("\"native_posts\":2"), std::string::npos);
}

}  // namespace
