// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <sstream>
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
        std::uint64_t payload_bytes = 4096) {
    return frame(
        "{\"destination\":1,\"eligible_at_ps\":0,\"execution_id\":"
        "\"execution-" + std::to_string(sequence)
        + "\",\"flow_id\":\"flow-" + std::to_string(sequence)
        + "\",\"operation_id\":\"operation-" + std::to_string(sequence)
        + "\",\"payload_bytes\":" + std::to_string(payload_bytes)
        + ",\"policy_context_token\":9001,\"schema\":\""
        + kRnicFlowSessionSchema + "\",\"sequence\":"
        + std::to_string(sequence)
        + ",\"source\":0,\"tag\":" + std::to_string(1000 + sequence)
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

}  // namespace
