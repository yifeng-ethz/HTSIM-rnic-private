// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "lgs/Parser.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kScheduleOffset = sizeof(uint64_t);
constexpr std::size_t kJumpStartOffset =
    kScheduleOffset + sizeof(uint32_t) + sizeof(uint8_t)*2;
constexpr std::size_t kJumpEndOffset = kJumpStartOffset + sizeof(uint64_t);
constexpr std::size_t kRankStart =
    sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2;
constexpr std::size_t kRankAbsolute = kScheduleOffset + kRankStart;
constexpr std::size_t kRootAbsolute = kRankAbsolute + sizeof(uint32_t)*2;
constexpr std::size_t kNodeBytes =
    sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*7
    + sizeof(uint8_t)*2;
constexpr std::size_t kNode0Absolute = kRootAbsolute + sizeof(uint32_t);
constexpr std::size_t kNode1Absolute = kNode0Absolute + kNodeBytes;
constexpr std::size_t kAppendixAbsolute = kNode1Absolute + kNodeBytes;

template <typename T>
void appendScalar(std::vector<char>& bytes, const T& value) {
    const std::size_t old_size = bytes.size();
    bytes.resize(old_size + sizeof(value));
    std::memcpy(bytes.data() + old_size, &value, sizeof(value));
}

template <typename T>
void overwriteScalar(
        std::vector<char>& bytes, std::size_t offset, const T& value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) {
        throw std::out_of_range("test scalar overwrite is out of range");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendNode(
        std::vector<char>& bytes,
        uint32_t incoming_dependencies,
        char type,
        uint32_t normal_count,
        uint32_t normal_start,
        uint32_t start_count,
        uint32_t start_start) {
    appendScalar(bytes, incoming_dependencies);
    appendScalar(bytes, type);
    appendScalar(bytes, uint32_t{0});
    appendScalar(bytes, uint64_t{100});
    appendScalar(bytes, uint32_t{0});
    appendScalar(bytes, uint8_t{0});
    appendScalar(bytes, uint8_t{0});
    appendScalar(bytes, normal_count);
    appendScalar(bytes, normal_start);
    appendScalar(bytes, start_count);
    appendScalar(bytes, start_start);
}

std::vector<char> twoNodeSchedule(bool start_dependency = false) {
    std::vector<char> rank;
    appendScalar(rank, uint32_t{2});
    appendScalar(rank, uint32_t{1});
    appendScalar(rank, uint32_t{0});
    appendNode(
        rank,
        0,
        OPTYPE_CALC,
        start_dependency ? 0u : 1u,
        0,
        start_dependency ? 1u : 0u,
        start_dependency ? 0u : 1u);
    appendNode(rank, 1, OPTYPE_CALC, 0, 1, 0, 1);
    appendScalar(rank, uint32_t{1});

    const uint64_t rank_end = kRankStart + rank.size();
    std::vector<char> bytes;
    appendScalar(bytes, uint64_t{MAGIC_COOKIE});
    appendScalar(bytes, uint32_t{1});
    appendScalar(bytes, uint8_t{0});
    appendScalar(bytes, uint8_t{0});
    appendScalar(bytes, uint64_t{kRankStart});
    appendScalar(bytes, rank_end);
    bytes.insert(bytes.end(), rank.begin(), rank.end());
    return bytes;
}

class TemporarySchedule {
public:
    TemporarySchedule() : descriptor_(-1) {
        char pattern[] = "/tmp/htsim-goal-parser-XXXXXX";
        descriptor_ = mkstemp(pattern);
        if (descriptor_ < 0) {
            throw std::runtime_error("mkstemp failed");
        }
        path_ = pattern;
    }

    explicit TemporarySchedule(const std::vector<char>& bytes)
            : TemporarySchedule() {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(
                descriptor_, bytes.data() + written, bytes.size() - written);
            if (count <= 0) {
                closeWriter();
                throw std::runtime_error("temporary schedule write failed");
            }
            written += static_cast<std::size_t>(count);
        }
        closeWriter();
    }

    TemporarySchedule(const TemporarySchedule&) = delete;
    TemporarySchedule& operator=(const TemporarySchedule&) = delete;

    ~TemporarySchedule() {
        closeWriter();
        if (!path_.empty()) {
            (void)unlink(path_.c_str());
        }
    }

    int descriptor() const { return descriptor_; }
    const std::string& path() const { return path_; }

    void closeWriter() {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
            descriptor_ = -1;
        }
    }

private:
    int descriptor_;
    std::string path_;
};

void expectMalformed(const std::vector<char>& bytes) {
    TemporarySchedule schedule(bytes);
    EXPECT_THROW(
        {
            Parser parser(schedule.path(), false);
            (void)parser;
        },
        std::runtime_error);
}

TEST(GoalParserTest, DecodesPackedNodesAndInitializesRootStartTime) {
    TemporarySchedule schedule(twoNodeSchedule());
    Parser parser(schedule.path(), false);

    ASSERT_EQ(parser.schedules.size(), 1u);
    EXPECT_EQ(parser.schedules[0].GetNumNodes(), 2u);
    SerializedGraph::nodelist_t roots;
    parser.schedules[0].GetExecutableNodes(&roots);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].offset, 0u);
    EXPECT_EQ(roots[0].type, OP_LOCOP);
    EXPECT_EQ(roots[0].starttime, 0u);

    parser.schedules[0].MarkNodeAsDone(0, 123);
    SerializedGraph::nodelist_t unlocked;
    parser.schedules[0].GetExecutableNodes(&unlocked);
    ASSERT_EQ(unlocked.size(), 1u);
    EXPECT_EQ(unlocked[0].offset, 1u);
    EXPECT_EQ(unlocked[0].starttime, 123u);
}

TEST(GoalParserTest, StartDependencyUsesCheckedPackedCounterUpdate) {
    TemporarySchedule schedule(twoNodeSchedule(true));
    Parser parser(schedule.path(), false);
    SerializedGraph::nodelist_t roots;
    parser.schedules[0].GetExecutableNodes(&roots);
    ASSERT_EQ(roots.size(), 1u);

    parser.schedules[0].MarkNodeAsStarted(0);
    SerializedGraph::nodelist_t unlocked;
    parser.schedules[0].GetExecutableNodes(&unlocked);
    ASSERT_EQ(unlocked.size(), 1u);
    EXPECT_EQ(unlocked[0].offset, 1u);
    EXPECT_EQ(unlocked[0].starttime, 0u);
    EXPECT_THROW(
        parser.schedules[0].MarkNodeAsStarted(0), std::logic_error);
    EXPECT_THROW(
        parser.schedules[0].MarkNodeAsDone(2, 0), std::out_of_range);
}

TEST(GoalParserTest, SerializerRoundTripsPackedDependencyRecords) {
    TemporarySchedule schedule;
    Graph graph;
    Node* first = graph.addNode();
    Node* second = graph.addNode();
    for (Node* node : {first, second}) {
        node->Type = OPTYPE_CALC;
        node->Peer = 0;
        node->Size = 100;
        node->Tag = 0;
        node->Proc = 0;
        node->Nic = 0;
    }
    graph.addDependency(second, first);
    graph.serialize_mmap(schedule.descriptor(), 0, 1, 0, 0);
    schedule.closeWriter();

    Parser parser(schedule.path(), false);
    SerializedGraph::nodelist_t roots;
    parser.schedules[0].GetExecutableNodes(&roots);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].offset, first->offset);
    parser.schedules[0].MarkNodeAsDone(first->offset, 77);
    SerializedGraph::nodelist_t unlocked;
    parser.schedules[0].GetExecutableNodes(&unlocked);
    ASSERT_EQ(unlocked.size(), 1u);
    EXPECT_EQ(unlocked[0].offset, second->offset);
    EXPECT_EQ(unlocked[0].starttime, 77u);
}

TEST(GoalParserTest, SerializerRoundTripsMultiplePackedRanks) {
    TemporarySchedule schedule;
    Graph first_rank;
    Graph second_rank;

    Node* first = first_rank.addNode();
    Node* second = second_rank.addNode();
    for (Node* node : {first, second}) {
        node->Type = OPTYPE_CALC;
        node->Peer = 0;
        node->Size = 100;
        node->Tag = 0;
        node->Proc = 0;
        node->Nic = 0;
    }

    first_rank.serialize_mmap(schedule.descriptor(), 0, 2, 0, 0);
    second_rank.serialize_mmap(schedule.descriptor(), 1, 2, 0, 0);
    schedule.closeWriter();

    Parser parser(schedule.path(), false);
    ASSERT_EQ(parser.schedules.size(), 2u);
    for (SerializedGraph& graph : parser.schedules) {
        EXPECT_EQ(graph.GetNumNodes(), 1u);
        SerializedGraph::nodelist_t roots;
        graph.GetExecutableNodes(&roots);
        ASSERT_EQ(roots.size(), 1u);
        EXPECT_EQ(roots[0].offset, 0u);
        EXPECT_EQ(roots[0].starttime, 0u);
    }
}

TEST(GoalParserTest, RejectsTruncatedHeadersAndRankSpans) {
    std::vector<char> truncated = twoNodeSchedule();
    truncated.resize(kScheduleOffset + sizeof(uint32_t) + sizeof(uint8_t)*2);
    expectMalformed(truncated);

    std::vector<char> before_header = twoNodeSchedule();
    overwriteScalar(before_header, kJumpStartOffset, uint64_t{1});
    expectMalformed(before_header);

    std::vector<char> reversed = twoNodeSchedule();
    overwriteScalar(reversed, kJumpStartOffset, uint64_t{200});
    expectMalformed(reversed);

    std::vector<char> beyond_file = twoNodeSchedule();
    overwriteScalar(
        beyond_file, kJumpEndOffset,
        static_cast<uint64_t>(beyond_file.size() + 100));
    expectMalformed(beyond_file);

    std::vector<char> truncated_nodes = twoNodeSchedule();
    truncated_nodes.resize(kNode1Absolute + 5);
    overwriteScalar(
        truncated_nodes, kJumpEndOffset,
        static_cast<uint64_t>(truncated_nodes.size() - kScheduleOffset));
    expectMalformed(truncated_nodes);
}

TEST(GoalParserTest, RejectsInvalidRootTypeAndDependencyMetadata) {
    std::vector<char> bad_root = twoNodeSchedule();
    overwriteScalar(bad_root, kRootAbsolute, uint32_t{2});
    expectMalformed(bad_root);

    std::vector<char> bad_type = twoNodeSchedule();
    overwriteScalar(
        bad_type, kNode0Absolute + sizeof(uint32_t), char{99});
    expectMalformed(bad_type);

    std::vector<char> bad_range = twoNodeSchedule();
    overwriteScalar(bad_range, kNode0Absolute + 23, uint32_t{2});
    expectMalformed(bad_range);

    std::vector<char> bad_dependency = twoNodeSchedule();
    overwriteScalar(bad_dependency, kAppendixAbsolute, uint32_t{2});
    expectMalformed(bad_dependency);

    std::vector<char> mismatched_count = twoNodeSchedule();
    overwriteScalar(mismatched_count, kNode1Absolute, uint32_t{0});
    expectMalformed(mismatched_count);

    std::vector<char> partial_appendix = twoNodeSchedule();
    partial_appendix.pop_back();
    overwriteScalar(
        partial_appendix, kJumpEndOffset,
        static_cast<uint64_t>(partial_appendix.size() - kScheduleOffset));
    expectMalformed(partial_appendix);
}

}  // namespace
