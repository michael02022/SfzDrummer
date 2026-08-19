#include "zenity_dialog.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

namespace {

// Runs `zenity <args...>` (argv built directly, no shell involved - safe
// regardless of what defaultPath/filters contain) and returns its trimmed
// stdout, or "" if it exited non-zero (cancel), couldn't be exec'd (zenity
// not installed), or any step failed.
std::string runZenity(const std::vector<std::string>& args) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        // Child: stdout -> pipe, stderr -> /dev/null (zenity logs GTK
        // warnings there that we don't want interleaved with anything).
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>("zenity"));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("zenity", argv.data());
        _exit(127); // exec failed (zenity not installed)
    }

    // Parent
    close(pipefd[1]);
    std::string result;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) result.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

void appendFilters(std::vector<std::string>& args, const std::vector<ZenityFilter>& filters) {
    for (const auto& f : filters) args.push_back("--file-filter=" + f.name + " | " + f.pattern);
}

} // namespace

std::string zenitySaveFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters) {
    std::vector<std::string> args = {
        "--file-selection", "--save", "--confirm-overwrite",
        "--title=" + title,
        "--filename=" + defaultPath,
    };
    appendFilters(args, filters);
    return runZenity(args);
}

std::string zenityOpenFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters) {
    std::vector<std::string> args = {
        "--file-selection",
        "--title=" + title,
        "--filename=" + defaultPath,
    };
    appendFilters(args, filters);
    return runZenity(args);
}
