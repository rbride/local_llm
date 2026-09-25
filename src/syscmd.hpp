// syscmd.hpp - run a program (no shell unless you ask for one) and restart the bot in place.
#pragma once
#include <string>
#include <utility>
#include <vector>

struct CmdResult {
    int exit_code = -1;
    std::string output;  // stdout + stderr, capped at 64 KB
    bool timed_out = false;
    bool ok() const { return exit_code == 0 && !timed_out; }
};

CmdResult run_cmd(const std::vector<std::string>& argv, const std::string& cwd, int timeout_s);

// Remember how we were started, so we can exec ourselves again later.
void remember_startup(int argc, char** argv);
std::string self_exe_path();
std::string self_exe_dir();
// Replace this process with a fresh copy of the bot. Only returns if exec failed.
bool restart_self(const std::vector<std::pair<std::string, std::string>>& env);
