#include "PJarczakLinuxSoBridgeRpcClient.hpp"
#include "PJarczakLinuxSoBridgeLauncher.hpp"
#include "PJarczakLinuxSoBridgeRpcProtocol.hpp"

#include <boost/log/trivial.hpp>
#include <boost/process/args.hpp>
#include <boost/process/env.hpp>
#include <boost/process/io.hpp>

#include <cassert>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Slic3r::PJarczakLinuxBridge {

RpcClient& RpcClient::instance()
{
    static RpcClient client;
    return client;
}

RpcClient::~RpcClient()
{
    stop();
}

bool RpcClient::is_started() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_proc && m_proc->child.running() && m_handshake_ok;
}

bool RpcClient::ensure_started()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (m_proc && m_proc->child.running() && m_handshake_ok)
        return true;
    return start_locked();
}

bool RpcClient::start_locked()
{
    if (m_proc) {
        // Already started (maybe not yet ok) — don't double-start
        return false;
    }

    const auto preflight = launch_preflight_error();
    if (!preflight.empty()) {
        m_last_error = "Bridge launch preflight failed: " + preflight;
        BOOST_LOG_TRIVIAL(error) << "RpcClient::start_locked: " << m_last_error;
        return false;
    }

    const LaunchSpec spec = build_default_launch_spec();
    if (spec.argv.empty()) {
        m_last_error = "Bridge launch spec has empty argv";
        BOOST_LOG_TRIVIAL(error) << "RpcClient::start_locked: " << m_last_error;
        return false;
    }

    try {
        auto proc = std::make_unique<Proc>();

        boost::process::environment env = boost::this_process::environment();
        for (const auto& kv : spec.env)
            env[kv.first] = kv.second;

        const std::string exe = spec.argv[0];
        std::vector<std::string> args(spec.argv.begin() + 1, spec.argv.end());

#if defined(_WIN32)
        proc->child = boost::process::child(
            exe, boost::process::args(args),
            boost::process::std_in < proc->in,
            boost::process::std_out > proc->out,
            boost::process::std_err > boost::process::null,
            env,
            boost::process::windows::hide
        );
#else
        proc->child = boost::process::child(
            exe, boost::process::args(args),
            boost::process::std_in < proc->in,
            boost::process::std_out > proc->out,
            boost::process::std_err > boost::process::null,
            env
        );
#endif

        m_proc = std::move(proc);
        m_reader_stop = false;
        m_reader = std::thread([this] { reader_loop(); });

    } catch (const std::exception& e) {
        m_last_error = std::string("Failed to launch bridge host: ") + e.what();
        BOOST_LOG_TRIVIAL(error) << "RpcClient::start_locked: " << m_last_error;
        m_proc.reset();
        return false;
    }

    return ensure_handshake();
}

bool RpcClient::ensure_handshake()
{
    if (m_handshake_ok)
        return true;

    // Perform handshake without acquiring m_state_mutex (already held by caller)
    try {
        RpcBinaryReply reply = request_impl("bridge.handshake", {{"version", 1}}, {}, true);
        if (reply.payload.value("ok", false)) {
            m_handshake_ok = true;
            BOOST_LOG_TRIVIAL(info) << "RpcClient: bridge handshake ok, host version="
                << reply.payload.value("host_version", std::string("unknown"));
            return true;
        }
        m_last_error = "Bridge handshake rejected: " + reply.payload.dump();
    } catch (const std::exception& e) {
        m_last_error = std::string("Bridge handshake exception: ") + e.what();
    }
    BOOST_LOG_TRIVIAL(error) << "RpcClient::ensure_handshake: " << m_last_error;
    return false;
}

void RpcClient::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_handshake_ok = false;
        m_reader_stop  = true;

        // Wake all pending requests with an error
        for (auto& kv : m_pending) {
            std::lock_guard<std::mutex> plk(kv.second->mutex);
            kv.second->ready  = true;
            kv.second->payload = {{"ok", false}, {"error", "bridge stopped"}};
            kv.second->cv.notify_all();
        }
        m_pending.clear();

        if (m_proc) {
            try { m_proc->in.pipe().close(); } catch (...) {}
            try { m_proc->child.terminate(); } catch (...) {}
            m_proc.reset();
        }
    }
    if (m_reader.joinable())
        m_reader.join();
}

void RpcClient::reader_loop()
{
    while (!m_reader_stop.load()) {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        if (!m_proc || !m_proc->child.running()) {
            lock.unlock();
            break;
        }
        auto& out_stream = m_proc->out;
        lock.unlock();

        RawRpcFrame frame;
        std::string err;
        if (!read_raw_frame(out_stream, frame, err)) {
            if (!m_reader_stop.load())
                BOOST_LOG_TRIVIAL(warning) << "RpcClient reader: read error: " << err;
            break;
        }

        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lk(m_state_mutex);
            auto it = m_pending.find(frame.id);
            if (it == m_pending.end())
                continue;
            pending = it->second;
        }

        std::lock_guard<std::mutex> plk(pending->mutex);
        if (frame.type == RpcFrameType::binary_data) {
            pending->binary.assign(frame.data.begin(), frame.data.end());
            pending->binary_received = true;
        } else {
            try {
                pending->payload = nlohmann::json::parse(frame.data.begin(), frame.data.end());
            } catch (...) {
                pending->payload = {{"ok", false}, {"error", "json parse error"}};
            }
            pending->json_received = true;
        }

        const bool done = pending->json_received &&
                          (!pending->expects_binary || pending->binary_received);
        if (done) {
            pending->ready = true;
            pending->cv.notify_all();
            std::lock_guard<std::mutex> lk(m_state_mutex);
            m_pending.erase(frame.id);
        }
    }
}

RpcBinaryReply RpcClient::request_impl(const std::string& method, const nlohmann::json& payload,
                                        const std::vector<unsigned char>& request_binary,
                                        bool skip_handshake)
{
    if (!skip_handshake && !is_started()) {
        return {nlohmann::json({{"ok", false}, {"error", "bridge not started"}}), {}};
    }

    int id;
    std::shared_ptr<Pending> pending;

    {
        std::lock_guard<std::mutex> lk(m_state_mutex);
        id = m_next_id++;
        pending = std::make_shared<Pending>();
        pending->expects_binary = !request_binary.empty();
        m_pending[id] = pending;
    }

    {
        std::lock_guard<std::mutex> wlk(m_write_mutex);
        std::lock_guard<std::mutex> lk(m_state_mutex);
        if (!m_proc) {
            std::lock_guard<std::mutex> plk(pending->mutex);
            pending->payload = {{"ok", false}, {"error", "no bridge process"}};
            pending->ready = true;
            m_pending.erase(id);
            return {pending->payload, {}};
        }

        std::string err;
        if (!write_request_frame(m_proc->in, id, method, payload, err)) {
            pending->payload = {{"ok", false}, {"error", err}};
            pending->ready = true;
            m_pending.erase(id);
            return {pending->payload, {}};
        }
        if (!request_binary.empty()) {
            RawRpcFrame bin_frame;
            bin_frame.type = RpcFrameType::binary_data;
            bin_frame.id   = id;
            bin_frame.data.assign(request_binary.begin(), request_binary.end());
            if (!write_raw_frame(m_proc->in, bin_frame, err)) {
                pending->payload = {{"ok", false}, {"error", err}};
                pending->ready = true;
                m_pending.erase(id);
                return {pending->payload, {}};
            }
        }
    }

    std::unique_lock<std::mutex> plk(pending->mutex);
    pending->cv.wait(plk, [&] { return pending->ready; });
    return {pending->payload, pending->binary};
}

int RpcClient::invoke_int(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload.value("result", -1);
}

bool RpcClient::invoke_bool(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload.value("result", false);
}

std::string RpcClient::invoke_string(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload.value("result", std::string());
}

nlohmann::json RpcClient::invoke_json(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload;
}

RpcBinaryReply RpcClient::invoke_binary(const std::string& method, const nlohmann::json& payload,
                                         const std::vector<unsigned char>& request_binary)
{
    return request_impl(method, payload, request_binary, false);
}

void RpcClient::invoke_void(const std::string& method, const nlohmann::json& payload)
{
    request_impl(method, payload, {}, false);
}

std::string RpcClient::last_error() const
{
    std::lock_guard<std::mutex> lk(m_state_mutex);
    return m_last_error;
}

} // namespace Slic3r::PJarczakLinuxBridge
