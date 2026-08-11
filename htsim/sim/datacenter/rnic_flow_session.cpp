// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_flow_session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "atlahs_htsim_api.h"
#include "eventlist.h"
#include "rnic_atlahs_driver.h"
#include "rnic_profile.h"
#include "simllm_atlahs_flow_runtime.h"
#include "simllm/rnic/work_queue.h"

namespace {

using htsim::simllm_rnic::SimllmAtlahsFlowRuntime;
using htsim::simllm_rnic::SimllmAtlahsRuntimeConfig;
using htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig;
using htsim::simllm_rnic::makeComposedSimllmAtlahsFlowRuntime;
using simllm::rnic::Picoseconds;
using simllm::rnic::WqeRecord;

class ProtocolError final : public std::runtime_error {
public:
    ProtocolError(std::string code_value, std::string message)
        : std::runtime_error(std::move(message)),
          code_(std::move(code_value)) {}

    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

class Json final {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<
        std::nullptr_t,
        bool,
        std::uint64_t,
        std::string,
        Array,
        Object>;

    Json() : value_(nullptr) {}
    explicit Json(std::nullptr_t) : value_(nullptr) {}
    explicit Json(bool value) : value_(value) {}
    explicit Json(std::uint64_t value) : value_(value) {}
    explicit Json(std::string value) : value_(std::move(value)) {}
    explicit Json(const char* value) : value_(std::string(value)) {}
    explicit Json(Array value) : value_(std::move(value)) {}
    explicit Json(Object value) : value_(std::move(value)) {}

    const Value& value() const noexcept { return value_; }

private:
    Value value_;
};

bool validUtf8(const std::string& value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first =
            static_cast<unsigned char>(value[index]);
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            codepoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            codepoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1;
             offset <= continuation_count;
             ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        const std::uint32_t minimum = continuation_count == 1
            ? 0x80U
            : (continuation_count == 2 ? 0x800U : 0x10000U);
        if (codepoint < minimum || codepoint > 0x10ffffU
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

void appendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(
            0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(
            0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(
            0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class JsonParser final {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    Json parse() {
        Json result = parseValue();
        if (position_ != input_.size()) {
            fail("trailing data");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ProtocolError(
            "invalid_json",
            "JSON byte " + std::to_string(position_) + ": " + message);
    }

    bool consume(char character) {
        if (position_ < input_.size()
            && input_[position_] == character) {
            ++position_;
            return true;
        }
        return false;
    }

    void require(char character) {
        if (!consume(character)) {
            fail(std::string("expected '") + character + "'");
        }
    }

    void requireLiteral(const char* literal) {
        const std::size_t size = std::strlen(literal);
        if (input_.compare(position_, size, literal) != 0) {
            fail(std::string("expected ") + literal);
        }
        position_ += size;
    }

    static int hexDigit(char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    std::uint32_t parseHexCodeUnit() {
        if (position_ + 4 > input_.size()) {
            fail("truncated Unicode escape");
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = hexDigit(input_[position_++]);
            if (digit < 0) {
                fail("invalid Unicode escape");
            }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::string parseString() {
        require('"');
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                if (!validUtf8(result)) {
                    fail("string is not valid UTF-8");
                }
                return result;
            }
            if (character < 0x20U) {
                fail("unescaped control character");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size()) {
                fail("truncated string escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = parseHexCodeUnit();
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (position_ + 2 > input_.size()
                        || input_[position_] != '\\'
                        || input_[position_ + 1] != 'u') {
                        fail("high surrogate lacks a low surrogate");
                    }
                    position_ += 2;
                    const std::uint32_t low = parseHexCodeUnit();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("invalid low surrogate");
                    }
                    codepoint = 0x10000U
                        + ((codepoint - 0xd800U) << 10U)
                        + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U
                           && codepoint <= 0xdfffU) {
                    fail("unpaired low surrogate");
                }
                appendUtf8(result, codepoint);
                break;
            }
            default:
                fail("unknown string escape");
            }
        }
        fail("unterminated string");
    }

    Json parseNumber() {
        const std::size_t start = position_;
        if (consume('0')) {
            if (position_ < input_.size()
                && std::isdigit(
                    static_cast<unsigned char>(input_[position_]))) {
                fail("integer has a leading zero");
            }
        } else {
            if (position_ == input_.size()
                || input_[position_] < '1'
                || input_[position_] > '9') {
                fail("expected an unsigned integer");
            }
            while (position_ < input_.size()
                   && std::isdigit(static_cast<unsigned char>(
                       input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size()
            && (input_[position_] == '.' || input_[position_] == 'e'
                || input_[position_] == 'E')) {
            fail("floating-point numbers are unsupported");
        }
        const std::string text = input_.substr(start, position_ - start);
        std::uint64_t value = 0;
        for (char character : text) {
            const std::uint64_t digit =
                static_cast<std::uint64_t>(character - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit)
                            / 10U) {
                fail("integer exceeds uint64");
            }
            value = value * 10U + digit;
        }
        return Json(value);
    }

    Json parseArray() {
        require('[');
        Json::Array result;
        if (consume(']')) {
            return Json(std::move(result));
        }
        while (true) {
            result.push_back(parseValue());
            if (consume(']')) {
                return Json(std::move(result));
            }
            require(',');
        }
    }

    Json parseObject() {
        require('{');
        Json::Object result;
        if (consume('}')) {
            return Json(std::move(result));
        }
        while (true) {
            if (position_ == input_.size()
                || input_[position_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parseString();
            require(':');
            Json value = parseValue();
            if (!result.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate object key");
            }
            if (consume('}')) {
                return Json(std::move(result));
            }
            require(',');
        }
    }

    Json parseValue() {
        if (position_ == input_.size()) {
            fail("expected a value");
        }
        switch (input_[position_]) {
        case 'n':
            requireLiteral("null");
            return Json(nullptr);
        case 't':
            requireLiteral("true");
            return Json(true);
        case 'f':
            requireLiteral("false");
            return Json(false);
        case '"':
            return Json(parseString());
        case '[':
            return parseArray();
        case '{':
            return parseObject();
        default:
            return parseNumber();
        }
    }

    const std::string& input_;
    std::size_t position_{0};
};

std::string renderJsonString(const std::string& value) {
    if (!validUtf8(value)) {
        throw std::logic_error("cannot render invalid UTF-8 JSON string");
    }
    std::ostringstream output;
    output << '"';
    for (unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << std::hex << std::setw(2)
                       << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

std::string renderJson(const Json& value) {
    const Json::Value& raw = value.value();
    if (std::holds_alternative<std::nullptr_t>(raw)) {
        return "null";
    }
    if (const bool* boolean = std::get_if<bool>(&raw)) {
        return *boolean ? "true" : "false";
    }
    if (const std::uint64_t* number =
            std::get_if<std::uint64_t>(&raw)) {
        return std::to_string(*number);
    }
    if (const std::string* string = std::get_if<std::string>(&raw)) {
        return renderJsonString(*string);
    }
    if (const Json::Array* array = std::get_if<Json::Array>(&raw)) {
        std::string result = "[";
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0) {
                result += ',';
            }
            result += renderJson(array->at(index));
        }
        result += ']';
        return result;
    }
    const Json::Object& object = std::get<Json::Object>(raw);
    std::string result = "{";
    std::size_t index = 0;
    for (const auto& item : object) {
        if (index++ != 0) {
            result += ',';
        }
        result += renderJsonString(item.first);
        result += ':';
        result += renderJson(item.second);
    }
    result += '}';
    return result;
}

const Json::Object& objectValue(const Json& value, const char* path) {
    const Json::Object* object = std::get_if<Json::Object>(&value.value());
    if (object == nullptr) {
        throw ProtocolError(
            "invalid_frame", std::string(path) + " must be an object");
    }
    return *object;
}

const Json& field(
        const Json::Object& object,
        const std::string& name,
        const char* path = "frame") {
    const auto item = object.find(name);
    if (item == object.end()) {
        throw ProtocolError(
            "invalid_fields",
            std::string(path) + " is missing field '" + name + "'");
    }
    return item->second;
}

void requireFields(
        const Json::Object& object,
        std::initializer_list<const char*> fields) {
    std::set<std::string> expected;
    for (const char* name : fields) {
        expected.emplace(name);
    }
    for (const auto& item : object) {
        if (expected.count(item.first) == 0) {
            throw ProtocolError(
                "invalid_fields", "frame has unknown field '" + item.first + "'");
        }
    }
    for (const std::string& name : expected) {
        static_cast<void>(field(object, name));
    }
}

std::string stringValue(
        const Json::Object& object,
        const std::string& name,
        bool nonblank = true) {
    const std::string* value =
        std::get_if<std::string>(&field(object, name).value());
    if (value == nullptr) {
        throw ProtocolError(
            "invalid_fields", "frame." + name + " must be a string");
    }
    if (nonblank
        && std::all_of(
            value->begin(), value->end(), [](unsigned char character) {
                return std::isspace(character) != 0;
            })) {
        throw ProtocolError(
            "invalid_fields", "frame." + name + " must be nonblank");
    }
    return *value;
}

std::uint64_t unsignedValue(
        const Json::Object& object,
        const std::string& name,
        std::uint64_t minimum = 0,
        std::uint64_t maximum =
            std::numeric_limits<std::uint64_t>::max()) {
    const std::uint64_t* value =
        std::get_if<std::uint64_t>(&field(object, name).value());
    if (value == nullptr || *value < minimum || *value > maximum) {
        throw ProtocolError(
            "invalid_fields",
            "frame." + name + " is outside its unsigned integer range");
    }
    return *value;
}

void requireSchema(const Json::Object& object) {
    if (stringValue(object, "schema") != kRnicFlowSessionSchema) {
        throw ProtocolError(
            "unsupported_schema", "frame.schema is unsupported");
    }
}

enum class ReadFrameStatus {
    Frame,
    CleanEof,
};

ReadFrameStatus readFrame(std::istream& input, Json& value) {
    std::array<unsigned char, 4> prefix{};
    input.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
    const std::streamsize prefix_bytes = input.gcount();
    if (prefix_bytes == 0 && input.eof()) {
        return ReadFrameStatus::CleanEof;
    }
    if (prefix_bytes != static_cast<std::streamsize>(prefix.size())) {
        throw ProtocolError(
            "truncated_frame", "EOF interrupted the frame length prefix");
    }
    const std::uint32_t size =
        (static_cast<std::uint32_t>(prefix[0]) << 24U)
        | (static_cast<std::uint32_t>(prefix[1]) << 16U)
        | (static_cast<std::uint32_t>(prefix[2]) << 8U)
        | static_cast<std::uint32_t>(prefix[3]);
    if (size > kRnicFlowSessionMaximumFrameBytes) {
        throw ProtocolError(
            "oversized_frame", "frame body exceeds the 1 MiB limit");
    }
    std::string body(size, '\0');
    input.read(body.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ProtocolError(
            "truncated_frame", "EOF interrupted the declared frame body");
    }
    value = JsonParser(body).parse();
    if (renderJson(value) != body) {
        throw ProtocolError(
            "noncanonical_json", "frame body is not canonical JSON");
    }
    static_cast<void>(objectValue(value, "frame"));
    return ReadFrameStatus::Frame;
}

void writeFrame(std::ostream& output, const Json& value) {
    const std::string body = renderJson(value);
    if (body.size() > kRnicFlowSessionMaximumFrameBytes) {
        throw std::logic_error("response frame exceeds the 1 MiB limit");
    }
    const std::uint32_t size = static_cast<std::uint32_t>(body.size());
    const std::array<char, 4> prefix{
        static_cast<char>((size >> 24U) & 0xffU),
        static_cast<char>((size >> 16U) & 0xffU),
        static_cast<char>((size >> 8U) & 0xffU),
        static_cast<char>(size & 0xffU),
    };
    output.write(prefix.data(), prefix.size());
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to write a session response frame");
    }
}

class SessionApi final : public AtlahsHtsimApi {
public:
    void EventFinished(const EventOver&) override {
        ++completion_notifications;
    }

    std::uint64_t completion_notifications{0};
};

class InjectionEvent final : public EventSource {
public:
    InjectionEvent(EventList& event_list, std::function<void()> callback)
        : EventSource(event_list, "RNIC flow-session injection"),
          callback_(std::move(callback)) {}

    ~InjectionEvent() override {
        if (handle_.has_value()) {
            EventList::cancelPendingSourceByHandle(*this, *handle_);
        }
    }

    void schedule(Picoseconds at_ps) {
        if (handle_.has_value()) {
            throw std::logic_error("flow-session injection scheduled twice");
        }
        handle_ = EventList::sourceIsPendingGetHandle(*this, at_ps);
        if (*handle_ == EventList::nullHandle()) {
            handle_.reset();
            throw std::logic_error("flow-session injection was not scheduled");
        }
    }

    void doNextEvent() override {
        handle_.reset();
        callback_();
    }

private:
    std::function<void()> callback_;
    std::optional<EventList::Handle> handle_;
};

struct Injection {
    std::uint64_t sequence{0};
    std::string execution_id;
    std::string operation_id;
    std::string flow_id;
    std::uint32_t source{0};
    std::uint32_t destination{0};
    std::uint32_t tag{0};
    std::uint64_t payload_bytes{0};
    Picoseconds eligible_at_ps{0};
    std::uint64_t policy_context_token{0};
    AtlahsFlowId native_flow_id{0};
    bool fired{false};
    unsigned int emitted_phases{0};
    std::unique_ptr<InjectionEvent> event;
};

Json authorityCountersJson(const AtlahsWqeAuthorityCounters& counters) {
    return Json(Json::Object{
        {"legacy_aborts", Json(counters.legacy_aborts)},
        {"legacy_ledger_constructed", Json(counters.legacy_ledger_constructed)},
        {"legacy_mutations", Json(counters.legacy_mutations)},
        {"legacy_posts", Json(counters.legacy_posts)},
        {"native_posts", Json(counters.native_posts)},
        {"native_session_constructed", Json(counters.native_session_constructed)},
    });
}

class FlowSession final {
public:
    FlowSession(
            std::string htsim_source_revision,
            std::string simllm_source_revision)
        : htsim_source_revision_(std::move(htsim_source_revision)),
          simllm_source_revision_(std::move(simllm_source_revision)) {
        EventList::setEndtime(
            std::numeric_limits<simtime_picosec>::max());
    }

    Json dispatch(const Json& frame) {
        const Json::Object& object = objectValue(frame, "frame");
        requireSchema(object);
        const std::string verb = stringValue(object, "verb");
        if (closed_) {
            throw ProtocolError(
                "post_terminal", "a complete frame followed close");
        }
        if (verb == "open") {
            return open(object);
        }
        if (!opened_) {
            throw ProtocolError(
                "open_required", "open must be the first frame");
        }
        if (verb == "inject") {
            return inject(object);
        }
        if (verb == "advance") {
            return advance(object);
        }
        if (verb == "drain") {
            return drain(object);
        }
        if (verb == "close") {
            return close(object);
        }
        throw ProtocolError(
            "unknown_verb", "frame.verb is unsupported");
    }

    bool closed() const noexcept { return closed_; }

    AtlahsWqeAuthorityCounters counters() const noexcept {
        return api_.activeWqeAuthority().has_value()
            ? api_.authorityCounters()
            : AtlahsWqeAuthorityCounters{};
    }

private:
    Json success(const char* verb, Json::Object fields = {}) const {
        fields.emplace("schema", Json(kRnicFlowSessionSchema));
        fields.emplace("status", Json("ok"));
        fields.emplace("verb", Json(verb));
        return Json(std::move(fields));
    }

    Json open(const Json::Object& object) {
        requireFields(
            object,
            {"effective_hardware_sha256", "link_rate_bps", "node_count",
             "profile", "schema", "seed", "session_id",
             "topology_identity", "verb", "wqe_authority"});
        if (opened_) {
            throw ProtocolError("duplicate_open", "session is already open");
        }
        const std::string session_id = stringValue(object, "session_id");
        const std::string profile_name = stringValue(object, "profile");
        const RnicProfile profile = parseRnicProfile(profile_name);
        if (profile != RnicProfile::PacketizedManifold
            && profile != RnicProfile::CollectiveNetwork) {
            throw ProtocolError(
                "unsupported_profile",
                "flow sessions require rnic-nn or rnic-cn");
        }
        const std::uint32_t node_count = static_cast<std::uint32_t>(
            unsignedValue(
                object, "node_count", 1,
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
        const std::uint64_t link_rate_bps =
            unsignedValue(object, "link_rate_bps", 1);
        const std::uint64_t seed = unsignedValue(object, "seed");
        const std::string topology_identity =
            stringValue(object, "topology_identity");
        const std::string expected_topology = profile_name + ":nodes="
            + std::to_string(node_count);
        if (topology_identity != expected_topology) {
            throw ProtocolError(
                "topology_mismatch",
                "topology identity disagrees with profile and node_count");
        }
        const std::string authority =
            stringValue(object, "wqe_authority");
        if (authority != "simllm-native-rnic-session") {
            throw ProtocolError(
                "authority_mismatch",
                "flow session requires the native RNIC WQE authority");
        }
        const std::string requested_hash =
            stringValue(object, "effective_hardware_sha256");
        if (requested_hash.size() != 64
            || !std::all_of(
                requested_hash.begin(), requested_hash.end(), [](char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                })) {
            throw ProtocolError(
                "invalid_fields",
                "effective_hardware_sha256 must be lowercase SHA-256");
        }

        RnicAtlahsCliOptions options{};
        options.goal_file = "flow-session";
        options.node_count = node_count;
        options.link_capacity_bps = link_rate_bps;
        options.profile = profile;
        options.collective.global_prbs_seed = seed;
        auto assembly = assembleRnicAtlahsProfile(
            event_list_, options, node_count);
        api_.setEventList(&event_list_);
        api_.setGoalRankMapping(
            AtlahsHtsimApi::GoalRankMapping::GpuRank);
        api_.linkspeed = link_rate_bps;
        api_.total_nodes = static_cast<int>(node_count);
        api_.setTopologyCfg(assembly->topologyConfig());
        api_.setTopology(assembly->physicalTopology());

        SimllmAtlahsRuntimeConfig config;
        config.session_id = session_id;
        config.transport_policy = profile_name;
        config.seed = seed;
        config.topology_identity = topology_identity;
        config.htsim_source_revision = htsim_source_revision_;
        config.simllm_source_revision = simllm_source_revision_;
        config.device = defaultSimllmAtlahsDeviceConfig();
        config.port.endpoint_count = node_count;
        config.port.link_rate_bps = link_rate_bps;
        config.port.traffic_class = 3;
        auto runtime = makeComposedSimllmAtlahsFlowRuntime(
            event_list_, std::move(config), std::move(assembly));
        native_ = runtime.get();
        api_.setFlowRuntime(std::move(runtime));
        api_.Setup();
        const std::string actual_hash =
            native_->runRecord().hardware_config_sha256;
        if (actual_hash != requested_hash) {
            throw ProtocolError(
                "hardware_mismatch",
                "effective hardware hash disagrees with the accepted runtime");
        }

        opened_ = true;
        session_id_ = session_id;
        profile_name_ = profile_name;
        topology_identity_ = topology_identity;
        node_count_ = node_count;
        link_rate_bps_ = link_rate_bps;
        seed_ = seed;
        hardware_hash_ = actual_hash;
        policy_context_token_ =
            native_->device(0).config().identity.policy_context_token;
        return success(
            "open",
            Json::Object{
                {"effective_hardware_sha256", Json(hardware_hash_)},
                {"link_rate_bps", Json(link_rate_bps_)},
                {"node_count", Json(static_cast<std::uint64_t>(node_count_))},
                {"profile", Json(profile_name_)},
                {"seed", Json(seed_)},
                {"sequence", Json(UINT64_C(0))},
                {"session_id", Json(session_id_)},
                {"topology_identity", Json(topology_identity_)},
                {"wqe_authority", Json("simllm-native-rnic-session")},
            });
    }

    Json inject(const Json::Object& object) {
        requireFields(
            object,
            {"destination", "eligible_at_ps", "execution_id", "flow_id",
             "operation_id", "payload_bytes", "policy_context_token",
             "schema", "sequence", "source", "tag", "verb"});
        if (drained_) {
            throw ProtocolError(
                "post_drain_inject", "inject is illegal after drain");
        }
        const std::uint64_t sequence = unsignedValue(
            object, "sequence", 1,
            std::numeric_limits<std::uint32_t>::max());
        if (sequence != last_sequence_ + 1) {
            throw ProtocolError(
                sequence <= last_sequence_
                    ? "duplicate_sequence"
                    : "skipped_sequence",
                "inject sequence is not the next contiguous value");
        }
        Injection injection;
        injection.sequence = sequence;
        injection.execution_id = stringValue(object, "execution_id");
        injection.operation_id = stringValue(object, "operation_id");
        injection.flow_id = stringValue(object, "flow_id");
        injection.source = static_cast<std::uint32_t>(unsignedValue(
            object, "source", 0, node_count_ - 1));
        injection.destination = static_cast<std::uint32_t>(unsignedValue(
            object, "destination", 0, node_count_ - 1));
        if (injection.source == injection.destination) {
            throw ProtocolError(
                "invalid_fields", "flow endpoints must be distinct");
        }
        injection.tag = static_cast<std::uint32_t>(unsignedValue(
            object, "tag", 0,
            std::numeric_limits<std::uint32_t>::max()));
        injection.payload_bytes =
            unsignedValue(object, "payload_bytes", 1);
        injection.eligible_at_ps =
            unsignedValue(object, "eligible_at_ps");
        if (last_horizon_.has_value()
            && injection.eligible_at_ps <= *last_horizon_) {
            throw ProtocolError(
                "stale_eligibility",
                "injection eligibility is inside an advanced interval");
        }
        injection.policy_context_token =
            unsignedValue(object, "policy_context_token", 1);
        if (injection.policy_context_token != policy_context_token_) {
            throw ProtocolError(
                "policy_context_mismatch",
                "flow policy context disagrees with effective hardware");
        }
        const auto identity = std::make_tuple(
            injection.execution_id,
            injection.operation_id,
            injection.flow_id);
        if (identities_.count(identity) != 0) {
            throw ProtocolError(
                "duplicate_identity", "flow identity tuple was reused");
        }
        injection.native_flow_id = makeAtlahsFlowId(
            injection.source,
            static_cast<std::uint32_t>(sequence - 1));

        injections_.push_back(std::move(injection));
        const std::size_t index = injections_.size() - 1;
        injections_.back().event = std::make_unique<InjectionEvent>(
            event_list_, [this, index]() { fireInjection(index); });
        injections_.back().event->schedule(
            injections_.back().eligible_at_ps);
        identities_.insert(identity);
        last_sequence_ = sequence;
        return success(
            "inject",
            Json::Object{
                {"accepted_sequence", Json(sequence)},
                {"eligible_at_ps", Json(injections_.back().eligible_at_ps)},
            });
    }

    void fireInjection(std::size_t index) {
        Injection& injection = injections_.at(index);
        if (injection.fired) {
            throw std::logic_error("flow-session injection fired twice");
        }
        if (EventList::now() != injection.eligible_at_ps) {
            throw std::logic_error(
                "flow-session injection fired at the wrong time");
        }
        graph_node_properties node{};
        node.host = injection.source;
        node.offset = static_cast<std::uint32_t>(injection.sequence - 1);
        node.target = injection.destination;
        node.size = injection.payload_bytes;
        node.tag = injection.tag;
        node.nic = 0;
        node.type = OP_SEND;
        api_.Send(
            SendEvent(
                static_cast<int>(injection.source),
                static_cast<int>(injection.destination),
                injection.payload_bytes,
                static_cast<int>(injection.tag),
                injection.eligible_at_ps),
            node);
        injection.fired = true;
    }

    const WqeRecord& wqe(const Injection& injection) const {
        const auto& records = native_->device(injection.source).records();
        const auto item = std::find_if(
            records.begin(), records.end(), [&](const WqeRecord& record) {
                return record.request.flow_id == injection.native_flow_id;
            });
        if (item == records.end()) {
            throw std::logic_error(
                "native RNIC record omitted a fired session flow");
        }
        return *item;
    }

    const AtlahsCompletedFlowRecord* completion(
            const Injection& injection) const {
        const auto& completed = api_.completedFlows();
        const auto item = std::find_if(
            completed.begin(), completed.end(),
            [&](const AtlahsCompletedFlowRecord& record) {
                return record.flow_id == injection.native_flow_id;
            });
        return item == completed.end() ? nullptr : &*item;
    }

    struct PendingProjection {
        Picoseconds timestamp_ps{0};
        std::uint64_t sequence{0};
        unsigned int phase{0};
        Json value;
    };

    Json lifecycleJson(
            const Injection& injection,
            const WqeRecord& record,
            const char* kind,
            Picoseconds timestamp_ps,
            const AtlahsCompletedFlowRecord* completed) const {
        const auto& config = native_->device(injection.source).config();
        const AtlahsTransportKind transport =
            native_->networkRuntime().transportKind();
        const std::uint64_t transport_object_id = completed == nullptr
            ? atlahsTransportObjectId(
                node_count_, transport, injection.source,
                injection.destination)
            : completed->transport_object_id;
        Json::Object result{
            {"cq_consume_sequence", Json(nullptr)},
            {"cq_id", Json(config.work_queue.cq_id)},
            {"cq_post_sequence", Json(nullptr)},
            {"destination", Json(static_cast<std::uint64_t>(injection.destination))},
            {"execution_id", Json(injection.execution_id)},
            {"flow_id", Json(injection.flow_id)},
            {"kind", Json(kind)},
            {"native_flow_id", Json(injection.native_flow_id)},
            {"operation_id", Json(injection.operation_id)},
            {"payload_bytes", Json(injection.payload_bytes)},
            {"policy_context_token", Json(injection.policy_context_token)},
            {"sequence", Json(injection.sequence)},
            {"source", Json(static_cast<std::uint64_t>(injection.source))},
            {"sq_id", Json(config.work_queue.sq_id)},
            {"sq_post_sequence", Json(record.sq_sequence)},
            {"tag", Json(static_cast<std::uint64_t>(injection.tag))},
            {"timestamp_ps", Json(timestamp_ps)},
            {"transport_kind", Json(atlahsTransportKindName(transport))},
            {"transport_object_id", Json(transport_object_id)},
            {"wqe_id", Json(record.wqe_id)},
        };
        if (completed != nullptr) {
            result["cq_consume_sequence"] =
                Json(completed->cq_consume_sequence);
            result["cq_post_sequence"] =
                Json(completed->cq_post_sequence);
        }
        return Json(std::move(result));
    }

    Json::Array collectLifecycleEvents() {
        std::vector<PendingProjection> pending;
        for (Injection& injection : injections_) {
            if (!injection.fired) {
                continue;
            }
            const WqeRecord& record = wqe(injection);
            const AtlahsCompletedFlowRecord* completed =
                completion(injection);
            struct Phase {
                unsigned int bit;
                const char* name;
                std::optional<Picoseconds> timestamp;
            };
            const std::array<Phase, 4> phases{
                Phase{1U, "accepted", record.timeline.posted_at_ps},
                Phase{2U, "queued", record.timeline.admitted_at_ps},
                Phase{4U, "started", record.timeline.network_accepted_at_ps},
                Phase{8U,
                      "completed",
                      completed == nullptr
                          ? std::optional<Picoseconds>{}
                          : std::optional<Picoseconds>{
                                completed->completion_time_ps}},
            };
            for (unsigned int index = 0; index < phases.size(); ++index) {
                const Phase& phase = phases[index];
                if ((injection.emitted_phases & phase.bit) != 0
                    || !phase.timestamp.has_value()) {
                    continue;
                }
                pending.push_back(PendingProjection{
                    *phase.timestamp,
                    injection.sequence,
                    index,
                    lifecycleJson(
                        injection,
                        record,
                        phase.name,
                        *phase.timestamp,
                        std::strcmp(phase.name, "completed") == 0
                            ? completed
                            : nullptr),
                });
                injection.emitted_phases |= phase.bit;
            }
        }
        std::sort(
            pending.begin(), pending.end(),
            [](const PendingProjection& left,
               const PendingProjection& right) {
                return std::tie(
                           left.timestamp_ps, left.sequence, left.phase)
                    < std::tie(
                           right.timestamp_ps, right.sequence, right.phase);
            });
        Json::Array result;
        result.reserve(pending.size());
        for (PendingProjection& projection : pending) {
            result.push_back(std::move(projection.value));
        }
        return result;
    }

    Json advance(const Json::Object& object) {
        requireFields(
            object,
            {"schema", "through_ps", "through_sequence", "verb"});
        if (drained_) {
            throw ProtocolError(
                "post_drain_advance", "advance is illegal after drain");
        }
        const std::uint64_t through_sequence =
            unsignedValue(object, "through_sequence");
        if (through_sequence != last_sequence_) {
            throw ProtocolError(
                "cursor_disagreement",
                "advance cursor disagrees with the accepted sequence");
        }
        const Picoseconds through_ps =
            unsignedValue(object, "through_ps");
        if (last_horizon_.has_value() && through_ps < *last_horizon_) {
            throw ProtocolError(
                "stale_horizon", "advance horizon moved backward");
        }
        last_horizon_ = through_ps;
        while (true) {
            const std::optional<simtime_picosec> next =
                EventList::nextEventTime();
            if (!next.has_value() || *next > through_ps) {
                break;
            }
            if (!EventList::doNextEvent()) {
                throw std::logic_error(
                    "event list advertised an event but did not run it");
            }
        }
        return success(
            "advance",
            Json::Object{
                {"authority_counters", authorityCountersJson(counters())},
                {"events", Json(collectLifecycleEvents())},
                {"last_accepted_sequence", Json(last_sequence_)},
                {"through_ps", Json(through_ps)},
            });
    }

    Json completionRowJson(
            const Injection& injection,
            const AtlahsCompletedFlowRecord& completed) const {
        return Json(Json::Object{
            {"completion_time_ps", Json(completed.completion_time_ps)},
            {"cq_consume_sequence", Json(completed.cq_consume_sequence)},
            {"cq_id", Json(completed.cq_id)},
            {"cq_post_sequence", Json(completed.cq_post_sequence)},
            {"destination", Json(static_cast<std::uint64_t>(completed.destination))},
            {"execution_id", Json(injection.execution_id)},
            {"fct_ps", Json(completed.fct_ps())},
            {"flow_id", Json(injection.flow_id)},
            {"native_flow_id", Json(completed.flow_id)},
            {"operation_id", Json(injection.operation_id)},
            {"payload_bytes", Json(completed.payload_bytes)},
            {"sequence", Json(injection.sequence)},
            {"source", Json(static_cast<std::uint64_t>(completed.source))},
            {"sq_dispatch_sequence", Json(completed.sq_dispatch_sequence)},
            {"sq_id", Json(completed.sq_id)},
            {"sq_post_sequence", Json(completed.sq_post_sequence)},
            {"start_time_ps", Json(completed.start_time_ps)},
            {"tag", Json(completed.tag)},
            {"transport_kind", Json(atlahsTransportKindName(completed.transport_kind))},
            {"transport_object_id", Json(completed.transport_object_id)},
            {"wqe_id", Json(completed.wqe_id)},
        });
    }

    Json drain(const Json::Object& object) {
        requireFields(
            object, {"schema", "through_sequence", "verb"});
        if (drained_) {
            throw ProtocolError("duplicate_drain", "session was already drained");
        }
        if (unsignedValue(object, "through_sequence") != last_sequence_) {
            throw ProtocolError(
                "cursor_disagreement",
                "drain cursor disagrees with the accepted sequence");
        }
        if (std::any_of(
                injections_.begin(), injections_.end(),
                [](const Injection& injection) { return !injection.fired; })) {
            throw ProtocolError(
                "not_drained", "an accepted injection has not reached eligibility");
        }
        if (api_.runtimeHasPendingPhysicalWork()) {
            throw ProtocolError(
                "not_drained", "the physical runtime still has pending work");
        }
        native_->validateQuiescent();
        api_.validateWqeQuiescent();
        if (api_.completedFlows().size() != injections_.size()
            || api_.completion_notifications != injections_.size()) {
            throw std::logic_error(
                "flow-session completion conservation failed");
        }

        Json::Array completion_rows;
        completion_rows.reserve(injections_.size());
        for (const Injection& injection : injections_) {
            const AtlahsCompletedFlowRecord* completed =
                completion(injection);
            if (completed == nullptr) {
                throw std::logic_error(
                    "flow-session drain omitted a completion row");
            }
            completion_rows.push_back(
                completionRowJson(injection, *completed));
        }
        Json::Array high_watermarks;
        high_watermarks.reserve(node_count_);
        for (std::uint32_t endpoint = 0;
             endpoint < node_count_;
             ++endpoint) {
            high_watermarks.emplace_back(static_cast<std::uint64_t>(
                native_->device(endpoint).counters().sq_high_watermark));
        }
        drained_ = true;
        drained_sequence_ = last_sequence_;
        return success(
            "drain",
            Json::Object{
                {"authority_counters", authorityCountersJson(counters())},
                {"completion_rows", Json(std::move(completion_rows))},
                {"events", Json(collectLifecycleEvents())},
                {"last_accepted_sequence", Json(last_sequence_)},
                {"quiescent", Json(true)},
                {"sq_high_watermarks", Json(std::move(high_watermarks))},
            });
    }

    Json close(const Json::Object& object) {
        requireFields(
            object, {"schema", "through_sequence", "verb"});
        if (!drained_) {
            throw ProtocolError(
                "drain_required", "close requires a successful drain");
        }
        if (unsignedValue(object, "through_sequence")
            != drained_sequence_) {
            throw ProtocolError(
                "cursor_disagreement",
                "close cursor disagrees with the drained sequence");
        }
        closed_ = true;
        return success(
            "close",
            Json::Object{
                {"last_accepted_sequence", Json(last_sequence_)},
                {"terminal", Json(true)},
            });
    }

    EventList event_list_;
    SessionApi api_;
    SimllmAtlahsFlowRuntime* native_{nullptr};
    std::string htsim_source_revision_;
    std::string simllm_source_revision_;
    bool opened_{false};
    bool drained_{false};
    bool closed_{false};
    std::string session_id_;
    std::string profile_name_;
    std::string topology_identity_;
    std::uint32_t node_count_{0};
    std::uint64_t link_rate_bps_{0};
    std::uint64_t seed_{0};
    std::string hardware_hash_;
    std::uint64_t policy_context_token_{0};
    std::uint64_t last_sequence_{0};
    std::uint64_t drained_sequence_{0};
    std::optional<Picoseconds> last_horizon_;
    std::set<std::tuple<std::string, std::string, std::string>> identities_;
    std::vector<Injection> injections_;
};

Json errorResponse(
        const std::string& code,
        const std::string& message,
        const AtlahsWqeAuthorityCounters& counters) {
    return Json(Json::Object{
        {"authority_counters", authorityCountersJson(counters)},
        {"error", Json(Json::Object{
            {"code", Json(code)},
            {"message", Json(message)},
        })},
        {"schema", Json(kRnicFlowSessionSchema)},
        {"status", Json("error")},
        {"terminal", Json(true)},
        {"verb", Json("error")},
    });
}

}  // namespace

int runRnicFlowSession(
        std::istream& input,
        std::ostream& output,
        std::ostream& error,
        const std::string& htsim_source_revision,
        const std::string& simllm_source_revision) {
    FlowSession session(htsim_source_revision, simllm_source_revision);
    while (true) {
        try {
            Json frame;
            if (readFrame(input, frame) == ReadFrameStatus::CleanEof) {
                if (session.closed()) {
                    return 0;
                }
                error << "htsim_rnic flow session: disconnect before close\n";
                return 2;
            }
            writeFrame(output, session.dispatch(frame));
        } catch (const ProtocolError& protocol_error) {
            if (protocol_error.code() == "truncated_frame") {
                error << "htsim_rnic flow session: "
                      << protocol_error.what() << '\n';
                return 2;
            }
            try {
                writeFrame(
                    output,
                    errorResponse(
                        protocol_error.code(),
                        protocol_error.what(),
                        session.counters()));
            } catch (const std::exception& response_error) {
                error << "htsim_rnic flow session: failed to report error: "
                      << response_error.what() << '\n';
            }
            return 2;
        } catch (const std::exception& runtime_error) {
            try {
                writeFrame(
                    output,
                    errorResponse(
                        "runtime_error",
                        runtime_error.what(),
                        session.counters()));
            } catch (const std::exception& response_error) {
                error << "htsim_rnic flow session: failed to report runtime error: "
                      << response_error.what() << '\n';
            }
            return 2;
        }
    }
}
