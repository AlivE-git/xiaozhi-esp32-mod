#ifndef LIST_STORAGE_H
#define LIST_STORAGE_H

#include <cstddef>
#include <string>

// The radio station, RSS feed and Wake-on-LAN computer lists are JSON documents
// of a few KB each, which does not fit the small NVS partition. They are kept
// as files on the writable LittleFS "storage" partition instead.
namespace list_storage {

// Mounts the partition, formatting it the first time so an empty partition
// works without flashing a filesystem image. Returns false when the partition
// is missing or cannot be mounted; every other call then fails quietly.
bool Init();

// Reads a whole file of the list directory. Returns false when it is missing.
bool Read(const char* file_name, std::string& text);

// Replaces a file in one step: the data is written next to it first, so a power
// loss keeps the previous list.
bool Write(const char* file_name, const std::string& text);

// Bytes used and available on the partition; false when it is not mounted.
bool GetStats(size_t& used_bytes, size_t& total_bytes);

}  // namespace list_storage

#endif  // LIST_STORAGE_H
