#include "formats.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <io.h>
#include <sys/stat.h>

#include <miniz.h>
#include <qrcodegen.hpp>

namespace {

constexpr int quietZone = 4;

qrcodegen::QrCode encodeQr(const std::string& text) {
    if (std::all_of(text.begin(), text.end(), [](unsigned char byte) { return byte > 0 && byte < 128; })) {
        return qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    }

    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return qrcodegen::QrCode::encodeSegments(
        {qrcodegen::QrSegment::makeEci(26), qrcodegen::QrSegment::makeBytes(bytes)},
        qrcodegen::QrCode::Ecc::MEDIUM);
}

std::FILE* openFile(const std::filesystem::path& path, bool write) {
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
}

void checkZip(mz_zip_archive& zip, mz_bool success, const std::string& operation) {
    if (!success) {
        throw std::runtime_error(operation + ": " + mz_zip_get_error_string(mz_zip_peek_last_error(&zip)));
    }
}

struct ZipWriter {
    mz_zip_archive archive{};

    ~ZipWriter() {
        if (archive.m_pState != nullptr) {
            mz_zip_writer_end(&archive);
        }
    }
};

} // namespace

std::string makeQrPng(const std::string& text) {
    const auto qr = encodeQr(text);
    constexpr int scale = 8;
    const int width = (qr.getSize() + 2 * quietZone) * scale;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * width, 255);
    for (int y = 0; y < width; ++y) {
        for (int x = 0; x < width; ++x) {
            if (qr.getModule(x / scale - quietZone, y / scale - quietZone)) {
                pixels[static_cast<std::size_t>(y) * width + x] = 0;
            }
        }
    }

    std::size_t size = 0;
    std::unique_ptr<void, decltype(&mz_free)> png(
        tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), width, width, 1, &size, 6, MZ_FALSE),
        mz_free);
    if (!png) {
        throw std::runtime_error("Could not encode QR code as PNG");
    }
    return std::string(static_cast<const char*>(png.get()), size);
}

std::string makeConsoleQr(const std::string& text) {
    const auto qr = encodeQr(text);
    std::string output;
    for (int y = -quietZone; y < qr.getSize() + quietZone; ++y) {
        bool black = false;
        output += "\033[107m";
        for (int x = -quietZone; x < qr.getSize() + quietZone; ++x) {
            const bool module = qr.getModule(x, y);
            if (module != black) {
                output += module ? "\033[40m" : "\033[107m";
                black = module;
            }
            output += "  ";
        }
        output += "\033[0m\n";
    }
    return output;
}

void makeZip(const std::filesystem::path& output,
             const std::vector<std::pair<std::string, std::filesystem::path>>& files) {
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    File destination(openFile(output, true), std::fclose);
    if (!destination) {
        throw std::runtime_error("Could not open ZIP output file");
    }

    try {
        ZipWriter writer;
        auto& zip = writer.archive;
        checkZip(zip, mz_zip_writer_init_cfile(&zip, destination.get(), MZ_ZIP_FLAG_WRITE_ZIP64),
                 "Could not initialize ZIP output");
        for (const auto& [name, path] : files) {
            if (name.empty() || name.find('\0') != std::string::npos || name.back() == '/') {
                throw std::runtime_error("Invalid ZIP entry name");
            }
            File source(openFile(path, false), std::fclose);
            if (!source) {
                throw std::runtime_error("Could not open ZIP input file: " + name);
            }
            struct _stat64 info{};
            if (_fstat64(_fileno(source.get()), &info) != 0 || (info.st_mode & _S_IFMT) != _S_IFREG) {
                throw std::runtime_error("Could not read ZIP input metadata: " + name);
            }
            const auto added = mz_zip_writer_add_cfile(
                &zip, name.c_str(), source.get(), static_cast<mz_uint64>(info.st_size), &info.st_mtime, nullptr, 0,
                MZ_NO_COMPRESSION | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE, nullptr, 0, nullptr, 0);
            checkZip(zip, added, "Could not add ZIP entry " + name);
            if (std::ferror(source.get()) || _ftelli64(source.get()) != info.st_size) {
                throw std::runtime_error("Could not read ZIP input file: " + name);
            }
        }
        checkZip(zip, mz_zip_writer_finalize_archive(&zip), "Could not finalize ZIP archive");
        checkZip(zip, mz_zip_writer_end(&zip), "Could not end ZIP archive");
        if (std::fclose(destination.release()) != 0) {
            throw std::runtime_error("Could not close ZIP output file");
        }
    } catch (...) {
        destination.reset();
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        throw;
    }
}
