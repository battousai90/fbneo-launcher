// src/RomCleanup.cpp
#include "RomCleanup.h"
#include <zip.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace RomCleanup {
namespace {
void log(const RomInbox::Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}
} // namespace

bool extract_entries_to_quarantine(const std::string& zip_path,
                                    const std::vector<std::string>& entries,
                                    const std::string& quarantine_dir,
                                    const RomInbox::Callbacks& cb) {
    int err = 0;
    zip_t* za = zip_open(zip_path.c_str(), 0, &err);
    if (!za) {
        log(cb, "Could not open " + zip_path);
        return false;
    }

    fs::path dest_dir = fs::path(quarantine_dir) / "_extra_files" / fs::path(zip_path).stem();
    std::error_code mkec;
    fs::create_directories(dest_dir, mkec);

    bool ok = true;
    for (const auto& name : entries) {
        zip_int64_t idx = zip_name_locate(za, name.c_str(), 0);
        if (idx < 0) { ok = false; continue; }

        zip_file_t* zf = zip_fopen_index(za, idx, 0);
        if (!zf) { ok = false; continue; }

        fs::path dest = dest_dir / fs::path(name).filename();
        std::error_code pec;
        fs::create_directories(dest.parent_path(), pec);
        std::ofstream out(dest, std::ios::binary);
        char buf[65536];
        zip_int64_t n;
        bool write_ok = out.good();
        while (write_ok && (n = zip_fread(zf, buf, sizeof(buf))) > 0)
            write_ok = (bool)out.write(buf, n);
        out.close();
        zip_fclose(zf);

        if (!write_ok || zip_delete(za, idx) != 0) ok = false;
    }
    if (zip_close(za) != 0) ok = false;

    if (!ok) log(cb, "Failed to fully clean " + zip_path);
    return ok;
}

} // namespace RomCleanup
