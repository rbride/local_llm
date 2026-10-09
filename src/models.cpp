#include "models.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>

#include "util.hpp"

namespace {

bool is_dir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

std::vector<std::string> list_models(const std::string& root, size_t max_count) {
    std::vector<std::string> out;
    DIR* d = ::opendir(root.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.size() < 2 || name[0] == '.') continue;  // skip ".", "..", .cache, .current_model
        if (!is_dir(root + "/" + name)) continue;          // ignore loose files (stray .gguf, etc.)
        if (!is_regular(root + "/" + name + "/server.args")) continue;  // not a model folder
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    if (out.size() > max_count) out.resize(max_count);
    return out;
}

std::string current_model_path(const std::string& root) { return root + "/.current_model"; }

std::string read_current_model(const std::string& root) {
    std::ifstream f(current_model_path(root));
    if (!f) return "";
    std::string line;
    std::getline(f, line);
    return trim(line);
}

bool write_current_model(const std::string& root, const std::string& name) {
    std::ofstream f(current_model_path(root), std::ios::trunc);
    if (!f) return false;
    f << name << "\n";
    f.flush();
    return static_cast<bool>(f);
}
