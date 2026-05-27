#include "PJarczakBambuNetworkForwarderState.hpp"
#include "PJarczakLinuxSoBridgeRpcClient.hpp"

#include <boost/log/trivial.hpp>

#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Slic3r::PJarczakLinuxBridge {

namespace {

std::mutex g_agents_mutex;
std::unordered_map<std::int64_t, BridgeAgent*> g_agents;

std::mutex g_tunnels_mutex;
std::unordered_map<std::int64_t, BridgeTunnel*> g_tunnels;

template<typename Fn>
void run_or_queue(BridgeAgent* agent, Fn&& fn)
{
    if (agent && agent->queue_on_main) {
        agent->queue_on_main(std::move(fn));
    } else {
        fn();
    }
}

#if defined(_WIN32)
static std::wstring utf8_to_wstring(const std::string& s)
{
    if (s.empty()) return {};
    const int sz = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (sz <= 0) return {};
    std::wstring out(static_cast<size_t>(sz - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), sz);
    return out;
}
#endif

} // namespace

// ============================================================================
// Agent lifecycle
// ============================================================================

BridgeAgent* as_agent(void* handle)
{
    return reinterpret_cast<BridgeAgent*>(handle);
}

void* new_agent(const std::string& log_dir)
{
    auto* agent = new BridgeAgent();
    agent->log_dir = log_dir;
    return agent;
}

int delete_agent(void* handle)
{
    auto* agent = as_agent(handle);
    if (!agent) return -1;
    unregister_remote_agent(agent);
    delete agent;
    return 0;
}

BridgeTunnel* as_tunnel_handle(void* handle)
{
    return reinterpret_cast<BridgeTunnel*>(handle);
}

// ============================================================================
// Remote agent registry
// ============================================================================

void register_remote_agent(BridgeAgent* agent)
{
    if (!agent || agent->remote_handle == 0) return;
    std::lock_guard<std::mutex> lk(g_agents_mutex);
    g_agents[agent->remote_handle] = agent;
}

void unregister_remote_agent(BridgeAgent* agent)
{
    if (!agent) return;
    std::lock_guard<std::mutex> lk(g_agents_mutex);
    g_agents.erase(agent->remote_handle);
}

BridgeAgent* find_remote_agent(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> lk(g_agents_mutex);
    auto it = g_agents.find(remote_handle);
    return it != g_agents.end() ? it->second : nullptr;
}

// ============================================================================
// Remote tunnel registry
// ============================================================================

void register_remote_tunnel(BridgeTunnel* tunnel)
{
    if (!tunnel || tunnel->remote_handle == 0) return;
    std::lock_guard<std::mutex> lk(g_tunnels_mutex);
    g_tunnels[tunnel->remote_handle] = tunnel;
}

void unregister_remote_tunnel(BridgeTunnel* tunnel)
{
    if (!tunnel) return;
    std::lock_guard<std::mutex> lk(g_tunnels_mutex);
    g_tunnels.erase(tunnel->remote_handle);
}

BridgeTunnel* find_remote_tunnel(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> lk(g_tunnels_mutex);
    auto it = g_tunnels.find(remote_handle);
    return it != g_tunnels.end() ? it->second : nullptr;
}

// ============================================================================
// Job state management
// ============================================================================

std::shared_ptr<BridgeJobState> register_job_state(BridgeAgent* agent, const std::shared_ptr<BridgeJobState>& job)
{
    if (!agent || !job) return nullptr;
    std::lock_guard<std::mutex> lk(agent->jobs_mutex);
    agent->jobs[job->job_id] = job;
    return job;
}

std::shared_ptr<BridgeJobState> find_job_state(BridgeAgent* agent, std::int64_t job_id)
{
    if (!agent) return nullptr;
    std::lock_guard<std::mutex> lk(agent->jobs_mutex);
    auto it = agent->jobs.find(job_id);
    return it != agent->jobs.end() ? it->second : nullptr;
}

void unregister_job_state(BridgeAgent* agent, std::int64_t job_id)
{
    if (!agent) return;
    std::shared_ptr<BridgeJobState> job;
    {
        std::lock_guard<std::mutex> lk(agent->jobs_mutex);
        auto it = agent->jobs.find(job_id);
        if (it == agent->jobs.end()) return;
        job = it->second;
        agent->jobs.erase(it);
    }
    if (job) {
        job->stop_cancel_watch = true;
        if (job->cancel_watch.joinable())
            job->cancel_watch.join();
    }
}

// ============================================================================
// Agent event dispatch
// ============================================================================

void dispatch_agent_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    BridgeAgent* agent = find_remote_agent(remote_handle);
    if (!agent) return;

    if (name == "on_ssdp_msg") {
        const auto msg = payload.value("msg", std::string());
        if (agent->on_ssdp_msg)
            run_or_queue(agent, [agent, msg]{ agent->on_ssdp_msg(msg); });

    } else if (name == "on_user_login") {
        const int ret  = payload.value("ret", 0);
        const auto info = payload.value("info", std::string());
        if (agent->on_user_login) {
            if (agent->queue_on_main)
                agent->queue_on_main([agent, ret, info]{ agent->on_user_login(ret, info); });
            else
                agent->on_user_login(ret, info);
        }

    } else if (name == "on_printer_connected") {
        const auto topic = payload.value("topic", std::string());
        if (agent->on_printer_connected)
            run_or_queue(agent, [agent, topic]{ agent->on_printer_connected(topic); });

    } else if (name == "on_server_connected") {
        const int ret_code    = payload.value("ret_code", 0);
        const int reason_code = payload.value("reason_code", 0);
        agent->server_connected = (ret_code == 0);
        if (agent->on_server_connected)
            run_or_queue(agent, [agent, ret_code, reason_code]{ agent->on_server_connected(ret_code, reason_code); });

    } else if (name == "on_http_error") {
        const unsigned http_code = payload.value("http_code", 0u);
        const auto http_body      = payload.value("http_body", std::string());
        if (agent->on_http_error)
            run_or_queue(agent, [agent, http_code, http_body]{ agent->on_http_error(http_code, http_body); });

    } else if (name == "on_subscribe_failure") {
        const auto topic = payload.value("topic", std::string());
        if (agent->on_subscribe_failure)
            run_or_queue(agent, [agent, topic]{ agent->on_subscribe_failure(topic); });

    } else if (name == "on_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg    = payload.value("msg",    std::string());
        if (agent->on_message)
            run_or_queue(agent, [agent, dev_id, msg]{ agent->on_message(dev_id, msg); });

    } else if (name == "on_user_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg    = payload.value("msg",    std::string());
        if (agent->on_user_message)
            run_or_queue(agent, [agent, dev_id, msg]{ agent->on_user_message(dev_id, msg); });

    } else if (name == "on_local_connect") {
        const int  status  = payload.value("status", 0);
        const auto dev_id  = payload.value("dev_id", std::string());
        const auto msg     = payload.value("msg",    std::string());
        if (agent->on_local_connect)
            run_or_queue(agent, [agent, status, dev_id, msg]{ agent->on_local_connect(status, dev_id, msg); });

    } else if (name == "on_local_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg    = payload.value("msg",    std::string());
        if (agent->on_local_message)
            run_or_queue(agent, [agent, dev_id, msg]{ agent->on_local_message(dev_id, msg); });

    } else if (name == "on_server_error") {
        const auto url    = payload.value("url",    std::string());
        const int  status = payload.value("status", 0);
        if (agent->on_server_error)
            run_or_queue(agent, [agent, url, status]{ agent->on_server_error(url, status); });

    } else if (name == "job.update_status") {
        const auto job_id = payload.value("job_id", std::int64_t(0));
        const int  status = payload.value("status", 0);
        const int  code   = payload.value("code",   0);
        const auto msg    = payload.value("msg",    std::string());
        auto job = find_job_state(agent, job_id);
        if (job && job->on_update_status)
            job->on_update_status(status, code, msg);

    } else if (name == "job.wait") {
        const auto job_id   = payload.value("job_id", std::int64_t(0));
        const int  status   = payload.value("status", 0);
        const auto job_info = payload.value("job_info", std::string());
        auto job = find_job_state(agent, job_id);
        if (job && job->on_wait) {
            const bool ok = job->on_wait(status, job_info);
            RpcClient::instance().invoke_void("job.wait_reply", {
                {"job_id", job_id}, {"ok", ok}
            });
        }

    } else if (name == "job.done") {
        const auto job_id = payload.value("job_id", std::int64_t(0));
        const int  result = payload.value("result", 0);
        auto job = find_job_state(agent, job_id);
        if (job && job->on_update_status)
            job->on_update_status(7 /* PrintingStageFinished */, result, "done");
        unregister_job_state(agent, job_id);

    } else if (name == "get_country_code_request") {
        std::string cc;
        if (agent->get_country_code) cc = agent->get_country_code();
        RpcClient::instance().invoke_void("agent.country_code_reply", {
            {"agent", remote_handle}, {"code", cc}
        });

    } else {
        BOOST_LOG_TRIVIAL(debug) << "dispatch_agent_event: unhandled event '" << name << "' for agent " << remote_handle;
    }
}

// ============================================================================
// Tunnel event dispatch
// ============================================================================

void dispatch_tunnel_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    BridgeTunnel* tunnel = find_remote_tunnel(remote_handle);
    if (!tunnel) return;

    if (name == "log") {
        const auto msg = payload.value("msg", std::string());
        const int level = payload.value("level", 0);
        if (tunnel->logger) {
#if defined(_WIN32)
            tunnel->logger_message_wide = utf8_to_wstring(msg);
            tunnel->logger(tunnel->logger_ctx, level, tunnel->logger_message_wide.c_str());
#else
            tunnel->logger_message_utf8 = msg;
            tunnel->logger(tunnel->logger_ctx, level, tunnel->logger_message_utf8.c_str());
#endif
        }
    } else if (name == "sample") {
        CachedSample sample;
        sample.itrack      = payload.value("itrack", 0);
        sample.size        = payload.value("size", 0);
        sample.flags       = payload.value("flags", 0);
        sample.decode_time = payload.value("decode_time", 0ULL);
        // binary data arrives via binary_data frame (handled by RpcClient)
        // For now store in queue
        tunnel->sample_queue.push_back(std::move(sample));
    } else if (name == "recv_message") {
        const auto data = payload.value("data", std::string());
        tunnel->recv_message_buffer += data;
    } else {
        BOOST_LOG_TRIVIAL(debug) << "dispatch_tunnel_event: unhandled event '" << name << "'";
    }
}

} // namespace Slic3r::PJarczakLinuxBridge
