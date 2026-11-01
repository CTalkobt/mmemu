#include "make_runner.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace project {

MakeResult runMake(const Config& cfg, const std::string& target,
                   const std::function<void(const std::string&)>& onLine,
                   const std::atomic<bool>* cancel) {
    MakeResult r;
    int fds[2];
    if (pipe(fds) != 0) { r.error = std::string("pipe: ") + strerror(errno); return r; }

    pid_t pid = fork();
    if (pid < 0) {
        r.error = std::string("fork: ") + strerror(errno);
        close(fds[0]); close(fds[1]);
        return r;
    }
    if (pid == 0) {
        // Child: own process group (so cancel can stop make's children too),
        // output to the pipe, project environment, then exec make.
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        for (const auto& [k, v] : cfg.env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv = {const_cast<char*>("make"), const_cast<char*>("-C"),
                                   const_cast<char*>(cfg.dir.c_str())};
        if (!target.empty()) argv.push_back(const_cast<char*>(target.c_str()));
        argv.push_back(nullptr);
        execvp("make", argv.data());
        dprintf(STDERR_FILENO, "mmemu: could not run make: %s\n", strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    r.started = true;

    std::string pending;
    char buf[4096];
    bool killed = false;
    while (true) {
        if (cancel && cancel->load() && !killed) {
            kill(-pid, SIGTERM);
            killed = true;
            r.cancelled = true;
        }
        struct pollfd pfd = {fds[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;   // EOF: make and its children closed the pipe
        pending.append(buf, (size_t)n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line);
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) onLine(pending);
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.exitCode = 128 + WTERMSIG(status);
    if (r.exitCode == 127 && !r.cancelled) r.error = "make could not be run (is it installed?)";
    return r;
}

}  // namespace project
