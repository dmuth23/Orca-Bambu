cmake_minimum_required(VERSION 3.13)

# ============================================================================
# pjarczak_bambu_networking_bridge — runtime bridge shared library
#
# This library is loaded by BBLNetworkPlugin at runtime (in place of the real
# Bambu network DLL/dylib/so) when PJARCZAK_BRIDGE_ENABLED is set.  It
# forwards every bambu_network_* and Bambu_* call to a Linux host process
# running the real libBambuNetwork.so via a stdio-based JSON-RPC protocol.
#
# The host process runs:
#   - directly on Linux
#   - inside WSL2 on Windows
#   - inside a Lima VM on macOS
# ============================================================================

set(BRIDGE_SOURCES
    PJarczakBambuNetworkForwarderExports.cpp
    PJarczakBambuNetworkForwarderState.cpp
    PJarczakLinuxBridgeConfig.cpp
    PJarczakLinuxSoBridgeRpcClient.cpp
    PJarczakLinuxSoBridgeRpcProtocol.cpp
    PJarczakLinuxSoBridgeEventPump.cpp
)

if (WIN32)
    list(APPEND BRIDGE_SOURCES PJarczakLinuxSoBridgeLauncher_win.cpp)
elseif (APPLE)
    list(APPEND BRIDGE_SOURCES PJarczakLinuxSoBridgeLauncher_mac.mm)
else()
    list(APPEND BRIDGE_SOURCES PJarczakLinuxSoBridgeLauncher_linux.cpp)
endif()

add_library(pjarczak_bambu_networking_bridge SHARED ${BRIDGE_SOURCES})

target_compile_definitions(pjarczak_bambu_networking_bridge PRIVATE
    BAMBU_EXPORTS
    PJARCZAK_LINUX_BRIDGE_STANDALONE_HOST
)

# Include paths: this directory, Utils, slic3r, src, and deps_src for nlohmann/json.hpp
target_include_directories(pjarczak_bambu_networking_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
target_include_directories(pjarczak_bambu_networking_bridge SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/deps_src
)

# Use the project-level targets which are already defined
target_link_libraries(pjarczak_bambu_networking_bridge
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::filesystem
    Boost::system
    Boost::log
    nlohmann_json
)

if (APPLE)
    target_link_libraries(pjarczak_bambu_networking_bridge PRIVATE "-framework Foundation")
    set_target_properties(pjarczak_bambu_networking_bridge PROPERTIES
        MACOSX_RPATH ON
        INSTALL_RPATH "@loader_path"
    )
elseif (UNIX AND NOT WIN32)
    target_link_libraries(pjarczak_bambu_networking_bridge PRIVATE dl pthread)
    set_target_properties(pjarczak_bambu_networking_bridge PROPERTIES
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# Hide all symbols by default; only EXPORT_API-decorated symbols are visible.
set_target_properties(pjarczak_bambu_networking_bridge PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Output library alongside the main app plugins directory.
set_target_properties(pjarczak_bambu_networking_bridge PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
