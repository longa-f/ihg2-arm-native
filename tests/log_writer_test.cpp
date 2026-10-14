#include "ihg2_arm/log_writer.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    assert(file.good());
}

int write_stream(const std::string& input, const std::string& active,
                 const std::string& rotated, std::size_t limit) {
    int pipe_fds[2];
    assert(::pipe(pipe_fds) == 0);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const ssize_t n = ::write(pipe_fds[1], input.data() + offset, input.size() - offset);
        assert(n > 0);
        offset += static_cast<std::size_t>(n);
    }
    assert(::close(pipe_fds[1]) == 0);
    const int result = ihg2_arm_log_write_stream(pipe_fds[0], active.c_str(), rotated.c_str(), limit);
    assert(::close(pipe_fds[0]) == 0);
    return result;
}

} // namespace

int main() {
    char temporary_directory[] = "/tmp/ihg2-log-writer.XXXXXX";
    const char* created_directory = ::mkdtemp(temporary_directory);
    assert(created_directory != nullptr);
    const std::string directory(created_directory);
    const std::string active = directory + "/bridge.log";
    const std::string rotated = directory + "/bridge.log.1";
    const std::string log_pipe = directory + "/bridge-log.pipe";

    assert(ihg2_arm_prepare_log_pipe(log_pipe.c_str()) == 0);
    struct stat pipe_stat;
    assert(::lstat(log_pipe.c_str(), &pipe_stat) == 0);
    assert(S_ISFIFO(pipe_stat.st_mode));
    assert((pipe_stat.st_mode & 0777) == 0600);
    assert(pipe_stat.st_uid == ::geteuid());

    std::string input;
    for (int i = 0; i < 100; ++i) input.push_back(static_cast<char>('A' + (i % 26)));
    assert(write_stream(input, active, rotated, 64) == 0);
    assert(read_file(active) == input.substr(64));
    assert(read_file(rotated) == input.substr(0, 64));

    std::string next_input;
    for (int i = 0; i < 80; ++i) next_input.push_back(static_cast<char>('a' + (i % 26)));
    assert(write_stream(next_input, active, rotated, 64) == 0);
    assert(read_file(active) == next_input.substr(28));
    assert(read_file(rotated) == input.substr(64) + next_input.substr(0, 28));

    write_file(active, std::string(100, 'x'));
    write_file(rotated, std::string(80, 'y'));
    assert(write_stream("abc", active, rotated, 64) == 0);
    assert(read_file(active) == "abc");
    assert(read_file(rotated) == std::string(64, 'x'));

    assert(::unlink(active.c_str()) == 0);
    assert(::unlink(rotated.c_str()) == 0);
    assert(::unlink(log_pipe.c_str()) == 0);
    assert(::rmdir(directory.c_str()) == 0);
    return 0;
}
