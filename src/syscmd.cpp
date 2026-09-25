#include "syscmd.hpp"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace {
std::vector<std::string> g_args;
std::string g_exe;
}  // namespace

CmdResult run_cmd(const std::vector<std::string>& argv, const std::string& cwd, int timeout_s) {
    CmdResult res;
    if (argv.empty()) return res;
    int fds[2];
    if (pipe(fds) != 0) { res.output = "pipe failed"; return res; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); res.output = "fork failed"; return res; }
    if (pid == 0) {
        setpgid(0, 0);  // own process group, so a timeout kills children too
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) { perror("chdir"); _exit(127); }
        setenv("GIT_TERMINAL_PROMPT", "0", 1);                 // never wait for a password
        setenv("GIT_SSH_COMMAND", "ssh -o BatchMode=yes", 0);  // or an ssh prompt
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        std::fprintf(stderr, "can't run %s: %s\n", args[0], std::strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    long long deadline = now_ms() + timeout_s * 1000LL;
    char buf[4096];
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        int left = static_cast<int>(deadline - now_ms());
        if (left <= 0) {
            kill(-pid, SIGKILL);
            res.timed_out = true;
            break;
        }
        int pr = poll(&p, 1, std::min(left, 500));
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        if (res.output.size() < 65536) res.output.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

void remember_startup(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) g_args.push_back(argv[i]);
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) g_exe.assign(buf, static_cast<size_t>(n));
    else if (argc > 0) {
        char* rp = realpath(argv[0], nullptr);
        if (rp) { g_exe = rp; free(rp); }
    }
}

std::string self_exe_path() { return g_exe; }

std::string self_exe_dir() {
    auto slash = g_exe.find_last_of('/');
    return slash == std::string::npos ? "." : g_exe.substr(0, slash);
}

bool restart_self(const std::vector<std::pair<std::string, std::string>>& env) {
    if (g_exe.empty()) return false;
    for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
    std::vector<char*> args;
    for (auto& a : g_args) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    // Close everything except stdio so the new copy starts clean.
    for (int fd = 3; fd < 1024; ++fd) fcntl(fd, F_SETFD, FD_CLOEXEC);
    execv(g_exe.c_str(), args.data());
    log(std::string("exec failed: ") + std::strerror(errno));
    return false;
}
