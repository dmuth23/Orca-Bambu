#include "PJarczakLinuxSoBridgeRpcProtocol.hpp"

#include <cstring>
#include <istream>
#include <ostream>

namespace Slic3r::PJarczakLinuxBridge {

namespace {

void write_u32_le(std::ostream& out, uint32_t v)
{
    uint8_t b[4] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24)
    };
    out.write(reinterpret_cast<const char*>(b), 4);
}

bool read_u32_le(std::istream& in, uint32_t& v)
{
    uint8_t b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4))
        return false;
    v = static_cast<uint32_t>(b[0])
      | (static_cast<uint32_t>(b[1]) << 8)
      | (static_cast<uint32_t>(b[2]) << 16)
      | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

} // namespace

// Wire format: [magic:4][type:1][pad:3][id:4][size:4][data:size]
bool write_raw_frame(std::ostream& out, const RawRpcFrame& frame, std::string& error)
{
    write_u32_le(out, RPC_FRAME_MAGIC);
    uint8_t type_byte = static_cast<uint8_t>(frame.type);
    out.write(reinterpret_cast<const char*>(&type_byte), 1);
    uint8_t pad[3] = {0, 0, 0};
    out.write(reinterpret_cast<const char*>(pad), 3);
    write_u32_le(out, static_cast<uint32_t>(frame.id));
    write_u32_le(out, static_cast<uint32_t>(frame.data.size()));
    if (!frame.data.empty())
        out.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
    if (!out) {
        error = "write_raw_frame: stream write error";
        return false;
    }
    out.flush();
    return true;
}

bool read_raw_frame(std::istream& in, RawRpcFrame& frame, std::string& error)
{
    uint32_t magic = 0;
    if (!read_u32_le(in, magic)) {
        error = "read_raw_frame: failed to read magic";
        return false;
    }
    if (magic != RPC_FRAME_MAGIC) {
        error = "read_raw_frame: invalid magic 0x" + std::to_string(magic);
        return false;
    }
    uint8_t type_byte = 0;
    if (!in.read(reinterpret_cast<char*>(&type_byte), 1)) {
        error = "read_raw_frame: failed to read type";
        return false;
    }
    frame.type = static_cast<RpcFrameType>(type_byte);
    uint8_t pad[3];
    in.read(reinterpret_cast<char*>(pad), 3);

    uint32_t id_u = 0;
    if (!read_u32_le(in, id_u)) {
        error = "read_raw_frame: failed to read id";
        return false;
    }
    frame.id = static_cast<int>(id_u);

    uint32_t size = 0;
    if (!read_u32_le(in, size)) {
        error = "read_raw_frame: failed to read size";
        return false;
    }
    frame.data.resize(size);
    if (size > 0 && !in.read(reinterpret_cast<char*>(frame.data.data()), static_cast<std::streamsize>(size))) {
        error = "read_raw_frame: failed to read data";
        return false;
    }
    return true;
}

bool write_json_frame(std::ostream& out, RpcFrameType type, int id, const nlohmann::json& payload, std::string& error)
{
    const std::string json_str = payload.dump();
    RawRpcFrame frame;
    frame.type = type;
    frame.id   = id;
    frame.data.assign(json_str.begin(), json_str.end());
    return write_raw_frame(out, frame, error);
}

bool read_json_frame(std::istream& in, RpcFrameType& type, int& id, nlohmann::json& payload, std::string& error)
{
    RawRpcFrame frame;
    if (!read_raw_frame(in, frame, error))
        return false;
    type = frame.type;
    id   = frame.id;
    try {
        payload = nlohmann::json::parse(frame.data.begin(), frame.data.end());
    } catch (const std::exception& e) {
        error = std::string("read_json_frame: JSON parse error: ") + e.what();
        return false;
    }
    return true;
}

bool write_request_frame(std::ostream& out, int id, const std::string& method, const nlohmann::json& payload, std::string& error)
{
    const nlohmann::json j = {{"method", method}, {"payload", payload}};
    return write_json_frame(out, RpcFrameType::json_request, id, j, error);
}

bool read_request_frame(std::istream& in, int& id, std::string& method, nlohmann::json& payload, std::string& error)
{
    RpcFrameType type;
    nlohmann::json j;
    if (!read_json_frame(in, type, id, j, error))
        return false;
    if (type != RpcFrameType::json_request) {
        error = "read_request_frame: unexpected frame type";
        return false;
    }
    method  = j.value("method", std::string());
    payload = j.value("payload", nlohmann::json::object());
    return true;
}

} // namespace Slic3r::PJarczakLinuxBridge
