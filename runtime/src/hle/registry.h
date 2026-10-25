// Virtual registry used by advapi32 and the settings UI.
#pragma once
#include <cstdint>
#include <string>

namespace rt {
void registry_init(const std::string& store_path);
void registry_set_dword(const std::string& key, const std::string& name, uint32_t value);

// Save overlay + CD install mapping (implemented in kernel32_files.cpp).
void vfs_overlay_init();
void vfs_overlay_set_root(const std::string& host_path);
std::string vfs_overlay_root();
std::string vfs_map_full_path(const std::string& guest);
std::string vfs_map_host(const std::string& guest, bool for_create);
bool vfs_map_delete(const std::string& guest);
bool vfs_map_is_deleted(const std::string& guest);
}  // namespace rt
