#include "native_files.hpp"

#ifndef _WIN32
#error "Native file picker is available only on Windows"
#endif

#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>

#include <system_error>
#include <stdexcept>
#include <memory>
#include <vector>

namespace {

class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            throw std::runtime_error("CoInitializeEx failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
        }
        initialized_ = true;
    }
    ~ComApartment() { if (initialized_) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
private:
    bool initialized_ = false;
};

template<class T>
class ComPtr {
public:
    ~ComPtr() { if (ptr_) ptr_->Release(); }
    T** put() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } return &ptr_; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr() = default;
private:
    T* ptr_ = nullptr;
};

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("Failed to convert path to UTF-8");
    std::string result(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr) != n)
        throw std::runtime_error("Failed to convert path to UTF-8");
    return result;
}

std::wstring shellPath(IShellItem* item) {
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (FAILED(hr)) throw std::runtime_error("Could not obtain selected filesystem path");
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::vector<NativeFile> enumerateFolder(const std::filesystem::path& root) {
    std::vector<NativeFile> result;
    std::error_code ec;
    const auto rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES || (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("请选择实际文件夹");
    }
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec), end;
    if (ec) throw std::runtime_error("Could not enumerate selected folder: " + ec.message());
    const auto folderName = root.filename();
    while (it != end) {
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("无法读取文件夹内容");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            it.disable_recursion_pending();
        } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const auto rel = folderName / it->path().lexically_relative(root);
            result.push_back({it->path(), toUtf8(rel.generic_wstring())});
            if (result.size() > 10000) throw std::runtime_error("一次最多可选择 10000 个文件");
        }
        it.increment(ec);
        if (ec) throw std::runtime_error("Could not continue folder enumeration: " + ec.message());
    }
    return result;
}
}

std::vector<NativeFile> pickNativeFiles(bool folder) {
    ComApartment apartment;
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr)) throw std::runtime_error("Could not create native file picker");

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (FAILED(hr)) throw std::runtime_error("Could not configure native file picker");
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        options |= FOS_PICKFOLDERS;
    } else {
        options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filters[] = {{L"All files (*.*)", L"*.*"}};
        hr = dialog->SetFileTypes(1, filters);
        if (FAILED(hr)) throw std::runtime_error("Could not configure native file picker filters");
    }
    hr = dialog->SetOptions(options);
    if (FAILED(hr)) throw std::runtime_error("Could not configure native file picker");

    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(hr)) throw std::runtime_error("Native file picker failed");

    if (folder) {
        ComPtr<IShellItem> selection;
        hr = dialog->GetResult(selection.put());
        if (FAILED(hr)) throw std::runtime_error("Could not retrieve selected folder");
        const std::filesystem::path root(shellPath(selection.get()));
        return enumerateFolder(root);
    }

    ComPtr<IShellItemArray> selections;
    hr = dialog->GetResults(selections.put());
    if (FAILED(hr)) throw std::runtime_error("Could not retrieve selected files");
    DWORD count = 0;
    hr = selections->GetCount(&count);
    if (FAILED(hr)) throw std::runtime_error("Could not count selected files");
    std::vector<NativeFile> result;
    result.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        hr = selections->GetItemAt(i, item.put());
        if (FAILED(hr)) throw std::runtime_error("Could not retrieve selected file");
        std::filesystem::path path(shellPath(item.get()));
        result.push_back({path, toUtf8(path.filename().generic_wstring())});
    }
    return result;
}
