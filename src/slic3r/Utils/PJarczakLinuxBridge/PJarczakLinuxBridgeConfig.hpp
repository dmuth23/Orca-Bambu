#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r::PJarczakLinuxBridge {

// ============================================================================
// Configuration / Feature Flags
// ============================================================================

bool enabled();
bool use_bridge_network_module();
bool source_module_is_network_module();

std::string forced_download_os_type();
std::string forced_client_version();

// ============================================================================
// Path / Library Naming
// ============================================================================

std::string host_executable_file_name();
std::string linux_network_library_name();
std::string linux_source_library_name();

std::string bridge_network_library_name();

// Returns absolute path to the bridge networking shared library given plugin folder
std::string bridge_network_library_path(const boost::filesystem::path& plugin_dir);
std::string bridge_network_library_path(const std::filesystem::path& plugin_dir);

// ============================================================================
// Platform-Specific Runtime File Names
// ============================================================================

// macOS
std::string mac_lima_instance_file_name();
std::string mac_runtime_install_script_file_name();
std::string mac_runtime_verify_script_file_name();
std::string mac_host_wrapper_file_name();

// Windows WSL
std::string windows_wsl_distro_file_name();
std::string windows_wsl_rootfs_file_name();
std::string windows_wsl_import_script_file_name();
std::string windows_wsl_validate_script_file_name();
std::string windows_wsl_bootstrap_script_file_name();
std::string windows_plugin_cache_subdir_file_name();

// ============================================================================
// Validation
// ============================================================================

bool validate_linux_so_binary(const std::filesystem::path& path, std::string* reason = nullptr);
bool validate_linux_so_binary(const boost::filesystem::path& path, std::string* reason = nullptr);

bool validate_linux_payload_file(const std::filesystem::path& path, std::string* reason = nullptr);

bool abi_version_matches_expected(const std::filesystem::path& so_path, std::string* reason = nullptr);

// ============================================================================
// Manifest
// ============================================================================

std::filesystem::path manifest_file_path(const std::filesystem::path& plugin_dir);

bool validate_linux_payload_file_against_manifest(
    const std::filesystem::path& plugin_dir,
    const std::string& filename,
    std::string* reason = nullptr);

// ============================================================================
// Preflight
// ============================================================================

bool bridge_payload_preflight(const boost::filesystem::path& plugin_dir, std::string* reason = nullptr);

std::string list_bridge_plugin_dir_files(const boost::filesystem::path& plugin_dir);

// ============================================================================
// Integrity
// ============================================================================

std::string sha256_file_hex(const std::filesystem::path& path);

// ============================================================================
// OTA
// ============================================================================

std::vector<std::string> ota_copy_extensions();

} // namespace Slic3r::PJarczakLinuxBridge
