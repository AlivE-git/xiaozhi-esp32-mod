#include "features/storage/list_storage.h"

#include <esp_littlefs.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#define TAG "ListStorage"

namespace {

constexpr const char* kBasePath = "/storage";
constexpr const char* kPartitionLabel = "storage";
constexpr size_t kReadBufferSize = 256;

bool mounted = false;

std::string PathOf(const char* file_name) {
    return std::string(kBasePath) + "/" + file_name;
}

}  // namespace

namespace list_storage {

bool Init() {
    if (mounted) {
        return true;
    }
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kBasePath;
    conf.partition_label = kPartitionLabel;
    // The partition ships without a filesystem, so format it on first use.
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount the %s partition: %s", kPartitionLabel,
                 esp_err_to_name(err));
        return false;
    }
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    if (esp_littlefs_info(kPartitionLabel, &total_bytes, &used_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "Mounted %s: %u of %u bytes used", kPartitionLabel,
                 static_cast<unsigned>(used_bytes), static_cast<unsigned>(total_bytes));
    }
    mounted = true;
    return true;
}

bool Read(const char* file_name, std::string& text) {
    text.clear();
    if (!mounted) {
        return false;
    }
    FILE* file = fopen(PathOf(file_name).c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGI(TAG, "No saved %s yet", file_name);
        return false;
    }
    char buffer[kReadBufferSize];
    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, read_bytes);
    }
    fclose(file);
    return !text.empty();
}

bool Write(const char* file_name, const std::string& text) {
    if (!mounted) {
        ESP_LOGE(TAG, "Cannot write %s: the partition is not mounted", file_name);
        return false;
    }
    const std::string path = PathOf(file_name);
    const std::string temp_path = path + ".tmp";
    FILE* file = fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open %s for writing", temp_path.c_str());
        return false;
    }
    bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
    if (written && fflush(file) != 0) {
        written = false;
    }
    if (fclose(file) != 0) {
        written = false;
    }
    if (!written) {
        ESP_LOGE(TAG, "Failed to write %s", temp_path.c_str());
        remove(temp_path.c_str());
        return false;
    }
    remove(path.c_str());
    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s", temp_path.c_str(), path.c_str());
        remove(temp_path.c_str());
        return false;
    }
    return true;
}

bool GetStats(size_t& used_bytes, size_t& total_bytes) {
    used_bytes = 0;
    total_bytes = 0;
    if (!mounted) {
        return false;
    }
    return esp_littlefs_info(kPartitionLabel, &total_bytes, &used_bytes) == ESP_OK;
}

}  // namespace list_storage
