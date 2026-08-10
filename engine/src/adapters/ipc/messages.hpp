#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace sotto::ipc {

using nlohmann::json;

// JSON-RPC 2.0 reserved codes, plus the engine range -32000..-32099.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;

inline constexpr int kProtocolVersion = 1;

// A string id echoes into every reply, so bounding it keeps replies encodable
inline constexpr std::size_t kMaxIdBytes = 128;

using Id = std::variant<std::int64_t, std::string>;

struct Request {
    Id id;
    std::string method;
    json params;
};

struct Error {
    int code = 0;
    std::string message;
    std::optional<json> data;
};

struct PeerInfo {
    std::string name;
    std::string version;
    int protocol_version = 0;
};

// Whisper output can carry invalid UTF-8; replace rather than throw
inline std::string Serialize(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

inline json IdToJson(const Id& id) {
    if (std::holds_alternative<std::int64_t>(id)) return std::get<std::int64_t>(id);
    return std::get<std::string>(id);
}

// Checks mirror envelope.schema.json, which is the source of truth. Takes the
// message by value so params can be moved out: nlohmann's copy recurses one
// stack frame per nesting level, so a deeply nested params would overflow.
inline std::variant<Request, Error> ParseRequest(json j) {
    const auto invalid = [](std::string why) {
        return Error{kInvalidRequest, "Invalid Request", json(std::move(why))};
    };
    if (!j.is_object()) return invalid("message is not an object");
    for (const auto& [key, value] : j.items()) {
        if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") {
            return invalid("unknown member: " + key);
        }
    }
    if (!j.contains("jsonrpc") || j["jsonrpc"] != "2.0") return invalid("jsonrpc must be \"2.0\"");
    if (!j.contains("method") || !j["method"].is_string()) return invalid("method missing");
    if (!j.contains("id")) return invalid("notification received where request expected");
    if (j.contains("params") && !j["params"].is_object())
        return invalid("params must be an object");

    Request req;
    const auto& id = j["id"];
    if (id.is_number_integer()) {
        if (id.is_number_unsigned() &&
            id.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return invalid("id out of range");
        }
        req.id = id.get<std::int64_t>();
    } else if (id.is_string()) {
        const auto& text = id.get_ref<const std::string&>();
        if (text.size() > kMaxIdBytes) return invalid("id string too long");
        req.id = text;
    } else {
        return invalid("id must be a string or integer");
    }
    req.method = j["method"].get<std::string>();
    req.params = j.contains("params") ? std::move(j["params"]) : json::object();
    return req;
}

inline json MakeResult(const Id& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", IdToJson(id)}, {"result", std::move(result)}};
}

inline json MakeError(const Id& id, const Error& e) {
    json err{{"code", e.code}, {"message", e.message}};
    if (e.data) err["data"] = *e.data;
    return json{{"jsonrpc", "2.0"}, {"id", IdToJson(id)}, {"error", std::move(err)}};
}

inline json MakeNotification(const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

inline json ToJson(const PeerInfo& p) {
    return json{{"name", p.name}, {"version", p.version}, {"protocolVersion", p.protocol_version}};
}

inline std::optional<PeerInfo> PeerInfoFromJson(const json& j) {
    if (!j.is_object() || j.size() != 3) return std::nullopt;
    if (!j.contains("name") || !j["name"].is_string()) return std::nullopt;
    if (!j.contains("version") || !j["version"].is_string()) return std::nullopt;
    // Compare on the json value: get<int>() would truncate an out-of-range number
    // into a match. There is only one supported version, so equality is the check.
    if (!j.contains("protocolVersion") || j["protocolVersion"] != kProtocolVersion) {
        return std::nullopt;
    }
    return PeerInfo{j["name"].get<std::string>(), j["version"].get<std::string>(),
                    kProtocolVersion};
}

}  // namespace sotto::ipc
