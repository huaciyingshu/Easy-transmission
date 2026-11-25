#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <httplib.h>
#include <windows.h>
#include <nlohmann/json.hpp>

#include "assets.hpp"
#include "formats.hpp"
#include "firewall.hpp"
#include "native_files.hpp"
#include "transfer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::system_clock;
using Request = httplib::Request;
using Response = httplib::Response;
using namespace std::chrono_literals;

std::atomic<bool> stopping{false};

BOOL WINAPI consoleControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    stopping = true;
    return TRUE;
}

class LanServer final : public httplib::Server {
public:
    void stopTransfers() {
        stop();
        std::lock_guard lock(socketsMutex_);
        for (const auto socket : sockets_) shutdown(socket, SD_BOTH);
    }

private:
    bool process_and_close_socket(socket_t socket) override {
        std::unique_lock lock(socketsMutex_);
        if (stopping) {
            closesocket(socket);
            return false;
        }
        const auto position = sockets_.insert(socket);
        lock.unlock();
        const auto eraseSocket = httplib::detail::scope_exit([&] {
            std::lock_guard guard(socketsMutex_);
            sockets_.erase(position);
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        });
        // Keep ownership of the accepted socket so Ctrl+C can wake blocked readers.
        std::string remoteAddress, localAddress;
        int remotePort = 0, localPort = 0;
        httplib::detail::get_remote_ip_and_port(socket, remoteAddress, remotePort);
        httplib::detail::get_local_ip_and_port(socket, localAddress, localPort);
        return httplib::detail::process_server_socket(
            svr_sock_, socket, keep_alive_max_count_, keep_alive_timeout_sec_,
            read_timeout_sec_, read_timeout_usec_, write_timeout_sec_, write_timeout_usec_,
            [&](httplib::Stream& stream, bool closeConnection, bool& connectionClosed) {
                return process_request(stream, remoteAddress, remotePort, localAddress, localPort,
                                       closeConnection, connectionClosed, nullptr);
            });
    }

    std::mutex socketsMutex_;
    std::multiset<socket_t> sockets_;
};

std::string id() {
    thread_local std::random_device random;
    std::string result;
    constexpr auto digits = "0123456789abcdef";
    for (int word = 0; word < 4; ++word) {
        auto value = static_cast<std::uint32_t>(random());
        for (int byte = 0; byte < 8; ++byte) {
            result += digits[value & 15];
            value >>= 4;
        }
    }
    return result;
}

void removeTree(const fs::path& path) noexcept {
    std::error_code error;
    fs::remove_all(path, error);
}

struct Storage {
    fs::path path;
    HANDLE lease = INVALID_HANDLE_VALUE;

    Storage() {
        const auto root = fs::temp_directory_path() / L"FileQuickTransfer";
        fs::create_directories(root);
        // The separate, delete-on-close lease avoids touching concurrent sessions.
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().wstring();
            if (name.size() != 32 || name.find_first_not_of(L"0123456789abcdef") != std::wstring::npos ||
                entry.is_symlink() || !entry.is_directory()) continue;
            const auto lockPath = root / (name + L".lock");
            auto lock = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (lock == INVALID_HANDLE_VALUE) continue;
            removeTree(entry.path());
            CloseHandle(lock);
        }
        const auto name = id();
        path = root / name;
        const auto lockPath = root / (name + ".lock");
        lease = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lease == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建临时存储锁");
        try {
            fs::create_directory(path);
        } catch (...) {
            CloseHandle(lease);
            throw;
        }
    }

    ~Storage() {
        removeTree(path);
        CloseHandle(lease);
    }
};

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string safeName(std::string_view text) {
    std::string name(trim(text));
    for (auto& ch : name) {
        if (static_cast<unsigned char>(ch) < 32 || std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos) ch = '_';
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) return "未命名文件";
    return name;
}

std::string relativePath(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string path;
    std::string_view remaining(text);
    for (;;) {
        const auto slash = remaining.find('/');
        const auto part = trim(remaining.substr(0, slash));
        if (!part.empty() && part != "." && part != "..") {
            if (!path.empty()) path += '/';
            path += safeName(part);
        }
        if (slash == std::string_view::npos) break;
        remaining.remove_prefix(slash + 1);
    }
    return path.empty() ? "未命名文件" : path;
}

std::string disposition(std::string_view name) {
    std::string header = "attachment; filename=\"download\"; filename*=UTF-8''";
    constexpr auto hex = "0123456789ABCDEF";
    for (unsigned char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
            header += static_cast<char>(ch);
        } else {
            header += '%';
            header += hex[ch >> 4];
            header += hex[ch & 15];
        }
    }
    return header;
}

void reply(Response& res, const Json& data, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(data.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json; charset=utf-8");
}

void fail(Response& res, int status, const char* message) {
    reply(res, {{"ok", false}, {"error", message}}, status);
}

std::string param(const Request& req, const char* key) {
    return std::string(trim(req.get_param_value(key)));
}

std::string field(const Json& msg, const char* key) {
    if (!msg.is_object()) return {};
    const auto found = msg.find(key);
    if (found == msg.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

struct SourceInfo {
    DWORD volume;
    DWORD indexHigh;
    DWORD indexLow;
    std::uint64_t size;
    std::uint64_t modified;
};

struct SourceHandle {
    explicit SourceHandle(const fs::path& path, bool managed = false)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | (managed ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~SourceHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    SourceHandle(const SourceHandle&) = delete;
    SourceHandle& operator=(const SourceHandle&) = delete;

    std::optional<SourceInfo> info() const {
        BY_HANDLE_FILE_INFORMATION data{};
        if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle, &data) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return std::nullopt;
        return SourceInfo{data.dwVolumeSerialNumber, data.nFileIndexHigh, data.nFileIndexLow,
                          (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow,
                          (std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime};
    }

    const HANDLE handle;
};

bool matches(const SourceInfo& expected, const std::optional<SourceInfo>& actual) {
    return actual && expected.volume == actual->volume && expected.indexHigh == actual->indexHigh &&
           expected.indexLow == actual->indexLow && expected.size == actual->size && expected.modified == actual->modified;
}

struct File {
    std::string id = ::id();
    std::string relative;
    std::string name;
    fs::path path;
    std::uint64_t size = 0;
    std::optional<SourceInfo> source;
};

struct Selection {
    File file;
    Clock::time_point expires;
};

struct Batch {
    const std::string id = ::id();
    const fs::path directory;
    const std::string room;
    const bool original;
    std::vector<File> files;
    std::string display;
    std::string sender;
    std::uint64_t size = 0;
    Clock::time_point uploaded;

    Batch(const fs::path& storage, std::string roomCode, bool originalFiles = false)
        : directory(originalFiles ? fs::path{} : storage / id), room(std::move(roomCode)), original(originalFiles) {
        if (!original) fs::create_directory(directory);
    }

    ~Batch() { if (!original) removeTree(directory); }

    Json json() const {
        auto ids = Json::array();
        auto entries = Json::array();
        for (const auto& file : files) {
            ids.push_back(file.id);
            entries.push_back({{"id", file.id}, {"name", file.name}, {"relPath", file.relative}, {"size", file.size}});
        }
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(uploaded.time_since_epoch()).count();
        return {{"id", id}, {"scope", room.empty() ? "broadcast" : "room"}, {"roomCode", room},
                {"displayName", display}, {"fileCount", files.size()}, {"totalSize", size},
                {"senderName", sender}, {"uploadedAt", time}, {"fileIds", std::move(ids)},
                {"firstFileId", files.front().id}, {"files", std::move(entries)}, {"original", original}};
    }
};

void makeUniquePath(File& file, std::set<std::string>& usedPaths) {
    const auto path = file.relative;
    const auto start = path.find_last_of('/') + 1;
    auto extension = path.find_last_of('.');
    if (extension == std::string::npos || extension <= start) extension = path.size();
    for (int duplicate = 1; !usedPaths.insert(file.relative).second; ++duplicate) {
        file.relative = path.substr(0, extension) + " (" + std::to_string(duplicate) + ')' + path.substr(extension);
    }
    file.name = file.relative.substr(file.relative.find_last_of('/') + 1);
}

std::string displayName(const Batch& batch, const std::string& topName) {
    if (!topName.empty()) return safeName(topName);
    if (batch.files.size() == 1) return batch.files.front().relative;
    return "多文件传输（" + std::to_string(batch.files.size()) + " 个文件）";
}

constexpr auto missingRoom = "接收码不存在或已失效";
constexpr auto missingFile = "文件不存在或已被下载";

std::string addressUrl(const std::string& address, int port) {
    return "http://" + address + ':' + std::to_string(port);
}

struct Peer {
    std::string id;
    std::string name;
    std::set<std::string> rooms;
};

class Transfer {
public:
    Transfer(std::string overrideIp, int boundPort) : overrideIp_(std::move(overrideIp)), port_(boundPort) {}

    void addRoutes(httplib::Server& server);
    void sweep();

private:
    Storage storage_;
    const std::string overrideIp_;
    const int port_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Batch>> batches_;
    std::unordered_map<std::string, Clock::time_point> rooms_;
    std::unordered_map<std::string, std::weak_ptr<Batch>> fileBatches_;
    std::unordered_map<std::string, Selection> selections_;
    std::mutex pickerMutex_;

    bool touchRoom(const std::string& code);
    Json filesLocked(const std::string& room);
    Json network();
    void upload(const Request& req, Response& res, const httplib::ContentReader& reader);
    void download(const Request& req, Response& res, bool zip);
    void selectOnHost(const Request& req, Response& res);
    bool hostRequest(const Request& req, Response& res) const;
    void shareFromHost(const Request& req, Response& res);
    void deleteSharedFiles(const Request& req, Response& res);
    void registerLocked(const std::shared_ptr<Batch>& batch);
    void eraseLocked(const Batch& batch);
    void publishLocked(const Json& message, const std::string& room);
    void handleSocket(const Request& req, httplib::ws::WebSocket& socket);
    Json devicesLocked();
    std::unordered_map<httplib::ws::WebSocket*, Peer> peers_;
};

bool Transfer::touchRoom(const std::string& code) {
    const auto found = rooms_.find(code);
    if (found == rooms_.end()) return false;
    const auto now = Clock::now();
    if (found->second + 2h <= now) return false;
    found->second = now;
    return true;
}

Json Transfer::filesLocked(const std::string& room) {
    std::vector<const Batch*> files;
    for (const auto& [key, batch] : batches_) {
        if (batch->room == room) files.push_back(batch.get());
    }
    std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->uploaded < b->uploaded; });
    auto result = Json::array();
    for (const auto* batch : files) result.push_back(batch->json());
    return result;
}

Json Transfer::network() {
    const auto addresses = discoverAddresses();
    auto urls = Json::array();
    if (!overrideIp_.empty() && std::none_of(addresses.begin(), addresses.end(), [&](const auto& item) {
        return item.ip == overrideIp_;
    })) {
        urls.push_back({{"url", addressUrl(overrideIp_, port_)}, {"name", "手动指定"}});
    }
    for (const auto& address : addresses) {
        urls.push_back({{"url", addressUrl(address.ip, port_)}, {"name", address.name}});
    }
    const auto preferred = !overrideIp_.empty() ? overrideIp_ : addresses.empty() ? "127.0.0.1" : addresses.front().ip;
    return {{"ok", true}, {"urls", std::move(urls)}, {"preferredUrl", addressUrl(preferred, port_)}};
}

bool Transfer::hostRequest(const Request& req, Response& res) const {
    const auto name = std::string("127.0.0.1:") + std::to_string(port_);
    const auto localName = std::string("localhost:") + std::to_string(port_);
    const auto host = req.get_header_value("Host");
    const auto origin = req.get_header_value("Origin");
    const bool loopbackHost = host == name || host == localName || host == "[::1]:" + std::to_string(port_);
    if ((req.remote_addr != "127.0.0.1" && req.remote_addr != "::1") || !loopbackHost ||
        (!origin.empty() && origin != "http://" + host)) {
        fail(res, 403, "此操作仅限主机本机网页");
        return false;
    }
    if (req.method == "POST" && (!origin.size() || req.get_header_value("Content-Type") != "application/json")) {
        fail(res, 403, "请在主机网页中进行此操作");
        return false;
    }
    return true;
}

void Transfer::selectOnHost(const Request& req, Response& res) {
    if (!hostRequest(req, res)) return;
    const auto data = Json::parse(req.body, nullptr, false);
    if (!data.is_object() || !data.contains("folder") || !data["folder"].is_boolean()) {
        return fail(res, 400, "请求格式错误");
    }
    const std::unique_lock picker(pickerMutex_, std::try_to_lock);
    if (!picker.owns_lock()) return fail(res, 409, "请先关闭已打开的主机文件选择窗口");

    auto chosen = pickNativeFiles(data["folder"].get<bool>());
    if (chosen.size() > 10000) return fail(res, 400, "一次最多可选择 10000 个文件");
    auto files = Json::array();
    std::vector<Selection> pending;
    const auto expires = Clock::now() + 2h;
    for (auto& item : chosen) {
        File file;
        file.path = std::move(item.path);
        file.relative = relativePath(std::move(item.relative));
        file.name = file.relative.substr(file.relative.find_last_of('/') + 1);
        file.source = SourceHandle(file.path).info();
        if (!file.source) return fail(res, 400, "无法读取所选文件，请确认文件可用后重新选择");
        file.size = file.source->size;
        files.push_back({{"token", file.id}, {"name", file.name}, {"relPath", file.relative}, {"size", file.size}});
        pending.push_back({std::move(file), expires});
    }
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        for (auto entry = selections_.begin(); entry != selections_.end();) {
            if (entry->second.expires <= now) entry = selections_.erase(entry);
            else ++entry;
        }
        if (selections_.size() + pending.size() > 20000) return fail(res, 429, "待发送文件过多，请先发送已选择的文件");
        for (auto& item : pending) {
            auto token = item.file.id;
            selections_.emplace(std::move(token), std::move(item));
        }
    }
    reply(res, {{"ok", true}, {"files", std::move(files)}});
}

void Transfer::shareFromHost(const Request& req, Response& res) {
    if (!hostRequest(req, res)) return;
    const auto data = Json::parse(req.body, nullptr, false);
    if (!data.is_object() || !data.contains("tokens") || !data["tokens"].is_array() ||
        data["tokens"].empty() || data["tokens"].size() > 10000) {
        return fail(res, 400, "请选择要发送的文件");
    }
    const auto room = param(req, "mode") == "room" ? param(req, "code") : std::string{};
    if (param(req, "mode") == "room" && room.empty()) return fail(res, 404, missingRoom);
    auto batch = std::make_shared<Batch>(storage_.path, room, true);
    batch->sender = field(data, "senderName");
    auto topName = field(data, "topName");
    if (batch->sender.size() > 8192 || topName.size() > 8192) return fail(res, 400, "请求格式错误");
    std::set<std::string> usedTokens;
    std::set<std::string> usedPaths;
    std::lock_guard lock(mutex_);
    if (!room.empty() && !touchRoom(room)) return fail(res, 404, missingRoom);
    const auto now = Clock::now();
    for (const auto& token : data["tokens"]) {
        if (!token.is_string()) return fail(res, 400, "请求格式错误");
        const auto value = token.get<std::string>();
        if (!usedTokens.insert(value).second) continue;
        const auto found = selections_.find(value);
        if (found == selections_.end() || found->second.expires <= now) {
            return fail(res, 400, "文件选择已失效，请重新选择");
        }
        auto file = found->second.file;
        if (!matches(*file.source, SourceHandle(file.path).info())) {
            return fail(res, 409, "所选文件已移动或改动，请重新选择");
        }
        makeUniquePath(file, usedPaths);
        batch->size += file.size;
        batch->files.push_back(std::move(file));
    }
    batch->uploaded = now;
    batch->display = displayName(*batch, topName);
    registerLocked(batch);
    for (const auto& token : usedTokens) selections_.erase(token);
    reply(res, {{"ok", true}, {"batchId", batch->id}, {"displayName", batch->display}, {"fileCount", batch->files.size()}});
}

void Transfer::deleteSharedFiles(const Request& req, Response& res) {
    if (!hostRequest(req, res)) return;
    const auto data = Json::parse(req.body, nullptr, false);
    if (!data.is_object() || !data.contains("ids") || !data["ids"].is_array() ||
        data["ids"].size() > 10000) return fail(res, 400, "请求格式错误");
    std::set<std::string> ids;
    for (const auto& item : data["ids"]) {
        if (!item.is_string()) return fail(res, 400, "请求格式错误");
        ids.insert(item.get<std::string>());
    }
    std::size_t deleted = 0;
    std::size_t failed = 0;
    std::lock_guard lock(mutex_);
    for (const auto& key : ids) {
        const auto found = fileBatches_.find(key);
        if (found == fileBatches_.end()) continue;
        const auto batch = found->second.lock();
        if (!batch) continue;
        const auto file = std::find_if(batch->files.begin(), batch->files.end(), [&](const auto& item) { return item.id == key; });
        if (!batch->original) {
            std::error_code error;
            const bool removed = fs::remove(file->path, error);
            if (error || !removed) { ++failed; continue; }
        }
        ++deleted;
        const auto event = Json{{"type", "file-removed"}, {"batchId", batch->id}};
        if (batch->files.size() == 1) {
            eraseLocked(*batch);
            publishLocked(event, batch->room);
        } else {
            batch->size -= file->size;
            fileBatches_.erase(found);
            batch->files.erase(file);
            auto partial = event;
            partial["fileId"] = key;
            publishLocked(partial, batch->room);
        }
    }
    reply(res, {{"ok", true}, {"deleted", deleted}, {"failed", failed}});
}

void Transfer::registerLocked(const std::shared_ptr<Batch>& batch) {
    for (const auto& file : batch->files) fileBatches_.emplace(file.id, batch);
    batches_.emplace(batch->id, batch);
    publishLocked({{"type", "file-arrived"}, {"batch", batch->json()}}, batch->room);
}

void Transfer::upload(const Request& req, Response& res, const httplib::ContentReader& reader) {
    const bool isRoom = param(req, "mode") == "room";
    const auto room = isRoom ? param(req, "code") : std::string{};
    if (isRoom) {
        std::lock_guard lock(mutex_);
        if (!touchRoom(room)) return fail(res, 404, missingRoom);
    }
    if (!req.is_multipart_form_data()) return fail(res, 400, "请求格式错误");

    const auto batch = std::make_shared<Batch>(storage_.path, room);
    std::ofstream output;
    std::vector<std::string> paths;
    std::string topName;
    std::string part;
    bool diskError = false;
    const auto closeFile = [&] {
        if (!output.is_open()) return true;
        output.close();
        if (!output) diskError = true;
        return !diskError;
    };
    const bool read = reader(
        [&](const httplib::FormData& item) {
            if (!closeFile()) return false;
            part = item.name;
            if (part == "files") {
                auto& file = batch->files.emplace_back();
                file.name = item.filename;
                file.path = batch->directory / file.id;
                output.open(file.path, std::ios::binary);
                if (!output) diskError = true;
            } else if (part == "relativePath") {
                paths.emplace_back();
            }
            return !diskError;
        },
        [&](const char* bytes, std::size_t size) {
            if (part == "files") {
                output.write(bytes, static_cast<std::streamsize>(size));
                if (!output) diskError = true;
                batch->files.back().size += size;
                return !diskError;
            }
            std::string* text = nullptr;
            if (part == "relativePath") text = &paths.back();
            if (part == "topName") text = &topName;
            if (part == "senderName") text = &batch->sender;
            constexpr std::size_t maxFieldSize = 8192;
            if (text && size > maxFieldSize - text->size()) return false;
            if (text) text->append(bytes, size);
            return true;
        });
    closeFile();
    if (diskError) return fail(res, 500, "服务器写入失败");
    if (!read) return fail(res, res.status >= 400 ? res.status : 400, "读取上传数据失败");
    if (batch->files.empty()) return fail(res, 400, "没有收到文件");

    std::set<std::string> usedPaths;
    for (std::size_t i = 0; i < batch->files.size(); ++i) {
        auto& file = batch->files[i];
        file.relative = relativePath(i < paths.size() && !paths[i].empty() ? paths[i] : file.name);
        makeUniquePath(file, usedPaths);
        batch->size += file.size;
    }
    const auto count = batch->files.size();
    batch->display = displayName(*batch, topName);
    batch->uploaded = Clock::now();
    {
        std::lock_guard lock(mutex_);
        if (isRoom && !touchRoom(room)) return fail(res, 404, missingRoom);
        registerLocked(batch);
    }
    reply(res, {{"ok", true}, {"batchId", batch->id}, {"displayName", batch->display}, {"fileCount", count}});
}

void Transfer::eraseLocked(const Batch& batch) {
    for (const auto& file : batch.files) fileBatches_.erase(file.id);
    batches_.erase(batch.id);
}

void Transfer::download(const Request& req, Response& res, bool zip) {
    const auto key = req.matches[1].str();
    const bool probe = req.method == "HEAD";
    if (req.has_header("Range")) return fail(res, 416, "请重新下载完整文件");
    std::shared_ptr<Batch> batch;
    std::shared_ptr<SourceHandle> stream;
    fs::path archivePath;
    std::string filename;
    std::uint64_t size = 0;
    std::vector<std::pair<std::string, fs::path>> entries;
    {
        std::lock_guard lock(mutex_);
        if (zip) {
            const auto found = batches_.find(key);
            if (found == batches_.end() || found->second->original) return fail(res, 404, missingFile);
            batch = found->second;
            filename = safeName(batch->display) + ".zip";
            if (!probe) {
                archivePath = batch->directory / (id() + ".zip");
                for (const auto& file : batch->files) entries.emplace_back(file.relative, file.path);
            }
        } else {
            const auto found = fileBatches_.find(key);
            if (found != fileBatches_.end()) batch = found->second.lock();
            if (!batch) return fail(res, 404, missingFile);
            const auto file = std::find_if(batch->files.begin(), batch->files.end(), [&](const auto& item) {
                return item.id == key;
            });
            stream = std::make_shared<SourceHandle>(file->path, !batch->original);
            const auto info = stream->info();
            if (file->source && !matches(*file->source, info)) return fail(res, 409, "主机原文件已移动、改动或被占用，请联系发送方重新发送");
            if (!info) return fail(res, 404, missingFile);
            filename = file->name;
            size = info->size;
        }
    }

    try {
        const auto mime = zip ? "application/zip" : "application/octet-stream";
        res.set_header("Content-Disposition", disposition(filename));
        res.set_header("Cache-Control", "no-store");
        if (probe) {
            res.set_header("Content-Type", mime);
            if (!zip) res.set_header("Content-Length", std::to_string(size));
            res.status = 200;
            return;
        }
        if (zip) {
            makeZip(archivePath, entries);
            stream = std::make_shared<SourceHandle>(archivePath, true);
            const auto info = stream->info();
            if (!info) throw std::runtime_error("打开归档失败");
            size = info->size;
        }
        const auto complete = [batch, archivePath](bool) {
            if (!archivePath.empty()) removeTree(archivePath);
        };
        if (size == 0) {
            res.set_header("Content-Length", "0");
            res.set_content_provider(mime, [](std::size_t, httplib::DataSink& sink) {
                sink.done();
                return true;
            }, complete);
            return;
        }
        res.set_content_provider(static_cast<std::size_t>(size), mime,
            [stream](std::size_t offset, std::size_t remaining, httplib::DataSink& sink) {
                std::array<char, 64 * 1024> buffer;
                std::size_t transferred = 0;
                while (transferred < remaining) {
                    const auto requested = static_cast<DWORD>(std::min(remaining - transferred, buffer.size()));
                    DWORD received = 0;
                    if (!ReadFile(stream->handle, buffer.data(), requested, &received, nullptr) || received == 0 ||
                        !sink.write(buffer.data(), received)) return false;
                    transferred += received;
                }
                return true;
            }, complete);
    } catch (...) {
        if (!archivePath.empty()) removeTree(archivePath);
        throw;
    }
}

void Transfer::sweep() {
    std::vector<std::shared_ptr<Batch>> expired;
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    for (auto room = rooms_.begin(); room != rooms_.end();) {
        if (room->second + 2h > now) { ++room; continue; }
        for (auto& [socket, peer] : peers_) peer.rooms.erase(room->first);
        room = rooms_.erase(room);
    }
    for (const auto& [key, batch] : batches_) {
        if (!batch->original && batch->uploaded + 24h <= now) expired.push_back(batch);
    }
    for (const auto& batch : expired) {
        eraseLocked(*batch);
        publishLocked({{"type", "file-removed"}, {"batchId", batch->id}}, batch->room);
    }
    for (auto entry = selections_.begin(); entry != selections_.end();) {
        if (entry->second.expires <= now) entry = selections_.erase(entry);
        else ++entry;
    }
}

Json Transfer::devicesLocked() {
    auto devices = Json::array();
    std::set<std::string> listed;
    for (const auto& [socket, peer] : peers_) {
        if (!peer.id.empty() && listed.insert(peer.id).second) {
            devices.push_back({{"deviceId", peer.id}, {"name", peer.name}});
        }
    }
    return devices;
}

void Transfer::publishLocked(const Json& message, const std::string& room) {
    const auto text = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    for (const auto& [socket, peer] : peers_) {
        if (room.empty() || peer.rooms.count(room)) socket->send(text);
    }
}

void Transfer::handleSocket(const Request& req, httplib::ws::WebSocket& socket) {
    const auto origin = req.get_header_value("Origin");
    if (!origin.empty() && origin != "http://" + req.get_header_value("Host")) {
        socket.close(httplib::ws::CloseStatus::PolicyViolation);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        peers_.try_emplace(&socket);
    }
    const auto broadcastDevices = [this] {
        const auto message = Json{{"type", "devices"}, {"devices", devicesLocked()}}.dump();
        for (const auto& [client, peer] : peers_) client->send(message);
    };
    try {
        std::string text;
        while (!stopping && socket.read(text)) {
            const auto message = Json::parse(text, nullptr, false);
            const auto type = field(message, "type");
            std::lock_guard lock(mutex_);
            auto& peer = peers_.at(&socket);
            if (type == "hello") {
                auto deviceId = field(message, "deviceId");
                auto name = field(message, "deviceName");
                peer.id = deviceId.empty() || deviceId.size() > 128 ? id() : std::move(deviceId);
                peer.name = name.empty() || name.size() > 512 ? "我的设备" : std::move(name);
                socket.send(Json{{"type", "welcome"}, {"deviceId", peer.id}, {"devices", devicesLocked()}}.dump());
                broadcastDevices();
            } else if (type == "join-room") {
                const auto code = std::string(trim(field(message, "code")));
                if (touchRoom(code)) {
                    peer.rooms.insert(code);
                    socket.send(Json{{"type", "room-joined"}, {"ok", true}, {"code", code}, {"files", filesLocked(code)}}
                                    .dump(-1, ' ', false, Json::error_handler_t::replace));
                } else {
                    socket.send(Json{{"type", "room-joined"}, {"ok", false}, {"error", missingRoom}}.dump());
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    {
        std::lock_guard lock(mutex_);
        peers_.erase(&socket);
        broadcastDevices();
    }
}

void Transfer::addRoutes(httplib::Server& server) {
    const auto serve = [](std::string_view asset, const char* mime) {
        return [asset, mime](const Request&, Response& res) {
            res.set_header("Cache-Control", "no-cache");
            res.set_content(asset.data(), asset.size(), mime);
        };
    };
    server.Get("/", serve(assets::index, "text/html; charset=utf-8"));
    server.Get("/index.html", serve(assets::index, "text/html; charset=utf-8"));
    server.Get("/app.js", serve(assets::script, "text/javascript; charset=utf-8"));
    server.Get("/style.css", serve(assets::style, "text/css; charset=utf-8"));
    server.Get("/api/network", [this](const Request&, Response& res) { reply(res, network()); });
    server.Get("/api/qr", [this](const Request& req, Response& res) {
        const auto info = network();
        const auto ip = param(req, "address");
        auto url = info.at("preferredUrl").get<std::string>();
        if (!ip.empty()) {
            const auto requested = addressUrl(ip, port_);
            const auto& urls = info.at("urls");
            if (requested != url && std::none_of(urls.begin(), urls.end(), [&](const auto& item) {
                return item.at("url") == requested;
            })) return fail(res, 400, "地址不可用");
            url = requested;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(makeQrPng(url), "image/png");
    });
    server.Post("/api/upload", [this](const Request& req, Response& res, const httplib::ContentReader& reader) {
        upload(req, res, reader);
    });
    server.Get("/api/broadcast/files", [this](const Request&, Response& res) {
        std::lock_guard lock(mutex_);
        reply(res, {{"ok", true}, {"files", filesLocked({})}});
    });
    server.Post("/api/room/create", [this](const Request& req, Response& res) {
        const auto previousCode = param(req, "exclude");
        sweep();
        std::lock_guard lock(mutex_);
        if (rooms_.size() == 10000) return fail(res, 503, "接收码暂时用完");
        std::random_device random;
        const auto start = std::uniform_int_distribution<int>(0, 9999)(random);
        for (int offset = 0; offset < 10000; ++offset) {
            const auto code = std::to_string(10000 + (start + offset) % 10000).substr(1);
            if (code != previousCode && rooms_.try_emplace(code, Clock::now()).second) {
                return reply(res, {{"ok", true}, {"code", code}});
            }
        }
        fail(res, 503, "接收码暂时用完");
    });
    const auto join = [this](const Request& req, Response& res) {
        auto code = param(req, "code");
        if (code.empty() && req.body.size() <= 65536) code = trim(field(Json::parse(req.body, nullptr, false), "code"));
        std::lock_guard lock(mutex_);
        if (!touchRoom(code)) return fail(res, 404, missingRoom);
        reply(res, {{"ok", true}, {"code", code}, {"files", filesLocked(code)}});
    };
    server.Post("/api/room/join", join);
    server.Get("/api/room/join", join);
    server.Get("/api/room/files", join);
    server.Get(R"(/api/download/([0-9a-f]{32}))", [this](const Request& req, Response& res) { download(req, res, false); });
    server.Post("/api/host/firewall", [this](const Request& req, Response& res) {
        if (!hostRequest(req, res)) return;
        const auto data = Json::parse(req.body, nullptr, false);
        if (!data.is_object() || !data.empty()) return fail(res, 400, "请求格式错误");
        static std::mutex approvalMutex;
        const std::unique_lock approval(approvalMutex, std::try_to_lock);
        if (!approval.owns_lock()) return fail(res, 409, "请先处理 Windows 管理员确认窗口");
        switch (allowLanAccess(port_)) {
        case FirewallResult::allowed:
            return reply(res, {{"ok", true}});
        case FirewallResult::cancelled:
            return fail(res, 409, "已取消 Windows 管理员确认");
        case FirewallResult::failed:
            return fail(res, 500, "无法设置防火墙规则，请检查 Windows 防火墙或设备管理策略");
        }
    });
    server.Post("/api/host/select", [this](const Request& req, Response& res) { selectOnHost(req, res); });
    server.Post("/api/host/share", [this](const Request& req, Response& res) { shareFromHost(req, res); });
    server.Post("/api/host/uploads/delete", [this](const Request& req, Response& res) { deleteSharedFiles(req, res); });
    server.Get(R"(/api/download-batch/([0-9a-f]{32}))", [this](const Request& req, Response& res) { download(req, res, true); });
    server.WebSocket("/ws", [this](const Request& req, httplib::ws::WebSocket& socket) { handleSocket(req, socket); });
    server.set_exception_handler([](const Request&, Response& res, std::exception_ptr error) {
        try { std::rethrow_exception(error); }
        catch (const std::exception& err) { std::cerr << err.what() << '\n'; }
        catch (...) { std::cerr << "服务处理失败\n"; }
        fail(res, 500, "服务处理失败");
    });
}
}

int runServer(int preferredPort, const std::vector<NetworkAddress>& addresses,
              const std::string& advertisedIp, bool openBrowser) {
    LanServer server;
    server.set_socket_options([](socket_t socket) {
        const BOOL exclusive = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    });
    server.set_address_family(AF_INET);
    // cpp-httplib also compares chunked bodies directly against this value.
    server.set_payload_max_length((std::numeric_limits<std::size_t>::max)());
    server.set_read_timeout(30);
    server.set_write_timeout(15);
    server.set_keep_alive_timeout(2);
    server.new_task_queue = [] { return new httplib::ThreadPool(8, 64, 128); };
    int port = -1;
    if (preferredPort == 0) {
        port = server.bind_to_any_port("0.0.0.0");
    } else {
        const auto first = preferredPort > 0 ? preferredPort : 3000;
        const auto last = preferredPort > 0 ? preferredPort : 4000;
        for (int candidate = first; candidate <= last; ++candidate) {
            if (server.bind_to_port("0.0.0.0", candidate)) { port = candidate; break; }
        }
    }
    if (port <= 0) {
        std::cerr << "无法监听端口 " << (preferredPort < 0 ? "3000–4000" : std::to_string(preferredPort)) << '\n';
        return 1;
    }
    Transfer transfer(advertisedIp, port);
    transfer.addRoutes(server);
    announceServer(port, addresses, advertisedIp, openBrowser);

    stopping = false;
    const bool handlesControl = SetConsoleCtrlHandler(consoleControl, TRUE) != FALSE;
    std::atomic<bool> done{false};
    std::thread maintenance([&] {
        auto nextSweep = std::chrono::steady_clock::now() + 1h;
        while (!done) {
            if (stopping && server.is_running()) {
                server.stopTransfers();
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSweep) {
                transfer.sweep();
                nextSweep = now + 1h;
            }
            std::this_thread::sleep_for(100ms);
        }
    });
    bool listening = false;
    try {
        listening = server.listen_after_bind();
    } catch (...) {
        done = true;
        maintenance.join();
        if (handlesControl) SetConsoleCtrlHandler(consoleControl, FALSE);
        throw;
    }
    done = true;
    maintenance.join();
    if (handlesControl) SetConsoleCtrlHandler(consoleControl, FALSE);
    return listening || stopping ? 0 : 1;
}
