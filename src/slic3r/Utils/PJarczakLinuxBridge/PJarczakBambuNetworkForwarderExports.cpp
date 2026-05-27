// Bridge library exports: implements bambu_network_* and Bambu_* as RPC forwarders.

#include "PJarczakBambuNetworkForwarderState.hpp"
#include "PJarczakLinuxSoBridgeRpcClient.hpp"
#include "PJarczakLinuxSoBridgeEventPump.hpp"
#include "PJarczakLinuxBridgeCompat.hpp"

#include "../bambu_networking.hpp"
#include "../../GUI/Printer/BambuTunnel.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32) && defined(BAMBU_EXPORTS)
#define EXPORT_API __declspec(dllexport)
#else
#define EXPORT_API __attribute__((visibility("default")))
#endif

// ============================================================================
// Helpers
// ============================================================================

namespace {

using namespace Slic3r::PJarczakLinuxBridge;

static std::string g_last_error;

static std::atomic<std::int64_t> g_next_job_id{1};

std::int64_t next_job_id()
{
    return g_next_job_id.fetch_add(1, std::memory_order_relaxed);
}

static bool ok_or_error(const nlohmann::json& j)
{
    if (!j.value("ok", true)) {
        g_last_error = j.value("error", std::string("unknown error"));
        return false;
    }
    return true;
}

static nlohmann::json print_params_to_json(const Slic3r::PrintParams& p)
{
    return {
        {"dev_id",              p.dev_id},
        {"task_name",           p.task_name},
        {"project_name",        p.project_name},
        {"preset_name",         p.preset_name},
        {"filename",            p.filename},
        {"config_filename",     p.config_filename},
        {"plate_index",         p.plate_index},
        {"ftp_folder",          p.ftp_folder},
        {"ftp_file",            p.ftp_file},
        {"ftp_file_md5",        p.ftp_file_md5},
        {"nozzle_mapping",      p.nozzle_mapping},
        {"ams_mapping",         p.ams_mapping},
        {"ams_mapping2",        p.ams_mapping2},
        {"ams_mapping_info",    p.ams_mapping_info},
        {"nozzles_info",        p.nozzles_info},
        {"connection_type",     p.connection_type},
        {"comments",            p.comments},
        {"origin_profile_id",   p.origin_profile_id},
        {"stl_design_id",       p.stl_design_id},
        {"origin_model_id",     p.origin_model_id},
        {"print_type",          p.print_type},
        {"dst_file",            p.dst_file},
        {"dev_name",            p.dev_name},
        {"dev_ip",              p.dev_ip},
        {"use_ssl_for_ftp",     p.use_ssl_for_ftp},
        {"use_ssl_for_mqtt",    p.use_ssl_for_mqtt},
        {"username",            p.username},
        {"password",            p.password},
        {"task_bed_leveling",   p.task_bed_leveling},
        {"task_flow_cali",      p.task_flow_cali},
        {"task_vibration_cali", p.task_vibration_cali},
        {"task_layer_inspect",  p.task_layer_inspect},
        {"task_record_timelapse",p.task_record_timelapse},
        {"task_use_ams",        p.task_use_ams},
        {"task_bed_type",       p.task_bed_type},
        {"extra_options",       p.extra_options},
        {"auto_bed_leveling",   p.auto_bed_leveling},
        {"auto_flow_cali",      p.auto_flow_cali},
        {"auto_offset_cali",    p.auto_offset_cali},
        {"task_ext_change_assist", p.task_ext_change_assist},
        {"try_emmc_print",      p.try_emmc_print}
    };
}

static nlohmann::json publish_params_to_json(const Slic3r::PublishParams& p)
{
    return {
        {"project_name",      p.project_name},
        {"project_3mf_file",  p.project_3mf_file},
        {"preset_name",       p.preset_name},
        {"project_model_id",  p.project_model_id},
        {"design_id",         p.design_id},
        {"config_filename",   p.config_filename}
    };
}

static nlohmann::json task_query_params_to_json(const Slic3r::TaskQueryParams& p)
{
    return {{"dev_id", p.dev_id}, {"status", p.status}, {"offset", p.offset}, {"limit", p.limit}};
}

static nlohmann::json str_map_to_json(const std::map<std::string, std::string>& m)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : m) j[kv.first] = kv.second;
    return j;
}

static nlohmann::json str_vec_to_json(const std::vector<std::string>& v)
{
    nlohmann::json j = nlohmann::json::array();
    for (const auto& s : v) j.push_back(s);
    return j;
}

// Start a print-class job (all four variants share this logic)
static int start_job(void* agent_handle,
                     const std::string& method,
                     const nlohmann::json& params_json,
                     Slic3r::OnUpdateStatusFn update_fn,
                     Slic3r::WasCancelledFn   cancel_fn,
                     Slic3r::OnWaitFn         wait_fn)
{
    auto* a = as_agent(agent_handle);
    if (!a) return -1;

    auto& rpc = RpcClient::instance();
    rpc.ensure_started();
    EventPump::instance().ensure_started();

    const std::int64_t job_id = next_job_id();

    auto job = std::make_shared<BridgeJobState>();
    job->job_id          = job_id;
    job->kind            = method;
    job->on_update_status = update_fn;
    job->was_cancelled   = cancel_fn;
    job->on_wait         = wait_fn;
    register_job_state(a, job);

    // Start cancel watcher thread
    if (cancel_fn) {
        job->cancel_watch = std::thread([job_id, cancel_fn, &rpc, &stop = job->stop_cancel_watch]() {
            using namespace std::chrono_literals;
            while (!stop.load()) {
                std::this_thread::sleep_for(120ms);
                if (cancel_fn()) {
                    rpc.invoke_void("job.cancel", {{"job_id", job_id}});
                    break;
                }
            }
        });
    }

    const auto j = rpc.invoke_json(method, {
        {"agent", a->remote_handle},
        {"job_id", job_id},
        {"params", params_json}
    });
    return j.value("result", -1);
}

} // anonymous namespace

// ============================================================================
// Exported C API
// ============================================================================

extern "C" {

// --------------------------------------------------------------------------
// Version / Consistency
// --------------------------------------------------------------------------

EXPORT_API bool bambu_network_check_debug_consistent(bool /*is_debug*/)
{
    return true;
}

EXPORT_API std::string bambu_network_get_version()
{
    return RpcClient::instance().invoke_string("bridge.get_version");
}

// --------------------------------------------------------------------------
// Agent lifecycle
// --------------------------------------------------------------------------

EXPORT_API void* bambu_network_create_agent(std::string log_dir)
{
    auto& rpc = RpcClient::instance();
    rpc.ensure_started();
    EventPump::instance().ensure_started();

    void* handle = new_agent(log_dir);
    auto* a = as_agent(handle);

    const auto j = rpc.invoke_json("agent.create", {{"log_dir", log_dir}});
    a->remote_handle = j.value("handle", std::int64_t(0));

    if (a->remote_handle != 0)
        register_remote_agent(a);

    return handle;
}

EXPORT_API int bambu_network_destroy_agent(void* agent)
{
    auto* a = as_agent(agent);
    if (!a) return -1;
    if (a->remote_handle)
        RpcClient::instance().invoke_void("agent.destroy", {{"agent", a->remote_handle}});
    return delete_agent(agent);
}

// --------------------------------------------------------------------------
// Agent initialization
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_init_log(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.init_log", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_set_config_dir(void* agent, std::string config_dir)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->config_dir = config_dir;
    return RpcClient::instance().invoke_int("agent.set_config_dir", {{"agent", a->remote_handle}, {"config_dir", config_dir}});
}

EXPORT_API int bambu_network_set_cert_file(void* agent, std::string folder, std::string filename)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->cert_dir  = folder;
    a->cert_file = filename;
    return RpcClient::instance().invoke_int("agent.set_cert_file", {{"agent", a->remote_handle}, {"folder", folder}, {"filename", filename}});
}

EXPORT_API int bambu_network_set_country_code(void* agent, std::string country_code)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->country_code = country_code;
    return RpcClient::instance().invoke_int("agent.set_country_code", {{"agent", a->remote_handle}, {"country_code", country_code}});
}

EXPORT_API int bambu_network_start(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->started = true;
    return RpcClient::instance().invoke_int("agent.start", {{"agent", a->remote_handle}});
}

// --------------------------------------------------------------------------
// Callback registration
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_set_on_ssdp_msg_fn(void* agent, Slic3r::OnMsgArrivedFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_ssdp_msg = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_printer_connected_fn(void* agent, Slic3r::OnPrinterConnectedFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_printer_connected = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_server_connected_fn(void* agent, Slic3r::OnServerConnectedFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_server_connected = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_http_error_fn(void* agent, Slic3r::OnHttpErrorFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_http_error = fn;
    return 0;
}

EXPORT_API int bambu_network_set_get_country_code_fn(void* agent, Slic3r::GetCountryCodeFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->get_country_code = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_subscribe_failure_fn(void* agent, Slic3r::GetSubscribeFailureFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_subscribe_failure = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_message_fn(void* agent, Slic3r::OnMessageFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_message = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_user_message_fn(void* agent, Slic3r::OnMessageFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_user_message = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_local_connect_fn(void* agent, Slic3r::OnLocalConnectedFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_local_connect = fn;
    return 0;
}

EXPORT_API int bambu_network_set_on_local_message_fn(void* agent, Slic3r::OnMessageFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_local_message = fn;
    return 0;
}

EXPORT_API int bambu_network_set_queue_on_main_fn(void* agent, Slic3r::QueueOnMainFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->queue_on_main = fn;
    return 0;
}

EXPORT_API int bambu_network_set_server_callback(void* agent, Slic3r::OnServerErrFn fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->on_server_error = fn;
    return 0;
}

// --------------------------------------------------------------------------
// Server connectivity
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_connect_server(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.connect_server", {{"agent", a->remote_handle}});
}

EXPORT_API bool bambu_network_is_server_connected(void* agent)
{
    auto* a = as_agent(agent); if (!a) return false;
    return RpcClient::instance().invoke_bool("agent.is_server_connected", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_refresh_connection(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.refresh_connection", {{"agent", a->remote_handle}});
}

// --------------------------------------------------------------------------
// Device subscriptions
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_start_subscribe(void* agent, std::string module)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.start_subscribe", {{"agent", a->remote_handle}, {"module", module}});
}

EXPORT_API int bambu_network_stop_subscribe(void* agent, std::string module)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.stop_subscribe", {{"agent", a->remote_handle}, {"module", module}});
}

EXPORT_API int bambu_network_add_subscribe(void* agent, std::vector<std::string> dev_list)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.add_subscribe", {{"agent", a->remote_handle}, {"dev_list", str_vec_to_json(dev_list)}});
}

EXPORT_API int bambu_network_del_subscribe(void* agent, std::vector<std::string> dev_list)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.del_subscribe", {{"agent", a->remote_handle}, {"dev_list", str_vec_to_json(dev_list)}});
}

EXPORT_API void bambu_network_enable_multi_machine(void* agent, bool enable)
{
    auto* a = as_agent(agent); if (!a) return;
    a->multi_machine_enabled = enable;
    RpcClient::instance().invoke_void("agent.enable_multi_machine", {{"agent", a->remote_handle}, {"enable", enable}});
}

// --------------------------------------------------------------------------
// Messaging / printer connection
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_send_message(void* agent, std::string dev_id, std::string json_str, int qos, int flag)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.send_message",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}, {"json_str", json_str}, {"qos", qos}, {"flag", flag}});
}

EXPORT_API int bambu_network_connect_printer(void* agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.connect_printer",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}, {"dev_ip", dev_ip},
         {"username", username}, {"password", password}, {"use_ssl", use_ssl}});
}

EXPORT_API int bambu_network_disconnect_printer(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.disconnect_printer", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_send_message_to_printer(void* agent, std::string dev_id, std::string json_str, int qos, int flag)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.send_message_to_printer",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}, {"json_str", json_str}, {"qos", qos}, {"flag", flag}});
}

EXPORT_API int bambu_network_update_cert(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.update_cert", {{"agent", a->remote_handle}});
}

EXPORT_API void bambu_network_install_device_cert(void* agent, std::string dev_id, bool lan_only)
{
    auto* a = as_agent(agent); if (!a) return;
    RpcClient::instance().invoke_void("agent.install_device_cert",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}, {"lan_only", lan_only}});
}

EXPORT_API bool bambu_network_start_discovery(void* agent, bool start, bool sending)
{
    auto* a = as_agent(agent); if (!a) return false;
    return RpcClient::instance().invoke_bool("agent.start_discovery",
        {{"agent", a->remote_handle}, {"start", start}, {"sending", sending}});
}

// --------------------------------------------------------------------------
// User management
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_change_user(void* agent, std::string user_info)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->user_info = user_info;
    return RpcClient::instance().invoke_int("agent.change_user",
        {{"agent", a->remote_handle}, {"user_info", user_info}});
}

EXPORT_API bool bambu_network_is_user_login(void* agent)
{
    auto* a = as_agent(agent); if (!a) return false;
    return RpcClient::instance().invoke_bool("agent.is_user_login", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_user_logout(void* agent, bool request)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.user_logout",
        {{"agent", a->remote_handle}, {"request", request}});
}

EXPORT_API std::string bambu_network_get_user_id(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_user_id", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_get_user_name(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_user_name", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_get_user_avatar(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_user_avatar", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_get_user_nickanme(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_user_nickname", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_build_login_cmd(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.build_login_cmd", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_build_logout_cmd(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.build_logout_cmd", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_build_login_info(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.build_login_info", {{"agent", a->remote_handle}});
}

// --------------------------------------------------------------------------
// Device binding
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_ping_bind(void* agent, std::string ping_code)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.ping_bind",
        {{"agent", a->remote_handle}, {"ping_code", ping_code}});
}

EXPORT_API int bambu_network_bind_detect(void* agent, std::string dev_ip, std::string sec_link, Slic3r::detectResult& detect)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.bind_detect",
        {{"agent", a->remote_handle}, {"dev_ip", dev_ip}, {"sec_link", sec_link}});
    const auto& r = j["result"];
    detect.result_msg   = r.value("result_msg",   std::string());
    detect.command      = r.value("command",      std::string());
    detect.dev_id       = r.value("dev_id",       std::string());
    detect.model_id     = r.value("model_id",     std::string());
    detect.dev_name     = r.value("dev_name",     std::string());
    detect.version      = r.value("version",      std::string());
    detect.bind_state   = r.value("bind_state",   std::string());
    detect.connect_type = r.value("connect_type", std::string());
    return j.value("ret", -1);
}

EXPORT_API int bambu_network_bind(void* agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, Slic3r::OnUpdateStatusFn update_fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const std::int64_t job_id = next_job_id();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = job_id;
    job->kind   = "bind";
    job->on_update_status = update_fn;
    register_job_state(a, job);
    return RpcClient::instance().invoke_int("agent.bind", {
        {"agent", a->remote_handle}, {"dev_ip", dev_ip}, {"dev_id", dev_id},
        {"sec_link", sec_link}, {"timezone", timezone}, {"improved", improved},
        {"job_id", job_id}
    });
}

EXPORT_API int bambu_network_unbind(void* agent, std::string dev_id)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.unbind",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}});
}

EXPORT_API std::string bambu_network_get_bambulab_host(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_bambulab_host", {{"agent", a->remote_handle}});
}

EXPORT_API std::string bambu_network_get_user_selected_machine(void* agent)
{
    auto* a = as_agent(agent); if (!a) return {};
    return RpcClient::instance().invoke_string("agent.get_user_selected_machine", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_set_user_selected_machine(void* agent, std::string dev_id)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.set_user_selected_machine",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}});
}

// --------------------------------------------------------------------------
// Print jobs
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_start_print(void* agent, Slic3r::PrintParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn, Slic3r::OnWaitFn wait_fn)
{
    return start_job(agent, "agent.start_print", print_params_to_json(params), update_fn, cancel_fn, wait_fn);
}

EXPORT_API int bambu_network_start_local_print_with_record(void* agent, Slic3r::PrintParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn, Slic3r::OnWaitFn wait_fn)
{
    return start_job(agent, "agent.start_local_print_with_record", print_params_to_json(params), update_fn, cancel_fn, wait_fn);
}

EXPORT_API int bambu_network_start_send_gcode_to_sdcard(void* agent, Slic3r::PrintParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn, Slic3r::OnWaitFn wait_fn)
{
    return start_job(agent, "agent.start_send_gcode_to_sdcard", print_params_to_json(params), update_fn, cancel_fn, wait_fn);
}

EXPORT_API int bambu_network_start_local_print(void* agent, Slic3r::PrintParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn)
{
    return start_job(agent, "agent.start_local_print", print_params_to_json(params), update_fn, cancel_fn, nullptr);
}

EXPORT_API int bambu_network_start_sdcard_print(void* agent, Slic3r::PrintParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn)
{
    return start_job(agent, "agent.start_sdcard_print", print_params_to_json(params), update_fn, cancel_fn, nullptr);
}

// --------------------------------------------------------------------------
// Settings / presets
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_get_user_presets(void* agent, std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_user_presets", {{"agent", a->remote_handle}});
    if (user_presets && j.contains("presets") && j["presets"].is_object()) {
        for (auto& [key, vals] : j["presets"].items()) {
            auto& inner = (*user_presets)[key];
            for (auto& [k, v] : vals.items())
                inner[k] = v.get<std::string>();
        }
    }
    return j.value("result", -1);
}

EXPORT_API std::string bambu_network_request_setting_id(void* agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    auto* a = as_agent(agent); if (!a) return {};
    const auto j = RpcClient::instance().invoke_json("agent.request_setting_id",
        {{"agent", a->remote_handle}, {"name", name}, {"values", values_map ? str_map_to_json(*values_map) : nlohmann::json::object()}});
    if (http_code) *http_code = j.value("http_code", 0u);
    return j.value("result", std::string());
}

EXPORT_API int bambu_network_put_setting(void* agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.put_setting",
        {{"agent", a->remote_handle}, {"setting_id", setting_id}, {"name", name},
         {"values", values_map ? str_map_to_json(*values_map) : nlohmann::json::object()}});
    if (http_code) *http_code = j.value("http_code", 0u);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_setting_list(void* agent, std::string bundle_version, Slic3r::ProgressFn pro_fn, Slic3r::WasCancelledFn cancel_fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_setting_list",
        {{"agent", a->remote_handle}, {"bundle_version", bundle_version}});
    if (pro_fn) pro_fn(100);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_setting_list2(void* agent, std::string bundle_version, Slic3r::CheckFn chk_fn, Slic3r::ProgressFn pro_fn, Slic3r::WasCancelledFn cancel_fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_setting_list2",
        {{"agent", a->remote_handle}, {"bundle_version", bundle_version}});
    if (pro_fn) pro_fn(100);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_delete_setting(void* agent, std::string setting_id)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.delete_setting",
        {{"agent", a->remote_handle}, {"setting_id", setting_id}});
}

EXPORT_API int bambu_network_set_extra_http_header(void* agent, std::map<std::string, std::string> extra_headers)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.set_extra_http_header",
        {{"agent", a->remote_handle}, {"headers", str_map_to_json(extra_headers)}});
}

// --------------------------------------------------------------------------
// Messages / tasks
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_get_my_message(void* agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_my_message",
        {{"agent", a->remote_handle}, {"type", type}, {"after", after}, {"limit", limit}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_check_user_task_report(void* agent, int* task_id, bool* printable)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.check_user_task_report", {{"agent", a->remote_handle}});
    if (task_id)  *task_id  = j.value("task_id",  0);
    if (printable) *printable = j.value("printable", false);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_user_print_info(void* agent, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_user_print_info", {{"agent", a->remote_handle}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_user_tasks(void* agent, Slic3r::TaskQueryParams params, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_user_tasks",
        {{"agent", a->remote_handle}, {"params", task_query_params_to_json(params)}});
    if (http_body) *http_body = j.value("http_body", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_printer_firmware(void* agent, std::string dev_id, unsigned* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_printer_firmware",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_task_plate_index(void* agent, std::string task_id, int* plate_index)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_task_plate_index",
        {{"agent", a->remote_handle}, {"task_id", task_id}});
    if (plate_index) *plate_index = j.value("plate_index", 0);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_user_info(void* agent, int* identifier)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_user_info", {{"agent", a->remote_handle}});
    if (identifier) *identifier = j.value("identifier", 0);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_request_bind_ticket(void* agent, std::string* ticket)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.request_bind_ticket", {{"agent", a->remote_handle}});
    if (ticket) *ticket = j.value("ticket", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_subtask_info(void* agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_subtask_info",
        {{"agent", a->remote_handle}, {"subtask_id", subtask_id}});
    if (task_json) *task_json = j.value("task_json", std::string());
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_slice_info(void* agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_slice_info",
        {{"agent", a->remote_handle}, {"project_id", project_id}, {"profile_id", profile_id}, {"plate_index", plate_index}});
    if (slice_json) *slice_json = j.value("slice_json", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_query_bind_status(void* agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.query_bind_status",
        {{"agent", a->remote_handle}, {"query_list", str_vec_to_json(query_list)}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_modify_printer_name(void* agent, std::string dev_id, std::string dev_name)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.modify_printer_name",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}, {"dev_name", dev_name}});
}

EXPORT_API int bambu_network_get_camera_url(void* agent, std::string dev_id, std::function<void(std::string)> callback)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_camera_url",
        {{"agent", a->remote_handle}, {"dev_id", dev_id}});
    const auto url = j.value("url", std::string());
    if (callback) callback(url);
    return j.value("result", -1);
}

// --------------------------------------------------------------------------
// Model / marketplace
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_get_design_staffpick(void* agent, int offset, int limit, std::function<void(std::string)> callback)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_design_staffpick",
        {{"agent", a->remote_handle}, {"offset", offset}, {"limit", limit}});
    const auto body = j.value("body", std::string());
    if (callback) callback(body);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_start_publish(void* agent, Slic3r::PublishParams params, Slic3r::OnUpdateStatusFn update_fn, Slic3r::WasCancelledFn cancel_fn, std::string* out)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const std::int64_t job_id = next_job_id();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = job_id;
    job->kind   = "publish";
    job->on_update_status = update_fn;
    job->was_cancelled    = cancel_fn;
    job->out_string       = out;
    register_job_state(a, job);
    const auto j = RpcClient::instance().invoke_json("agent.start_publish",
        {{"agent", a->remote_handle}, {"job_id", job_id}, {"params", publish_params_to_json(params)}});
    if (out) *out = j.value("out", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_model_publish_url(void* agent, std::string* url)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_model_publish_url", {{"agent", a->remote_handle}});
    if (url) *url = j.value("url", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_subtask(void* agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_subtask",
        {{"agent", a->remote_handle}, {"task", Slic3r::PJarczakLinuxBridge::model_task_to_json(task)}});
    if (task)
        Slic3r::PJarczakLinuxBridge::json_to_model_task(j.value("task", nlohmann::json::object()), task);
    if (getsub_fn && task)
        getsub_fn(task);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_model_mall_home_url(void* agent, std::string* url)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_model_mall_home_url", {{"agent", a->remote_handle}});
    if (url) *url = j.value("url", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_model_mall_detail_url(void* agent, std::string* url, std::string id)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_model_mall_detail_url",
        {{"agent", a->remote_handle}, {"id", id}});
    if (url) *url = j.value("url", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_my_profile(void* agent, std::string token, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_my_profile",
        {{"agent", a->remote_handle}, {"token", token}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_my_token(void* agent, std::string ticket, unsigned int* http_code, std::string* http_body)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_my_token",
        {{"agent", a->remote_handle}, {"ticket", ticket}});
    if (http_code) *http_code = j.value("http_code", 0u);
    if (http_body)  *http_body  = j.value("http_body",  std::string());
    return j.value("result", -1);
}

// --------------------------------------------------------------------------
// Analytics / tracking
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_track_enable(void* agent, bool enable)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->tracking_enabled = enable;
    return RpcClient::instance().invoke_int("agent.track_enable",
        {{"agent", a->remote_handle}, {"enable", enable}});
}

EXPORT_API int bambu_network_track_remove_files(void* agent)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.track_remove_files", {{"agent", a->remote_handle}});
}

EXPORT_API int bambu_network_track_event(void* agent, std::string evt_key, std::string content)
{
    auto* a = as_agent(agent); if (!a) return -1;
    return RpcClient::instance().invoke_int("agent.track_event",
        {{"agent", a->remote_handle}, {"evt_key", evt_key}, {"content", content}});
}

EXPORT_API int bambu_network_track_header(void* agent, std::string header)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->track_header = header;
    return RpcClient::instance().invoke_int("agent.track_header",
        {{"agent", a->remote_handle}, {"header", header}});
}

EXPORT_API int bambu_network_track_update_property(void* agent, std::string name, std::string value, std::string type)
{
    auto* a = as_agent(agent); if (!a) return -1;
    a->track_properties[name] = value;
    return RpcClient::instance().invoke_int("agent.track_update_property",
        {{"agent", a->remote_handle}, {"name", name}, {"value", value}, {"type", type}});
}

EXPORT_API int bambu_network_track_get_property(void* agent, std::string name, std::string& value, std::string type)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.track_get_property",
        {{"agent", a->remote_handle}, {"name", name}, {"type", type}});
    value = j.value("value", std::string());
    return j.value("result", -1);
}

// --------------------------------------------------------------------------
// Ratings
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_put_model_mall_rating(void* agent, int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.put_model_mall_rating",
        {{"agent", a->remote_handle}, {"design_id", design_id}, {"score", score},
         {"content", content}, {"images", str_vec_to_json(images)}});
    http_code  = j.value("http_code",  0u);
    http_error = j.value("http_error", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_oss_config(void* agent, std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_oss_config",
        {{"agent", a->remote_handle}, {"country_code", country_code}});
    config     = j.value("config",     std::string());
    http_code  = j.value("http_code",  0u);
    http_error = j.value("http_error", std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_put_rating_picture_oss(void* agent, std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.put_rating_picture_oss",
        {{"agent", a->remote_handle}, {"config", config}, {"model_id", model_id}, {"profile_id", profile_id}});
    pic_oss_path = j.value("pic_oss_path", std::string());
    http_code    = j.value("http_code",    0u);
    http_error   = j.value("http_error",   std::string());
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_model_mall_rating(void* agent, int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_model_mall_rating_result",
        {{"agent", a->remote_handle}, {"job_id", job_id}});
    rating_result = j.value("rating_result", std::string());
    http_code     = j.value("http_code",     0u);
    http_error    = j.value("http_error",    std::string());
    return j.value("result", -1);
}

// --------------------------------------------------------------------------
// Makerwiki / 4U
// --------------------------------------------------------------------------

EXPORT_API int bambu_network_get_mw_user_preference(void* agent, std::function<void(std::string)> callback)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_mw_user_preference", {{"agent", a->remote_handle}});
    const auto body = j.value("body", std::string());
    if (callback) callback(body);
    return j.value("result", -1);
}

EXPORT_API int bambu_network_get_mw_user_4ulist(void* agent, int seed, int limit, std::function<void(std::string)> callback)
{
    auto* a = as_agent(agent); if (!a) return -1;
    const auto j = RpcClient::instance().invoke_json("agent.get_mw_user_4ulist",
        {{"agent", a->remote_handle}, {"seed", seed}, {"limit", limit}});
    const auto body = j.value("body", std::string());
    if (callback) callback(body);
    return j.value("result", -1);
}

// ============================================================================
// Bambu Tunnel API (video streaming)
// ============================================================================

EXPORT_API int Bambu_Init()
{
    return 0;
}

EXPORT_API void Bambu_Deinit()
{
}

EXPORT_API char const* Bambu_GetLastErrorMsg()
{
    return g_last_error.c_str();
}

EXPORT_API void Bambu_FreeLogMsg(tchar const* /*msg*/)
{
}

EXPORT_API int Bambu_Create(Bambu_Tunnel* tunnel, char const* path)
{
    if (!tunnel || !path) return -1;
    auto* t = new BridgeTunnel();
    const auto j = RpcClient::instance().invoke_json("tunnel.create", {{"path", std::string(path)}});
    t->remote_handle = j.value("handle", std::int64_t(0));
    if (t->remote_handle)
        register_remote_tunnel(t);
    *tunnel = reinterpret_cast<Bambu_Tunnel>(t);
    return j.value("result", -1);
}

EXPORT_API void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void* context)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return;
    t->logger     = logger;
    t->logger_ctx = context;
}

EXPORT_API int Bambu_Open(Bambu_Tunnel tunnel)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    t->opened = true;
    return RpcClient::instance().invoke_int("tunnel.open", {{"tunnel", t->remote_handle}});
}

EXPORT_API int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    return RpcClient::instance().invoke_int("tunnel.start_stream",
        {{"tunnel", t->remote_handle}, {"video", video}});
}

EXPORT_API int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    return RpcClient::instance().invoke_int("tunnel.start_stream_ex",
        {{"tunnel", t->remote_handle}, {"type", type}});
}

EXPORT_API int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return 0;
    return RpcClient::instance().invoke_int("tunnel.get_stream_count", {{"tunnel", t->remote_handle}});
}

EXPORT_API int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t || !info) return -1;
    const auto j = RpcClient::instance().invoke_json("tunnel.get_stream_info",
        {{"tunnel", t->remote_handle}, {"index", index}});
    info->type     = static_cast<Bambu_StreamType>(j.value("type", 0));
    info->sub_type = j.value("sub_type", 0);
    if (info->type == VIDE) {
        info->format.video.width      = j.value("width", 0);
        info->format.video.height     = j.value("height", 0);
        info->format.video.frame_rate = j.value("frame_rate", 0);
    } else {
        info->format.audio.sample_rate   = j.value("sample_rate", 0);
        info->format.audio.channel_count = j.value("channel_count", 0);
        info->format.audio.sample_size   = j.value("sample_size", 0);
    }
    info->format_type     = j.value("format_type", 0);
    info->format_size     = 0;
    info->max_frame_size  = j.value("max_frame_size", 0);
    info->format_buffer   = nullptr;
    // format_buffer data comes via binary_data frame and is stored in t->stream_format_buffers
    const int fi = j.value("format_buffer_index", -1);
    if (fi >= 0 && fi < static_cast<int>(t->stream_format_buffers.size())) {
        info->format_size   = static_cast<int>(t->stream_format_buffers[fi].size());
        info->format_buffer = t->stream_format_buffers[fi].data();
    }
    return j.value("result", -1);
}

EXPORT_API unsigned long Bambu_GetDuration(Bambu_Tunnel tunnel)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return 0;
    return static_cast<unsigned long>(RpcClient::instance().invoke_int("tunnel.get_duration", {{"tunnel", t->remote_handle}}));
}

EXPORT_API int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long time)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    return RpcClient::instance().invoke_int("tunnel.seek",
        {{"tunnel", t->remote_handle}, {"time", static_cast<int64_t>(time)}});
}

EXPORT_API int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t || !sample) return Bambu_would_block;
    if (t->sample_queue.empty())
        return Bambu_would_block;
    CachedSample& cs = t->sample_queue.front();
    sample->itrack      = cs.itrack;
    sample->size        = cs.size;
    sample->flags       = cs.flags;
    sample->buffer      = cs.buffer.data();
    sample->decode_time = cs.decode_time;
    return Bambu_success;
}

EXPORT_API int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const* data, int len)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    return RpcClient::instance().invoke_int("tunnel.send_message",
        {{"tunnel", t->remote_handle}, {"ctrl", ctrl},
         {"data", data ? std::string(data, static_cast<size_t>(len)) : std::string()}});
}

EXPORT_API int Bambu_RecvMessage(Bambu_Tunnel tunnel, int* ctrl, char* data, int* len)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return -1;
    if (t->recv_message_buffer.empty()) return Bambu_would_block;
    if (len) {
        const int avail = static_cast<int>(t->recv_message_buffer.size());
        const int copy  = std::min(avail, *len);
        std::memcpy(data, t->recv_message_buffer.data(), static_cast<size_t>(copy));
        t->recv_message_buffer.erase(0, static_cast<size_t>(copy));
        *len = copy;
    }
    if (ctrl) *ctrl = 0;
    return Bambu_success;
}

EXPORT_API void Bambu_Close(Bambu_Tunnel tunnel)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return;
    t->opened = false;
    RpcClient::instance().invoke_void("tunnel.close", {{"tunnel", t->remote_handle}});
}

EXPORT_API void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    auto* t = as_tunnel_handle(reinterpret_cast<void*>(tunnel));
    if (!t) return;
    if (t->remote_handle)
        RpcClient::instance().invoke_void("tunnel.destroy", {{"tunnel", t->remote_handle}});
    unregister_remote_tunnel(t);
    delete t;
}

} // extern "C"
