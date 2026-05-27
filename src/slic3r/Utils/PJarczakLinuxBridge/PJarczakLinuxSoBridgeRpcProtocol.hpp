#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r::PJarczakLinuxBridge {

// Magic 4-byte header identifier for all RPC frames
static constexpr uint32_t RPC_FRAME_MAGIC = 0x52424a50u; // 'PJBR'

enum class RpcFrameType : uint8_t {
    json_request  = 0,
    json_response = 1,
    binary_data   = 2,
    log           = 3,
};

struct RpcFrame {
    int            id{0};
    std::string    method;
    nlohmann::json payload;
};

struct RawRpcFrame {
    RpcFrameType          type{RpcFrameType::json_request};
    int                   id{0};
    std::vector<uint8_t>  data;
};

// Low-level frame I/O
bool write_raw_frame(std::ostream& out, const RawRpcFrame& frame, std::string& error);
bool read_raw_frame(std::istream& in,   RawRpcFrame& frame,       std::string& error);

// JSON frame helpers
bool write_json_frame(std::ostream& out, RpcFrameType type, int id, const nlohmann::json& payload, std::string& error);
bool read_json_frame(std::istream& in,  RpcFrameType& type, int& id, nlohmann::json& payload,       std::string& error);

// Request helpers (add "method" and "payload" keys)
bool write_request_frame(std::ostream& out, int id, const std::string& method, const nlohmann::json& payload, std::string& error);
bool read_request_frame(std::istream& in,  int& id, std::string& method,       nlohmann::json& payload,       std::string& error);

} // namespace Slic3r::PJarczakLinuxBridge
