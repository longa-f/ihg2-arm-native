#include "ihg2_arm/native_elevator.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

extern char** environ;

namespace ihg2::arm::native_elevator {
namespace {

constexpr char kActivityManager[] = "/system/bin/am";
constexpr char kAppDataDirectory[] = "/data/user/0/com.leelen.phone";
constexpr char kActivityComponent[] =
        "com.leelen.phone/com.leelen.calllift.activity.CallLiftActivity";
constexpr char kActivityTimeoutSeconds[] = "2";

void child_error_and_exit(int fd, int error) noexcept {
    const auto* bytes = reinterpret_cast<const char*>(&error);
    std::size_t written = 0;
    while (written < sizeof(error)) {
        const auto count = ::write(fd, bytes + written, sizeof(error) - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
    }
    ::_exit(127);
}

} // namespace

std::vector<std::string> activity_arguments(Direction direction) {
    const char* value = direction == Direction::up ? "up" : "down";
    return {
            "am", "start", "-n", kActivityComponent,
            "-a", "android.intent.action.MAIN",
            "-f", "0x10000000",
            "--es", "call_lift", value,
            "--ei", "close_time", kActivityTimeoutSeconds};
}

bool available() noexcept {
    struct stat st{};
    return ::lstat(kAppDataDirectory, &st) == 0 && S_ISDIR(st.st_mode) &&
           ::access(kActivityManager, X_OK) == 0;
}

bool launch(Direction direction) noexcept {
    if (!available()) return false;

    try {
        const auto arguments = activity_arguments(direction);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);

        int pipefd[2]{};
        if (::pipe(pipefd) != 0) return false;
        if (::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        const pid_t first = ::fork();
        if (first < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }
        if (first == 0) {
            ::close(pipefd[0]);
            const pid_t detached = ::fork();
            if (detached < 0) child_error_and_exit(pipefd[1], errno);
            if (detached > 0) ::_exit(0);

            (void)::setsid();
            const int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd < 0) child_error_and_exit(pipefd[1], errno);
            for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
                if (::dup2(null_fd, fd) < 0) child_error_and_exit(pipefd[1], errno);
            }
            if (null_fd > STDERR_FILENO) ::close(null_fd);
            ::execve(kActivityManager, argv.data(), environ);
            child_error_and_exit(pipefd[1], errno);
        }

        ::close(pipefd[1]);
        int status = 0;
        while (::waitpid(first, &status, 0) < 0) {
            if (errno == EINTR) continue;
            ::close(pipefd[0]);
            return false;
        }
        int child_error = 0;
        ssize_t error_bytes;
        do {
            error_bytes = ::read(pipefd[0], &child_error, sizeof(child_error));
        } while (error_bytes < 0 && errno == EINTR);
        ::close(pipefd[0]);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 && error_bytes == 0;
    } catch (...) {
        return false;
    }
}

} // namespace ihg2::arm::native_elevator
