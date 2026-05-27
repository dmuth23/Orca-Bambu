#include "PJarczakLinuxSoBridgeLauncher.hpp"
#include "PJarczakLinuxBridgeConfig.hpp"

#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace Slic3r::PJarczakLinuxBridge {

namespace {

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&build_default_launch_spec), &info) || !info.dli_fname)
        return {};
    return std::filesystem::path(info.dli_fname).parent_path();
}

std::string trim_ascii(std::string s)
{
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ')
        s.erase(s.begin());
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ')
        s.pop_back();
    return s;
}

std::string read_text_file_trimmed(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string v((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // strip UTF-8 BOM if present
    if (v.size() >= 3 &&
        static_cast<unsigned char>(v[0]) == 0xEFu &&
        static_cast<unsigned char>(v[1]) == 0xBBu &&
        static_cast<unsigned char>(v[2]) == 0xBFu)
        v.erase(0, 3);
    return trim_ascii(v);
}

std::string run_and_capture(const std::string& cmd)
{
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        result += buf;
    pclose(pipe);
    return trim_ascii(result);
}

std::filesystem::path runtime_dir(const std::filesystem::path& plugin_dir)
{
    // Runtime directory lives next to the plugin directory
    return plugin_dir.parent_path() / "pjarczak_lima_runtime";
}

std::string configured_instance_name(const std::filesystem::path& plugin_dir)
{
    const char* env_val = std::getenv("PJARCZAK_LIMA_INSTANCE");
    if (env_val && *env_val) return trim_ascii(std::string(env_val));
    return read_text_file_trimmed(plugin_dir / mac_lima_instance_file_name());
}

std::string first_missing_runtime_file(const std::filesystem::path& plugin_dir)
{
    const std::filesystem::path rt_dir = runtime_dir(plugin_dir);

    const std::vector<std::string> plugin_files = {
        host_executable_file_name(),
        mac_lima_instance_file_name(),
    };
    for (const auto& name : plugin_files)
        if (!std::filesystem::exists(plugin_dir / name))
            return name;

    const std::vector<std::string> runtime_files = {
        mac_runtime_install_script_file_name(),
    };
    for (const auto& name : runtime_files)
        if (!std::filesystem::exists(rt_dir / name))
            return name;

    return {};
}

bool probe_runtime_ready(const std::filesystem::path& plugin_dir, const std::string& instance_name)
{
    const std::filesystem::path rt_dir = runtime_dir(plugin_dir);
    const std::filesystem::path verify_script = rt_dir / mac_runtime_verify_script_file_name();
    if (!std::filesystem::exists(verify_script))
        return false;

    const std::string cmd = std::string("bash ") + verify_script.string() + " " + instance_name + " 2>/dev/null";
    const std::string out = run_and_capture(cmd);
    return out.find("ok") != std::string::npos || out.find("ready") != std::string::npos;
}

} // namespace

std::string host_executable_name()
{
    return host_executable_file_name();
}

std::string host_pipe_hint()
{
    return "stdio";
}

std::string launch_preflight_error()
{
    const std::filesystem::path plugin_dir = module_dir();
    if (plugin_dir.empty())
        return "bridge launcher could not resolve plugin directory";

    const auto missing = first_missing_runtime_file(plugin_dir);
    if (!missing.empty())
        return "required macOS bridge runtime file missing: " + missing;

    const std::string instance_name = configured_instance_name(plugin_dir);
    if (instance_name.empty())
        return "Lima instance name not configured (missing pjarczak_lima_instance.txt)";

    if (!probe_runtime_ready(plugin_dir, instance_name))
        return "Lima runtime for instance '" + instance_name + "' is not ready. Run the install script.";

    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path plugin_dir = module_dir();
    if (plugin_dir.empty()) {
        LaunchSpec spec;
        spec.description = "mac bridge preflight error";
        spec.argv = {"/bin/sh", "-c", "echo 'bridge launcher could not resolve plugin directory' >&2; exit 127"};
        return spec;
    }

    const std::string instance_name = configured_instance_name(plugin_dir);
    const std::filesystem::path rt_dir = runtime_dir(plugin_dir);

    // Wrapper script that runs host executable inside Lima
    const std::filesystem::path wrapper = plugin_dir / mac_host_wrapper_file_name();

    LaunchSpec spec;
    spec.description = "macos via lima";
    spec.argv = {
        wrapper.string(),
        instance_name,
        (plugin_dir / host_executable_file_name()).string()
    };
    spec.env = {
        {"PJARCZAK_BAMBU_PLUGIN_DIR",   plugin_dir.string()},
        {"PJARCZAK_LIMA_INSTANCE",       instance_name},
        {"PJARCZAK_BAMBU_NETWORK_SO",   (plugin_dir / linux_network_library_name()).string()},
        {"PJARCZAK_BAMBU_SOURCE_SO",    (plugin_dir / linux_source_library_name()).string()},
        {"PJARCZAK_LIMA_RUNTIME_DIR",   rt_dir.string()},
    };
    return spec;
}

} // namespace Slic3r::PJarczakLinuxBridge
