#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

std::string makeQrPng(const std::string& text);
std::string makeConsoleQr(const std::string& text);
void makeZip(const std::filesystem::path& output,
             const std::vector<std::pair<std::string, std::filesystem::path>>& files);
