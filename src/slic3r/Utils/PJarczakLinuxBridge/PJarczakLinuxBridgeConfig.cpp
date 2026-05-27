#include "PJarczakLinuxBridgeConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if !defined(_MSC_VER) && !defined(_WIN32)
#include <elf.h>
#include <sys/utsname.h>
#endif

#include <openssl/evp.h>

namespace Slic3r::PJarczakLinuxBridge {

// ============================================================================
// Configuration
// ============================================================================

static bool env_flag(const char* name, bool default_value)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return default_value;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return default_value;
}

bool enabled()
{
#if defined(_MSC_VER) || defined(_WIN32)
    return env_flag("PJARCZAK_BRIDGE_ENABLED", true);
#elif defined(__WXMAC__) || defined(__APPLE__)
    return env_flag("PJARCZAK_BRIDGE_ENABLED", true);
#else
    // Linux: bridge is optional — only enabled if explicitly requested
    return env_flag("PJARCZAK_BRIDGE_ENABLED", false);
#endif
}

bool use_bridge_network_module()
{
    return enabled();
}

bool source_module_is_network_module()
{
    // The bridge library provides both the network and source module functions
    return env_flag("PJARCZAK_SOURCE_IS_NETWORK", false);
}

std::string forced_download_os_type()
{
    const char* v = std::getenv("PJARCZAK_FORCED_OS_TYPE");
    return (v && *v) ? std::string(v) : std::string();
}

std::string forced_client_version()
{
    const char* v = std::getenv("PJARCZAK_FORCED_CLIENT_VERSION");
    return (v && *v) ? std::string(v) : std::string();
}

// ============================================================================
// File / Library Names
// ============================================================================

std::string host_executable_file_name()
{
    // The bootstrap script picks the right abi variant; expose the abi1 name
    // as the canonical name used for preflight checks.
    return "pjarczak_bambu_linux_host_abi1";
}

std::string linux_network_library_name()
{
    return "libBambuNetwork.so";
}

std::string linux_source_library_name()
{
    return "libBambuSource.so";
}

std::string bridge_network_library_name()
{
#if defined(_MSC_VER) || defined(_WIN32)
    return "pjarczak_bambu_networking_bridge.dll";
#elif defined(__WXMAC__) || defined(__APPLE__)
    return "libpjarczak_bambu_networking_bridge.dylib";
#else
    return "libpjarczak_bambu_networking_bridge.so";
#endif
}

std::string bridge_network_library_path(const boost::filesystem::path& plugin_dir)
{
    return (plugin_dir / bridge_network_library_name()).string();
}

std::string bridge_network_library_path(const std::filesystem::path& plugin_dir)
{
    return (plugin_dir / bridge_network_library_name()).string();
}

// ============================================================================
// Platform File Names
// ============================================================================

std::string mac_lima_instance_file_name()      { return "pjarczak_lima_instance.txt"; }
std::string mac_runtime_install_script_file_name() { return "install_runtime.sh"; }
std::string mac_runtime_verify_script_file_name()  { return "verify_runtime.sh"; }
std::string mac_host_wrapper_file_name()       { return "pjarczak-lima-run-host.sh"; }

std::string windows_wsl_distro_file_name()            { return "pjarczak_wsl_distro.txt"; }
std::string windows_wsl_rootfs_file_name()            { return "pjarczak_wsl_rootfs.tar.gz"; }
std::string windows_wsl_import_script_file_name()     { return "install_runtime.ps1"; }
std::string windows_wsl_validate_script_file_name()   { return "validate_runtime.ps1"; }
std::string windows_wsl_bootstrap_script_file_name()  { return "pjarczak-wsl-run-host-v2.sh"; }
std::string windows_plugin_cache_subdir_file_name()   { return "pjarczak_plugin_cache_subdir.txt"; }

// ============================================================================
// SHA-256 via OpenSSL
// ============================================================================

std::string sha256_file_hex(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    char buf[8192];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(in.gcount())) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss.width(2);
        oss.fill('0');
        oss << static_cast<unsigned>(digest[i]);
    }
    return oss.str();
}

// ============================================================================
// ELF Validation (Linux / macOS only)
// ============================================================================

bool validate_linux_so_binary(const std::filesystem::path& path, std::string* reason)
{
#if defined(_MSC_VER) || defined(_WIN32)
    // Can't validate Linux ELF on Windows — assume ok if file exists
    if (!std::filesystem::exists(path)) {
        if (reason) *reason = "file does not exist: " + path.string();
        return false;
    }
    return true;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (reason) *reason = "cannot open: " + path.string();
        return false;
    }

    uint8_t ident[16];
    if (!in.read(reinterpret_cast<char*>(ident), 16)) {
        if (reason) *reason = "file too small for ELF ident: " + path.string();
        return false;
    }

    // ELF magic
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') {
        if (reason) *reason = "not an ELF file: " + path.string();
        return false;
    }
    // Must be 64-bit
    if (ident[4] != ELFCLASS64) {
        if (reason) *reason = "not a 64-bit ELF: " + path.string();
        return false;
    }
    // Little-endian
    if (ident[5] != ELFDATA2LSB) {
        if (reason) *reason = "not a little-endian ELF: " + path.string();
        return false;
    }

    // Read e_machine
    uint8_t e_hdr[48];
    if (!in.read(reinterpret_cast<char*>(e_hdr), 48)) {
        if (reason) *reason = "cannot read ELF header: " + path.string();
        return false;
    }
    const uint16_t e_machine = static_cast<uint16_t>(e_hdr[2]) | (static_cast<uint16_t>(e_hdr[3]) << 8);

    struct utsname uts{};
    uname(&uts);
    std::string machine(uts.machine);

    bool machine_ok = false;
    if (machine == "x86_64"  && e_machine == EM_X86_64)  machine_ok = true;
    if (machine == "aarch64" && e_machine == EM_AARCH64) machine_ok = true;

    if (!machine_ok) {
        if (reason)
            *reason = "ELF machine mismatch (host=" + machine + ", e_machine=" + std::to_string(e_machine) + "): " + path.string();
        return false;
    }
    return true;
#endif
}

bool validate_linux_so_binary(const boost::filesystem::path& path, std::string* reason)
{
    return validate_linux_so_binary(std::filesystem::path(path.string()), reason);
}

bool validate_linux_payload_file(const std::filesystem::path& path, std::string* reason)
{
    if (!std::filesystem::exists(path)) {
        if (reason) *reason = "missing: " + path.filename().string();
        return false;
    }
    if (path.extension() == ".so")
        return validate_linux_so_binary(path, reason);
    return true;
}

// ============================================================================
// Manifest
// ============================================================================

std::filesystem::path manifest_file_path(const std::filesystem::path& plugin_dir)
{
    return plugin_dir / "pjarczak_manifest.json";
}

static nlohmann::json find_manifest_entry(const nlohmann::json& manifest, const std::string& filename)
{
    if (!manifest.contains("files") || !manifest["files"].is_array())
        return nullptr;
    for (const auto& entry : manifest["files"]) {
        if (entry.value("name", std::string()) == filename)
            return entry;
    }
    return nullptr;
}

bool abi_version_matches_expected(const std::filesystem::path& so_path, std::string* reason)
{
    const char* expected = std::getenv("PJARCZAK_EXPECTED_BAMBU_NETWORK_VERSION");
    if (!expected || !*expected)
        return true; // No version check configured

    // Read the ABI version string from the .so's note section (simplified check:
    // look for the version string in the first 64 KB of the binary).
    std::ifstream in(so_path, std::ios::binary);
    if (!in) {
        if (reason) *reason = "cannot open .so for version check: " + so_path.string();
        return false;
    }
    std::string content(std::istreambuf_iterator<char>(in), {});
    if (content.find(std::string(expected)) != std::string::npos)
        return true;
    if (reason)
        *reason = "ABI version mismatch: expected '" + std::string(expected) + "' not found in " + so_path.string();
    return false;
}

bool validate_linux_payload_file_against_manifest(
    const std::filesystem::path& plugin_dir,
    const std::string& filename,
    std::string* reason)
{
    const auto manifest_path = manifest_file_path(plugin_dir);
    if (!std::filesystem::exists(manifest_path))
        return true; // No manifest — skip validation

    std::ifstream in(manifest_path);
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(in);
    } catch (...) {
        return true; // Unparseable manifest — skip
    }

    const auto entry = find_manifest_entry(manifest, filename);
    if (entry.is_null())
        return true; // Not listed in manifest — skip

    const auto file_path = plugin_dir / filename;
    const std::string expected_sha256 = entry.value("sha256", std::string());
    if (!expected_sha256.empty()) {
        const std::string actual = sha256_file_hex(file_path);
        if (actual != expected_sha256) {
            if (reason)
                *reason = "SHA-256 mismatch for " + filename + " (expected=" + expected_sha256 + ", actual=" + actual + ")";
            return false;
        }
    }

    const std::string expected_abi = entry.value("abi_version", std::string());
    if (!expected_abi.empty() && file_path.extension() == ".so") {
        if (!abi_version_matches_expected(file_path, reason))
            return false;
    }

    return true;
}

// ============================================================================
// Preflight
// ============================================================================

bool bridge_payload_preflight(const boost::filesystem::path& plugin_dir_boost, std::string* reason)
{
    const std::filesystem::path plugin_dir(plugin_dir_boost.string());

    // Check that the bridge shared library itself exists
    const std::filesystem::path bridge_lib = plugin_dir / bridge_network_library_name();
    if (!std::filesystem::exists(bridge_lib)) {
        if (reason) *reason = "bridge library missing: " + bridge_lib.string();
        return false;
    }

    // Platform-specific runtime file checks
#if defined(_MSC_VER) || defined(_WIN32)
    const std::vector<std::string> required = {
        host_executable_file_name(),
        windows_wsl_distro_file_name(),
        windows_wsl_rootfs_file_name(),
        windows_wsl_import_script_file_name(),
    };
#elif defined(__WXMAC__) || defined(__APPLE__)
    const std::vector<std::string> required = {
        host_executable_file_name(),
        mac_lima_instance_file_name(),
    };
#else
    const std::vector<std::string> required = {
        host_executable_file_name(),
    };
#endif

    for (const auto& name : required) {
        const auto p = plugin_dir / name;
        if (!std::filesystem::exists(p)) {
            if (reason) *reason = "required bridge file missing: " + name;
            return false;
        }
    }

    // Validate the Linux .so files if present
    const std::filesystem::path network_so = plugin_dir / linux_network_library_name();
    if (std::filesystem::exists(network_so)) {
        std::string val_reason;
        if (!validate_linux_payload_file(network_so, &val_reason)) {
            if (reason) *reason = "network library validation failed: " + val_reason;
            return false;
        }
    }

    return true;
}

std::string list_bridge_plugin_dir_files(const boost::filesystem::path& plugin_dir_boost)
{
    const std::filesystem::path plugin_dir(plugin_dir_boost.string());
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(plugin_dir)) {
            if (!first) oss << ", ";
            oss << entry.path().filename().string();
            first = false;
        }
    } catch (...) {}
    oss << "]";
    return oss.str();
}

// ============================================================================
// OTA
// ============================================================================

std::vector<std::string> ota_copy_extensions()
{
    return {".so", ".json", ".dll", ".dylib", ".ps1", ".txt", ".sh", ".tar"};
}

} // namespace Slic3r::PJarczakLinuxBridge
