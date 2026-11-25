#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct NativeFile {
    std::filesystem::path path;
    std::string relative;
};

// Opens a native Windows picker synchronously. When folder is true, returns
// regular files below the selected folder with paths relative to that folder.
std::vector<NativeFile> pickNativeFiles(bool folder);
