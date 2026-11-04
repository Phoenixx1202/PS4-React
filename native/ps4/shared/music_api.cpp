// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// MP3 music on the mixer's music deck. Local files and HTTP(S) URLs are
// decoded incrementally with minimp3_ex; a worker thread resamples to 48 kHz
// and fills the stream ring. HTTP bodies are fed through a bounded byte ring
// so the decoder waits for the network without downloading the whole track.

#include "host_api.hpp"

#include "audio/mixer.hpp"
#include "network.hpp"
#include "system.hpp"

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C" {
#include "er_scene.h"
}

#include <curl/curl.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kNoSeek = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kHttpRingBytes = 1u << 20;
constexpr std::size_t kMetadataPrefixBytes = 512u << 10;
constexpr std::size_t kOutputFrames = 2048;
constexpr int kCrossfadeMs = 1500;

enum class State : int { playing, paused, completed, failed, stopped };

struct Metadata {
    std::string title;
    std::string artist;
    std::string album;
};

struct Task {
    std::uint32_t id = 0;
    std::string source;
    bool remote = false;
    bool vorbis = false;
    int slot = 0;
    hui::audio::StreamRing* deck = nullptr;
    float volume = 1.0f;
    std::atomic<State> state{State::playing};
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> position{0};  // output frames at 48 kHz
    std::atomic<std::uint64_t> seek_ms{kNoSeek};
    std::atomic<std::int64_t> fade_deadline_ms{0};
    std::mutex info_mutex;
    double duration = 0;
    Metadata metadata;
    std::vector<std::uint8_t> cover;
    std::vector<std::uint32_t> cover_pixels;
    std::string cover_name;
    bool cover_failed = false;
    std::string error;
};

// Bounded SPSC byte queue between the HTTP worker and the decoder. The writer
// blocks when full and the reader blocks when empty, so a fast network cannot
// grow memory without bound.
class ByteRing {
public:
    explicit ByteRing(std::size_t capacity) : data_(capacity) {}

    bool write(const std::uint8_t* source, std::size_t size, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (size > 0) {
            cv_.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return used_ < data_.size() || eof_ || stop.load();
            });
            if (stop.load() || eof_) return false;
            const std::size_t free = data_.size() - used_;
            if (free == 0) continue;
            const std::size_t chunk = std::min(size, free);
            const std::size_t first = std::min(chunk, data_.size() - head_);
            std::memcpy(data_.data() + head_, source, first);
            if (chunk > first) std::memcpy(data_.data(), source + first, chunk - first);
            head_ = (head_ + chunk) % data_.size();
            used_ += chunk;
            source += chunk;
            size -= chunk;
            cv_.notify_all();
        }
        return true;
    }

    std::size_t read(std::uint8_t* target, std::size_t size, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (used_ == 0 && !eof_ && !stop.load()) {
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (used_ == 0) return 0;
        const std::size_t chunk = std::min(size, used_);
        const std::size_t first = std::min(chunk, data_.size() - tail_);
        std::memcpy(target, data_.data() + tail_, first);
        if (chunk > first) std::memcpy(target + first, data_.data(), chunk - first);
        tail_ = (tail_ + chunk) % data_.size();
        used_ -= chunk;
        cv_.notify_all();
        return chunk;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::uint8_t> data_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t used_ = 0;
    bool eof_ = false;
};

struct HttpState {
    explicit HttpState(std::shared_ptr<Task> owner) : task(std::move(owner)) {}
    ByteRing ring{kHttpRingBytes};
    std::shared_ptr<Task> task;
    CURL* curl = nullptr;
    std::atomic<long long> content_length{-1};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::string error;
    std::mutex prefix_mutex;
    std::vector<std::uint8_t> prefix;
    bool prefix_done = false;
};

struct HttpJob {
    std::shared_ptr<HttpState> state;
};

hui::audio::StreamRing deck_a(1u << 15);
hui::audio::StreamRing deck_b(1u << 15);
hui::audio::Mixer* mixer = nullptr;
std::mutex task_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::shared_ptr<Task> current;
std::atomic<std::uint32_t> next_id{1};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool faded_out(const Task& task) {
    const std::int64_t deadline = task.fade_deadline_ms.load();
    return deadline != 0 && now_ms() >= deadline;
}

const char* state_name(State state) {
    switch (state) {
        case State::playing: return "playing";
        case State::paused: return "paused";
        case State::completed: return "completed";
        case State::failed: return "failed";
        default: return "stopped";
    }
}

void set_error(Task& task, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(task.info_mutex);
        task.error = message;
    }
    task.state = State::failed;
    ps4::system::log("music: %s failed: %s", task.source.c_str(), message.c_str());
}

std::uint32_t big_endian(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::uint32_t synchsafe(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0] & 0x7F) << 21) |
           (static_cast<std::uint32_t>(data[1] & 0x7F) << 14) |
           (static_cast<std::uint32_t>(data[2] & 0x7F) << 7) |
           (static_cast<std::uint32_t>(data[3] & 0x7F));
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string decode_id3_text(const std::uint8_t* data, std::size_t size) {
    if (size < 1) return {};
    const std::uint8_t encoding = data[0];
    const std::uint8_t* text = data + 1;
    const std::size_t length = size - 1;
    std::string out;
    if (encoding == 0 || encoding == 3) {
        for (std::size_t i = 0; i < length && text[i]; ++i) {
            if (encoding == 0) {
                append_utf8(out, text[i]);
            } else {
                out.push_back(static_cast<char>(text[i]));
            }
        }
        return out;
    }
    if (encoding != 1 && encoding != 2) return out;
    bool little = encoding == 1;
    std::size_t i = 0;
    if (encoding == 1 && length >= 2) {
        if (text[0] == 0xFF && text[1] == 0xFE) {
            little = true;
            i = 2;
        } else if (text[0] == 0xFE && text[1] == 0xFF) {
            little = false;
            i = 2;
        }
    }
    while (i + 1 < length) {
        const std::uint32_t unit = little
                                       ? (static_cast<std::uint32_t>(text[i]) |
                                          (static_cast<std::uint32_t>(text[i + 1]) << 8))
                                       : ((static_cast<std::uint32_t>(text[i]) << 8) |
                                          static_cast<std::uint32_t>(text[i + 1]));
        i += 2;
        if (unit == 0) break;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < length) {
            const std::uint32_t low = little
                                          ? (static_cast<std::uint32_t>(text[i]) |
                                             (static_cast<std::uint32_t>(text[i + 1]) << 8))
                                          : ((static_cast<std::uint32_t>(text[i]) << 8) |
                                             static_cast<std::uint32_t>(text[i + 1]));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                i += 2;
                append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                continue;
            }
        }
        append_utf8(out, unit);
    }
    return out;
}

bool parse_id3v2(const std::uint8_t* data, std::size_t size, Metadata& metadata,
                 std::vector<std::uint8_t>* cover = nullptr) {
    if (size < 10 || std::memcmp(data, "ID3", 3) != 0) return false;
    const int version = data[3];
    if (version < 2 || version > 4) return false;
    const std::uint32_t tag_size = synchsafe(data + 6);
    if (tag_size == 0 || tag_size + 10 > size) return false;
    const std::uint8_t* at = data + 10;
    const std::uint8_t* end = at + tag_size;
    while (at + 10 <= end && at[0] != 0) {
        char id[5] = {static_cast<char>(at[0]), static_cast<char>(at[1]),
                      static_cast<char>(at[2]), static_cast<char>(at[3]), 0};
        const std::uint32_t frame_size =
            version == 4 ? synchsafe(at + 4) : big_endian(at + 4);
        if (frame_size == 0 || at + 10 + frame_size > end) break;
        const std::uint8_t* body = at + 10;
        if (std::strcmp(id, "TIT2") == 0) metadata.title = decode_id3_text(body, frame_size);
        else if (std::strcmp(id, "TPE1") == 0) metadata.artist = decode_id3_text(body, frame_size);
        else if (std::strcmp(id, "TALB") == 0) metadata.album = decode_id3_text(body, frame_size);
        else if (std::strcmp(id, "APIC") == 0 && cover && cover->empty()) {
            const std::uint8_t* at_picture = body;
            const std::uint8_t* end_picture = body + frame_size;
            if (at_picture < end_picture) {
                const std::uint8_t encoding = *at_picture++;
                while (at_picture < end_picture && *at_picture) ++at_picture;
                if (at_picture < end_picture) ++at_picture;
                if (at_picture < end_picture) ++at_picture;
                if (encoding == 1 || encoding == 2) {
                    while (at_picture + 1 < end_picture &&
                           !(at_picture[0] == 0 && at_picture[1] == 0))
                        at_picture += 2;
                    if (at_picture + 1 < end_picture) at_picture += 2;
                } else {
                    while (at_picture < end_picture && *at_picture) ++at_picture;
                    if (at_picture < end_picture) ++at_picture;
                }
                if (at_picture < end_picture)
                    cover->assign(at_picture, end_picture);
            }
        }
        at += 10 + frame_size;
    }
    return true;
}

void set_metadata(Task& task, const Metadata& metadata) {
    std::lock_guard<std::mutex> lock(task.info_mutex);
    if (!metadata.title.empty()) task.metadata.title = metadata.title;
    if (!metadata.artist.empty()) task.metadata.artist = metadata.artist;
    if (!metadata.album.empty()) task.metadata.album = metadata.album;
}

void read_local_metadata(Task& task) {
    FILE* file = std::fopen(task.source.c_str(), "rb");
    if (!file) return;
    std::uint8_t header[10]{};
    if (std::fread(header, 1, sizeof header, file) == sizeof header &&
        std::memcmp(header, "ID3", 3) == 0) {
        const std::uint32_t tag_size = synchsafe(header + 6);
        if (tag_size > 0 && tag_size < kMetadataPrefixBytes) {
            std::vector<std::uint8_t> tag(10 + tag_size);
            std::memcpy(tag.data(), header, 10);
            if (std::fread(tag.data() + 10, 1, tag_size, file) == tag_size) {
                Metadata metadata;
                std::vector<std::uint8_t> cover;
                if (parse_id3v2(tag.data(), tag.size(), metadata, &cover)) {
                    set_metadata(task, metadata);
                    if (!cover.empty()) {
                        std::lock_guard<std::mutex> lock(task.info_mutex);
                        task.cover = std::move(cover);
                    }
                }
            }
        }
    }
    std::fclose(file);
}

bool vorbis_name(const std::string& source) {
    std::string path = source;
    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path.resize(query);
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = path.substr(dot + 1);
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == "ogg" || extension == "oga";
}

bool local_vorbis(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char magic[4]{};
    const bool ogg = std::fread(magic, 1, sizeof magic, file) == sizeof magic &&
                     std::memcmp(magic, "OggS", 4) == 0;
    std::fclose(file);
    return ogg;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<std::uint8_t> base64_decode(const std::string& text) {
    std::vector<std::uint8_t> out;
    std::uint32_t value = 0;
    int bits = -8;
    for (char c : text) {
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        const int digit = base64_value(c);
        if (digit < 0) return {};
        value = (value << 6) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
            if (out.size() > 4u * 1024 * 1024) return {};
        }
    }
    return out;
}

bool parse_picture_block(const std::vector<std::uint8_t>& block, std::vector<std::uint8_t>& cover) {
    std::size_t at = 0;
    auto read32 = [&](std::uint32_t& value) {
        if (at + 4 > block.size()) return false;
        value = (static_cast<std::uint32_t>(block[at]) << 24) |
                (static_cast<std::uint32_t>(block[at + 1]) << 16) |
                (static_cast<std::uint32_t>(block[at + 2]) << 8) | block[at + 3];
        at += 4;
        return true;
    };
    std::uint32_t type = 0, mime_length = 0, description_length = 0;
    std::uint32_t width = 0, height = 0, depth = 0, colors = 0, data_length = 0;
    if (!read32(type) || !read32(mime_length) || at + mime_length > block.size()) return false;
    at += mime_length;
    if (!read32(description_length) || at + description_length > block.size()) return false;
    at += description_length;
    if (!read32(width) || !read32(height) || !read32(depth) || !read32(colors) ||
        !read32(data_length))
        return false;
    if (data_length == 0 || data_length > 4u * 1024 * 1024 || at + data_length > block.size())
        return false;
    cover.assign(block.begin() + static_cast<std::ptrdiff_t>(at),
                 block.begin() + static_cast<std::ptrdiff_t>(at + data_length));
    return true;
}

void vorbis_metadata(stb_vorbis* vorbis, Task& task) {
    const stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis);
    Metadata metadata;
    std::vector<std::uint8_t> cover;
    for (int i = 0; i < comments.comment_list_length; ++i) {
        const char* entry = comments.comment_list[i];
        if (!entry) continue;
        const char* equals = std::strchr(entry, '=');
        if (!equals) continue;
        const std::string key(entry, equals);
        std::string upper = key;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const char* value = equals + 1;
        if (upper == "TITLE") metadata.title = value;
        else if (upper == "ARTIST") metadata.artist = value;
        else if (upper == "ALBUM") metadata.album = value;
        else if (upper == "METADATA_BLOCK_PICTURE" && cover.empty()) {
            const std::vector<std::uint8_t> block = base64_decode(value);
            parse_picture_block(block, cover);
        } else if (upper == "COVERART" && cover.empty()) {
            cover = base64_decode(value);
        }
    }
    if (!metadata.title.empty() || !metadata.artist.empty() || !metadata.album.empty())
        set_metadata(task, metadata);
    if (!cover.empty()) {
        std::lock_guard<std::mutex> lock(task.info_mutex);
        task.cover = std::move(cover);
    }
}

std::string ensure_cover(Task& task) {
    std::lock_guard<std::mutex> lock(task.info_mutex);
    if (!task.cover_name.empty()) return task.cover_name;
    if (task.cover.empty() || task.cover_failed) return {};
    int width = 0, height = 0, components = 0;
    unsigned char* source = stbi_load_from_memory(
        task.cover.data(), static_cast<int>(task.cover.size()), &width, &height, &components, 4);
    if (!source || width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) * height > 5000000) {
        if (source) stbi_image_free(source);
        task.cover_failed = true;
        return {};
    }
    task.cover_pixels.resize(static_cast<std::size_t>(width) * height);
    bool opaque = true;
    for (std::size_t i = 0; i < task.cover_pixels.size(); ++i) {
        const unsigned a = source[i * 4 + 3];
        if (a != 255) opaque = false;
        const unsigned r = source[i * 4 + 0] * a / 255;
        const unsigned g = source[i * 4 + 1] * a / 255;
        const unsigned b = source[i * 4 + 2] * a / 255;
        task.cover_pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(source);
    char name[24];
    std::snprintf(name, sizeof name, "@music:%u", task.id);
    if (!er_image_load_argb(name, task.cover_pixels.data(), width, height, opaque)) {
        task.cover_pixels.clear();
        task.cover_failed = true;
        return {};
    }
    task.cover_name = name;
    return task.cover_name;
}

void drop_cover(Task& task) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(task.info_mutex);
        name = task.cover_name;
        task.cover_name.clear();
        task.cover.clear();
        task.cover_failed = true;
    }
    if (!name.empty()) er_image_unload(name.c_str());
    {
        std::lock_guard<std::mutex> lock(task.info_mutex);
        task.cover_pixels.clear();
    }
}

void update_http_metadata(HttpState& http) {
    std::lock_guard<std::mutex> lock(http.prefix_mutex);
    if (http.prefix_done) return;
    if (http.prefix.size() < 10) return;
    if (std::memcmp(http.prefix.data(), "ID3", 3) != 0) {
        http.prefix_done = true;
        http.prefix.clear();
        return;
    }
    const std::uint32_t tag_size = synchsafe(http.prefix.data() + 6);
    if (tag_size == 0 || tag_size >= kMetadataPrefixBytes) {
        http.prefix_done = true;
        http.prefix.clear();
        return;
    }
    if (http.prefix.size() < static_cast<std::size_t>(tag_size) + 10) return;
    Metadata metadata;
    std::vector<std::uint8_t> cover;
    if (parse_id3v2(http.prefix.data(), http.prefix.size(), metadata, &cover)) {
        set_metadata(*http.task, metadata);
        if (!cover.empty()) {
            std::lock_guard<std::mutex> lock(http.task->info_mutex);
            http.task->cover = std::move(cover);
        }
    }
    http.prefix_done = true;
    http.prefix.clear();
}

std::size_t http_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* http = static_cast<HttpState*>(user);
    const std::size_t bytes = size * count;
    if (!bytes) return 0;
    if (http->stop.load() || http->task->stop.load()) return 0;
    if (http->curl && http->content_length.load() < 0) {
        curl_off_t length = 0;
        if (curl_easy_getinfo(http->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK &&
            length > 0)
            http->content_length = static_cast<long long>(length);
    }
    {
        std::lock_guard<std::mutex> lock(http->prefix_mutex);
        if (!http->prefix_done && http->prefix.size() < kMetadataPrefixBytes) {
            const std::size_t room = kMetadataPrefixBytes - http->prefix.size();
            const std::size_t take = std::min(room, bytes);
            http->prefix.insert(http->prefix.end(), data, data + take);
        }
    }
    update_http_metadata(*http);
    if (!http->ring.write(reinterpret_cast<const std::uint8_t*>(data), bytes, http->stop)) return 0;
    return bytes;
}

int http_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* http = static_cast<HttpState*>(user);
    return (http->stop.load() || http->task->stop.load()) ? 1 : 0;
}

void* http_entry(void* argument) {
    std::unique_ptr<HttpJob> job(static_cast<HttpJob*>(argument));
    HttpState& http = *job->state;
    CURL* curl = curl_easy_init();
    if (!curl) {
        {
            std::lock_guard<std::mutex> lock(http.error_mutex);
            http.error = "cannot initialize HTTP";
        }
        http.failed = true;
        http.ring.finish();
        return nullptr;
    }
    if (!network::configure_transport(curl, http.task->source, true)) {
        {
            std::lock_guard<std::mutex> lock(http.error_mutex);
            http.error = "cannot configure HTTP";
        }
        http.failed = true;
        curl_easy_cleanup(curl);
        http.ring.finish();
        return nullptr;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &http);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &http);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64L * 1024);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    http.curl = curl;
    ps4::system::log("music: GET %s", http.task->source.c_str());
    const CURLcode result = curl_easy_perform(curl);
    http.curl = nullptr;
    if (result != CURLE_OK && !http.stop.load() && !http.task->stop.load()) {
        std::lock_guard<std::mutex> lock(http.error_mutex);
        http.error = curl_easy_strerror(result);
        http.failed = true;
        ps4::system::log("music: HTTP failed result=%d %s", static_cast<int>(result),
                         http.error.c_str());
    }
    curl_easy_cleanup(curl);
    http.ring.finish();
    return nullptr;
}

void start_http(const std::shared_ptr<HttpState>& http) {
    auto* job = new HttpJob{http};
    pthread_attr_t attributes;
    pthread_t thread{};
    bool started = false;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, http_entry, job) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete job;
        http->failed = true;
        http->ring.finish();
    }
}

void run_local(const std::shared_ptr<Task>& owner) {
    Task& task = *owner;
    mp3dec_ex_t decoder{};
    const int opened = mp3dec_ex_open(&decoder, task.source.c_str(), MP3D_SEEK_TO_SAMPLE);
    if (opened != 0) {
        set_error(task, "cannot decode the MP3");
        return;
    }
    read_local_metadata(task);

    const int hz = decoder.info.hz;
    const int channels = decoder.info.channels;
    if (hz <= 0 || channels < 1) {
        mp3dec_ex_close(&decoder);
        set_error(task, "invalid MP3 stream");
        return;
    }
    const std::uint64_t total_samples = decoder.samples;
    const std::uint64_t source_frames = total_samples / static_cast<std::uint64_t>(channels);
    {
        std::lock_guard<std::mutex> lock(task.info_mutex);
        task.duration = source_frames ? static_cast<double>(source_frames) / hz : 0.0;
    }
    ps4::system::log("music: %s %d Hz %d ch, %llu frames", task.source.c_str(), hz, channels,
                     static_cast<unsigned long long>(source_frames));

    std::vector<mp3d_sample_t> pcm(8192);
    std::vector<mp3d_sample_t> pending;
    std::uint64_t pending_base = 0;
    double source_pos = 0.0;
    const double ratio = static_cast<double>(hz) / 48000.0;
    std::vector<float> output(kOutputFrames * 2);
    bool eof = false;

    while (!task.stop.load()) {
        if (faded_out(task)) {
            task.stop = true;
            task.state = State::stopped;
            break;
        }
        const std::uint64_t seek = task.seek_ms.exchange(kNoSeek);
        if (seek != kNoSeek) {
            std::uint64_t frame = seek * static_cast<std::uint64_t>(hz) / 1000;
            if (source_frames && frame >= source_frames) frame = source_frames - 1;
            if (mp3dec_ex_seek(&decoder, frame * static_cast<std::uint64_t>(channels)) == 0) {
                pending.clear();
                pending_base = frame;
                source_pos = static_cast<double>(frame);
                task.position = frame * 48000ull / static_cast<std::uint64_t>(hz);
                eof = false;
            } else {
                ps4::system::log("music: seek to %llu ms failed", static_cast<unsigned long long>(seek));
            }
        }
        if (task.paused.load()) {
            task.state = State::paused;
            ps4::system::sleep_us(20000);
            continue;
        }
        task.state = State::playing;

        if (!eof) {
            const std::size_t got = mp3dec_ex_read(&decoder, pcm.data(), pcm.size());
            if (got) {
                pending.insert(pending.end(), pcm.begin(), pcm.begin() + got);
            } else {
                eof = true;
            }
        }

        const std::uint64_t pending_end = pending_base + pending.size() / channels;
        std::size_t produced = 0;
        while (produced < kOutputFrames) {
            const std::uint64_t index = static_cast<std::uint64_t>(std::floor(source_pos));
            if (index >= pending_end) break;
            if (!eof && index + 1 >= pending_end) break;
            const double fraction = source_pos - static_cast<double>(index);
            const std::size_t at = static_cast<std::size_t>(index - pending_base);
            const std::size_t next = index + 1 < pending_end ? at + 1 : at;
            const float left = static_cast<float>(pending[at * channels]) / 32768.0f;
            const float right = channels > 1 ? static_cast<float>(pending[at * channels + 1]) / 32768.0f : left;
            const float next_left = static_cast<float>(pending[next * channels]) / 32768.0f;
            const float next_right =
                channels > 1 ? static_cast<float>(pending[next * channels + 1]) / 32768.0f : next_left;
            output[produced * 2] = left + (next_left - left) * static_cast<float>(fraction);
            output[produced * 2 + 1] = right + (next_right - right) * static_cast<float>(fraction);
            ++produced;
            source_pos += ratio;
        }

        std::size_t offset = 0;
        while (offset < produced && !task.stop.load() && !faded_out(task)) {
            const std::size_t written = task.deck->write(output.data() + offset * 2, produced - offset);
            offset += written;
            task.position += written;
            if (!written) {
                if (task.paused.load()) break;
                ps4::system::sleep_us(5000);
            }
        }

        const std::uint64_t keep = static_cast<std::uint64_t>(std::floor(source_pos));
        if (keep > pending_base && pending.size() / channels > 16384) {
            const std::size_t drop = static_cast<std::size_t>(keep - pending_base);
            pending.erase(pending.begin(), pending.begin() + drop * channels);
            pending_base += drop;
        }
        if (eof && produced == 0 && source_pos >= pending_end) break;
    }

    mp3dec_ex_close(&decoder);
    if (task.stop.load()) {
        task.state = State::stopped;
        return;
    }
    task.state = State::completed;
    ps4::system::log("music: completed %s", task.source.c_str());
}

void run_remote(const std::shared_ptr<Task>& owner) {
    Task& task = *owner;
    auto http = std::make_shared<HttpState>(owner);
    start_http(http);

    mp3dec_t decoder{};
    mp3dec_init(&decoder);
    std::vector<std::uint8_t> input;
    input.reserve(64 * 1024);
    std::vector<mp3d_sample_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    std::vector<mp3d_sample_t> pending;
    std::vector<float> output(kOutputFrames * 2);
    std::uint64_t pending_base = 0;
    double source_pos = 0.0;
    int hz = 0, channels = 0;
    double ratio = 1.0;
    bool eof = false;
    bool id3_checked = false;
    std::uint64_t skip_tag = 0;
    bool seek_warned = false;

    auto drain = [&](bool final) {
        const std::uint64_t pending_end = pending_base + pending.size() / channels;
        std::size_t produced = 0;
        while (produced < kOutputFrames) {
            const std::uint64_t index = static_cast<std::uint64_t>(std::floor(source_pos));
            if (index >= pending_end) break;
            if (!final && index + 1 >= pending_end) break;
            const double fraction = source_pos - static_cast<double>(index);
            const std::size_t at = static_cast<std::size_t>(index - pending_base);
            const std::size_t next = index + 1 < pending_end ? at + 1 : at;
            const float left = static_cast<float>(pending[at * channels]) / 32768.0f;
            const float right = channels > 1 ? static_cast<float>(pending[at * channels + 1]) / 32768.0f : left;
            const float next_left = static_cast<float>(pending[next * channels]) / 32768.0f;
            const float next_right =
                channels > 1 ? static_cast<float>(pending[next * channels + 1]) / 32768.0f : next_left;
            output[produced * 2] = left + (next_left - left) * static_cast<float>(fraction);
            output[produced * 2 + 1] = right + (next_right - right) * static_cast<float>(fraction);
            ++produced;
            source_pos += ratio;
        }
        std::size_t offset = 0;
        while (offset < produced && !task.stop.load() && !faded_out(task)) {
            const std::size_t written = task.deck->write(output.data() + offset * 2, produced - offset);
            offset += written;
            task.position += written;
            if (!written) {
                if (task.paused.load()) break;
                ps4::system::sleep_us(5000);
            }
        }
        const std::uint64_t keep = static_cast<std::uint64_t>(std::floor(source_pos));
        if (keep > pending_base && pending.size() / channels > 16384) {
            const std::size_t drop = static_cast<std::size_t>(keep - pending_base);
            pending.erase(pending.begin(), pending.begin() + drop * channels);
            pending_base += drop;
        }
        return produced;
    };

    while (!task.stop.load()) {
        if (faded_out(task)) {
            task.stop = true;
            task.state = State::stopped;
            break;
        }
        const std::uint64_t seek = task.seek_ms.exchange(kNoSeek);
        if (seek != kNoSeek && !seek_warned) {
            ps4::system::log("music: seek is not supported on HTTP streams");
            seek_warned = true;
        }
        if (task.paused.load()) {
            task.state = State::paused;
            ps4::system::sleep_us(20000);
            continue;
        }
        task.state = State::playing;

        if (!eof) {
            std::uint8_t chunk[16384];
            const std::size_t got = http->ring.read(chunk, sizeof chunk, http->stop);
            if (got) {
                input.insert(input.end(), chunk, chunk + got);
            } else {
                eof = true;
            }
        }

        if (!id3_checked && input.size() >= 10) {
            if (input[0] == 'I' && input[1] == 'D' && input[2] == '3') {
                skip_tag = static_cast<std::uint64_t>(synchsafe(input.data() + 6)) + 10;
            }
            id3_checked = true;
        }
        if (skip_tag) {
            const std::size_t drop = static_cast<std::size_t>(std::min<std::uint64_t>(skip_tag, input.size()));
            input.erase(input.begin(), input.begin() + drop);
            skip_tag -= drop;
            if (skip_tag) {
                if (eof) break;
                continue;
            }
        }

        bool consumed = false;
        while (input.size() >= 4) {
            mp3dec_frame_info_t info{};
            const int samples = mp3dec_decode_frame(&decoder, input.data(), static_cast<int>(input.size()),
                                                    pcm.data(), &info);
            if (info.frame_bytes <= 0) break;
            input.erase(input.begin(), input.begin() + info.frame_bytes);
            consumed = true;
            if (samples <= 0) continue;
            if (hz == 0) {
                hz = info.hz;
                channels = info.channels;
                if (hz <= 0 || channels < 1) {
                    set_error(task, "invalid MP3 stream");
                    http->stop = true;
                    http->ring.finish();
                    return;
                }
                ratio = static_cast<double>(hz) / 48000.0;
                if (info.bitrate_kbps > 0) {
                    const long long length = http->content_length.load();
                    if (length > 0) {
                        std::lock_guard<std::mutex> lock(task.info_mutex);
                        task.duration =
                            static_cast<double>(length) * 8.0 / (info.bitrate_kbps * 1000.0);
                    }
                }
                ps4::system::log("music: %s %d Hz %d ch (stream)", task.source.c_str(), hz, channels);
            }
            if (info.channels != channels || info.hz != hz) continue;
            pending.insert(pending.end(), pcm.begin(), pcm.begin() + samples * channels);
            drain(false);
        }

        if (eof) {
            if (hz) drain(true);
            break;
        }
        if (!consumed && input.size() > 256 * 1024) {
            input.erase(input.begin(), input.begin() + 128 * 1024);
            ps4::system::log("music: skipping unreadable stream data");
        }
    }

    http->stop = true;
    http->ring.finish();
    if (task.stop.load()) {
        task.state = State::stopped;
        return;
    }
    if (http->failed.load()) {
        std::lock_guard<std::mutex> lock(http->error_mutex);
        set_error(task, http->error.empty() ? "HTTP stream failed" : http->error);
        return;
    }
    if (hz == 0) {
        set_error(task, "not an MP3 stream");
        return;
    }
    task.state = State::completed;
    ps4::system::log("music: completed %s", task.source.c_str());
}

void run_vorbis(const std::shared_ptr<Task>& owner) {
    Task& task = *owner;
    std::shared_ptr<HttpState> http;
    stb_vorbis* vorbis = nullptr;
    std::vector<std::uint8_t> input;
    std::vector<mp3d_sample_t> pcm;
    std::vector<mp3d_sample_t> pending;
    std::vector<float> output(kOutputFrames * 2);
    std::uint64_t pending_base = 0;
    double source_pos = 0.0;
    int hz = 0, channels = 0;
    double ratio = 1.0;
    bool eof = false;
    bool seek_warned = false;

    auto drain = [&](bool final) {
        const std::uint64_t pending_end = pending_base + pending.size() / channels;
        std::size_t produced = 0;
        while (produced < kOutputFrames) {
            const std::uint64_t index = static_cast<std::uint64_t>(std::floor(source_pos));
            if (index >= pending_end) break;
            if (!final && index + 1 >= pending_end) break;
            const double fraction = source_pos - static_cast<double>(index);
            const std::size_t at = static_cast<std::size_t>(index - pending_base);
            const std::size_t next = index + 1 < pending_end ? at + 1 : at;
            const float left = static_cast<float>(pending[at * channels]) / 32768.0f;
            const float right = channels > 1 ? static_cast<float>(pending[at * channels + 1]) / 32768.0f : left;
            const float next_left = static_cast<float>(pending[next * channels]) / 32768.0f;
            const float next_right =
                channels > 1 ? static_cast<float>(pending[next * channels + 1]) / 32768.0f : next_left;
            output[produced * 2] = left + (next_left - left) * static_cast<float>(fraction);
            output[produced * 2 + 1] = right + (next_right - right) * static_cast<float>(fraction);
            ++produced;
            source_pos += ratio;
        }
        std::size_t offset = 0;
        while (offset < produced && !task.stop.load() && !faded_out(task)) {
            const std::size_t written = task.deck->write(output.data() + offset * 2, produced - offset);
            offset += written;
            task.position += written;
            if (!written) {
                if (task.paused.load()) break;
                ps4::system::sleep_us(5000);
            }
        }
        const std::uint64_t keep = static_cast<std::uint64_t>(std::floor(source_pos));
        if (keep > pending_base && pending.size() / channels > 16384) {
            const std::size_t drop = static_cast<std::size_t>(keep - pending_base);
            pending.erase(pending.begin(), pending.begin() + drop * channels);
            pending_base += drop;
        }
        return produced;
    };

    auto append = [&](const mp3d_sample_t* samples, std::size_t count) {
        pending.insert(pending.end(), samples, samples + count);
        drain(false);
    };

    if (task.remote) {
        http = std::make_shared<HttpState>(owner);
        start_http(http);
        int error = 0;
        while (!vorbis && !task.stop.load()) {
            if (!eof) {
                std::uint8_t chunk[16384];
                const std::size_t got = http->ring.read(chunk, sizeof chunk, http->stop);
                if (got) input.insert(input.end(), chunk, chunk + got);
                else eof = true;
            }
            if (input.size() < 4) {
                if (eof) break;
                continue;
            }
            int consumed = 0;
            vorbis = stb_vorbis_open_pushdata(input.data(), static_cast<int>(input.size()),
                                              &consumed, &error, nullptr);
            if (vorbis) {
                input.erase(input.begin(), input.begin() + consumed);
                break;
            }
            if (error != VORBIS_need_more_data) break;
            if (eof) break;
        }
        if (!vorbis) {
            http->stop = true;
            http->ring.finish();
            std::string message = "cannot decode the OGG";
            if (http->failed.load()) {
                std::lock_guard<std::mutex> lock(http->error_mutex);
                if (!http->error.empty()) message = http->error;
            }
            set_error(task, message);
            return;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        hz = info.sample_rate;
        channels = info.channels;
        if (hz <= 0 || channels < 1) {
            stb_vorbis_close(vorbis);
            http->stop = true;
            http->ring.finish();
            set_error(task, "invalid OGG stream");
            return;
        }
        ratio = static_cast<double>(hz) / 48000.0;
        vorbis_metadata(vorbis, task);
        ps4::system::log("music: %s %d Hz %d ch (stream)", task.source.c_str(), hz, channels);

        while (!task.stop.load()) {
            if (faded_out(task)) {
                task.stop = true;
                task.state = State::stopped;
                break;
            }
            const std::uint64_t seek = task.seek_ms.exchange(kNoSeek);
            if (seek != kNoSeek && !seek_warned) {
                ps4::system::log("music: seek is not supported on HTTP streams");
                seek_warned = true;
            }
            if (task.paused.load()) {
                task.state = State::paused;
                ps4::system::sleep_us(20000);
                continue;
            }
            task.state = State::playing;
            if (!eof) {
                std::uint8_t chunk[16384];
                const std::size_t got = http->ring.read(chunk, sizeof chunk, http->stop);
                if (got) input.insert(input.end(), chunk, chunk + got);
                else eof = true;
            }

            bool consumed = false;
            while (input.size() >= 4) {
                float** frame = nullptr;
                int frame_channels = 0;
                int samples = 0;
                const int used = stb_vorbis_decode_frame_pushdata(
                    vorbis, input.data(), static_cast<int>(input.size()), &frame_channels, &frame,
                    &samples);
                if (used > 0) {
                    input.erase(input.begin(), input.begin() + used);
                    consumed = true;
                }
                if (samples <= 0 || !frame) {
                    if (used == 0) break;
                    continue;
                }
                pcm.resize(static_cast<std::size_t>(samples) * frame_channels);
                for (int i = 0; i < samples; ++i)
                    for (int c = 0; c < frame_channels; ++c) {
                        const float value = std::clamp(frame[c][i], -1.0f, 1.0f);
                        pcm[static_cast<std::size_t>(i) * frame_channels + c] =
                            static_cast<mp3d_sample_t>(value * 32767.0f);
                    }
                append(pcm.data(), pcm.size());
            }
            if (eof) {
                drain(true);
                break;
            }
            if (!consumed && input.size() > 256 * 1024) {
                input.erase(input.begin(), input.begin() + 128 * 1024);
                ps4::system::log("music: skipping unreadable OGG data");
            }
        }
    } else {
        int error = 0;
        vorbis = stb_vorbis_open_filename(task.source.c_str(), &error, nullptr);
        if (!vorbis) {
            set_error(task, "cannot decode the OGG");
            return;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        hz = info.sample_rate;
        channels = info.channels;
        if (hz <= 0 || channels < 1) {
            stb_vorbis_close(vorbis);
            set_error(task, "invalid OGG stream");
            return;
        }
        ratio = static_cast<double>(hz) / 48000.0;
        const std::uint64_t source_frames = stb_vorbis_stream_length_in_samples(vorbis);
        {
            std::lock_guard<std::mutex> lock(task.info_mutex);
            task.duration = source_frames ? static_cast<double>(source_frames) / hz : 0.0;
        }
        vorbis_metadata(vorbis, task);
        ps4::system::log("music: %s %d Hz %d ch, %llu frames", task.source.c_str(), hz, channels,
                         static_cast<unsigned long long>(source_frames));

        while (!task.stop.load()) {
            if (faded_out(task)) {
                task.stop = true;
                task.state = State::stopped;
                break;
            }
            const std::uint64_t seek = task.seek_ms.exchange(kNoSeek);
            if (seek != kNoSeek) {
                std::uint64_t sample = seek * static_cast<std::uint64_t>(hz) / 1000;
                if (source_frames && sample >= source_frames) sample = source_frames - 1;
                if (stb_vorbis_seek(vorbis, static_cast<unsigned int>(sample))) {
                    pending.clear();
                    pending_base = sample;
                    source_pos = static_cast<double>(sample);
                    task.position = sample * 48000ull / static_cast<std::uint64_t>(hz);
                } else {
                    ps4::system::log("music: seek to %llu ms failed", static_cast<unsigned long long>(seek));
                }
            }
            if (task.paused.load()) {
                task.state = State::paused;
                ps4::system::sleep_us(20000);
                continue;
            }
            task.state = State::playing;
            pcm.resize(static_cast<std::size_t>(4096) * channels);
            const int got = stb_vorbis_get_samples_short_interleaved(
                vorbis, channels, reinterpret_cast<short*>(pcm.data()),
                static_cast<int>(pcm.size()));
            if (got <= 0) {
                drain(true);
                break;
            }
            append(pcm.data(), static_cast<std::size_t>(got) * channels);
        }
    }

    if (vorbis) stb_vorbis_close(vorbis);
    if (http) {
        http->stop = true;
        http->ring.finish();
    }
    if (task.stop.load()) {
        task.state = State::stopped;
        return;
    }
    if (http && http->failed.load()) {
        std::lock_guard<std::mutex> lock(http->error_mutex);
        set_error(task, http->error.empty() ? "HTTP stream failed" : http->error);
        return;
    }
    task.state = State::completed;
    ps4::system::log("music: completed %s", task.source.c_str());
}

void run(const std::shared_ptr<Task>& owner) {
    owner->running = true;
    if (owner->vorbis) run_vorbis(owner);
    else if (owner->remote) run_remote(owner);
    else run_local(owner);
    owner->running = false;
}

void* entry(void* value) {
    std::unique_ptr<std::shared_ptr<Task>> task(
        static_cast<std::shared_ptr<Task>*>(value));
    run(*task);
    return nullptr;
}

void start(const std::shared_ptr<Task>& task) {
    auto* argument = new std::shared_ptr<Task>(task);
    pthread_attr_t attributes;
    pthread_t thread{};
    bool started = false;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete argument;
        set_error(*task, "cannot start the music thread");
    }
}

std::shared_ptr<Task> find_task(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(task_mutex);
    for (const auto& task : tasks)
        if (task->id == id) return task;
    return {};
}

bool starts_with(const std::string& value, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

JSValue play(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Music.play: expected a path or URL");
    std::size_t length = 0;
    const char* input = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!input) return JS_EXCEPTION;
    std::string source(input, length);
    JS_FreeCString(ctx, input);
    if (source.empty() || source.size() > 2048 || source.find('\0') != std::string::npos)
        return JS_ThrowTypeError(ctx, "Music.play: invalid path or URL");
    const bool remote = starts_with(source, "http://") || starts_with(source, "https://");
    bool vorbis = vorbis_name(source);
    char path[PATH_MAX];
    if (!remote) {
        if (source.size() >= PATH_MAX || !host::resolve_path(source.c_str(), path, sizeof path))
            return JS_ThrowTypeError(ctx, "Music.play: path is not allowed");
        if (!vorbis) vorbis = local_vorbis(path);
    }
    double volume = 1;
    if (argc > 1 && JS_ToFloat64(ctx, &volume, argv[1])) return JS_EXCEPTION;
    volume = std::clamp(volume, 0.0, 1.0);

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->source = remote ? source : path;
    task->remote = remote;
    task->vorbis = vorbis;
    task->volume = static_cast<float>(volume);

    std::shared_ptr<Task> previous;
    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        previous = current;
        slot = previous ? (previous->slot == 0 ? 2 : 0) : 0;
        task->slot = slot;
        task->deck = slot == 0 ? &deck_a : &deck_b;
        tasks.push_back(task);
        if (tasks.size() > 8) {
            auto old = tasks.front();
            tasks.erase(tasks.begin());
            drop_cover(*old);
        }
        current = task;
    }

    // Stop any older task still fading on the slot we are about to reuse.
    std::shared_ptr<Task> stale;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (const auto& candidate : tasks) {
            if (candidate != task && candidate != previous && candidate->slot == slot &&
                candidate->running.load()) {
                stale = candidate;
                stale->stop = true;
                break;
            }
        }
    }
    if (stale) {
        for (int i = 0; i < 400 && stale->running.load(); ++i) ps4::system::sleep_us(5000);
        stale->deck->drain();
    }
    task->deck->drain();

    const bool fading = previous && previous->running.load() &&
                        previous->state.load() == State::playing;
    if (mixer) {
        mixer->set_stream_gain(static_cast<std::size_t>(slot), 0, 0);
        if (fading) {
            previous->fade_deadline_ms = now_ms() + kCrossfadeMs;
            mixer->set_stream_gain(static_cast<std::size_t>(previous->slot), 0,
                                   kCrossfadeMs / 1000.0f);
            mixer->set_stream_gain(static_cast<std::size_t>(slot), static_cast<float>(volume),
                                   kCrossfadeMs / 1000.0f);
        } else {
            if (previous) {
                previous->stop = true;
                previous->state = State::stopped;
                previous->deck->drain();
            }
            mixer->set_stream_gain(static_cast<std::size_t>(slot), static_cast<float>(volume), 0.05f);
        }
    }
    start(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue set_volume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!argc) return JS_ThrowTypeError(ctx, "Music.setVolume: expected a volume");
    double volume = 1;
    if (JS_ToFloat64(ctx, &volume, argv[0])) return JS_EXCEPTION;
    const float level = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    std::shared_ptr<Task> active;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        active = current;
    }
    if (active) active->volume = level;
    if (mixer && active) mixer->set_stream_gain(static_cast<std::size_t>(active->slot), level, 0.1f);
    return JS_UNDEFINED;
}

JSValue pause(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.pause: expected a task id");
    const auto task = find_task(id);
    if (task) {
        task->paused = true;
        if (mixer) mixer->set_stream_gain(static_cast<std::size_t>(task->slot), 0, 0.1f);
    }
    return JS_UNDEFINED;
}

JSValue resume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.resume: expected a task id");
    const auto task = find_task(id);
    if (task) {
        task->paused = false;
        if (mixer) mixer->set_stream_gain(static_cast<std::size_t>(task->slot), task->volume, 0.1f);
    }
    return JS_UNDEFINED;
}

JSValue stop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.stop: expected a task id");
    const auto task = find_task(id);
    if (task) {
        task->stop = true;
        task->state = State::stopped;
        task->deck->drain();
        if (mixer) mixer->set_stream_gain(static_cast<std::size_t>(task->slot), 0, 0);
    }
    return JS_UNDEFINED;
}

JSValue seek(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    double seconds = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0]) || JS_ToFloat64(ctx, &seconds, argv[1]))
        return JS_ThrowTypeError(ctx, "Music.seek: expected a task id and seconds");
    const auto task = find_task(id);
    if (task && !task->remote) {
        const double clamped = std::max(0.0, seconds);
        task->seek_ms = static_cast<std::uint64_t>(clamped * 1000.0);
        task->deck->drain();
    }
    return JS_UNDEFINED;
}

JSValue status(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.status: expected a task id");
    JSValue out = JS_NewObject(ctx);
    const auto task = find_task(id);
    if (!task) {
        JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, "unknown"));
        JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, "unknown music task"));
        return out;
    }
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, state_name(task->state)));
    JS_SetPropertyStr(ctx, out, "position", JS_NewFloat64(ctx, task->position / 48000.0));
    JS_SetPropertyStr(ctx, out, "seekable", JS_NewBool(ctx, !task->remote));
    const std::string cover = ensure_cover(*task);
    std::lock_guard<std::mutex> lock(task->info_mutex);
    JS_SetPropertyStr(ctx, out, "duration", JS_NewFloat64(ctx, task->duration));
    JS_SetPropertyStr(ctx, out, "title", JS_NewString(ctx, task->metadata.title.c_str()));
    JS_SetPropertyStr(ctx, out, "artist", JS_NewString(ctx, task->metadata.artist.c_str()));
    JS_SetPropertyStr(ctx, out, "album", JS_NewString(ctx, task->metadata.album.c_str()));
    JS_SetPropertyStr(ctx, out, "cover", JS_NewString(ctx, cover.c_str()));
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, task->error.c_str()));
    return out;
}

} // namespace

void ps4_react_music_attach(hui::audio::Mixer& target) {
    mixer = &target;
    target.attach_stream(0, &deck_a);
    target.attach_stream(2, &deck_b);
    target.set_stream_gain(0, 0, 0);
    target.set_stream_gain(2, 0, 0);
}

JSValue ps4_react_music_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "play", JS_NewCFunction(ctx, play, "play", 2));
    JS_SetPropertyStr(ctx, api, "pause", JS_NewCFunction(ctx, pause, "pause", 1));
    JS_SetPropertyStr(ctx, api, "resume", JS_NewCFunction(ctx, resume, "resume", 1));
    JS_SetPropertyStr(ctx, api, "stop", JS_NewCFunction(ctx, stop, "stop", 1));
    JS_SetPropertyStr(ctx, api, "seek", JS_NewCFunction(ctx, seek, "seek", 2));
    JS_SetPropertyStr(ctx, api, "setVolume", JS_NewCFunction(ctx, set_volume, "setVolume", 1));
    JS_SetPropertyStr(ctx, api, "status", JS_NewCFunction(ctx, status, "status", 1));
    return api;
}
