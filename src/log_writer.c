#include "ihg2_arm/log_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int ihg2_arm_prepare_log_pipe(const char* fifo_path) {
    if (!fifo_path || !*fifo_path) {
        errno = EINVAL;
        return -1;
    }

    if (mkfifo(fifo_path, 0600) != 0 && errno != EEXIST) return -1;

    struct stat st;
    if (lstat(fifo_path, &st) != 0) return -1;
    if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid()) {
        errno = EINVAL;
        return -1;
    }
    return chmod(fifo_path, 0600);
}

static int open_regular_log(const char* path, int flags) {
    int fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        fchmod(fd, 0600) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int pwrite_all(int fd, const unsigned char* bytes, size_t size, off_t offset) {
    size_t written_total = 0;
    while (written_total < size) {
        ssize_t written = pwrite(fd, bytes + written_total, size - written_total,
                                 offset + (off_t)written_total);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        written_total += (size_t)written;
    }
    return 0;
}

static int shrink_to_tail(int fd, off_t old_size, size_t file_limit) {
    if (old_size <= (off_t)file_limit) return 0;

    const off_t source_offset = old_size - (off_t)file_limit;
    unsigned char buffer[4096];
    size_t copied = 0;
    while (copied < file_limit) {
        size_t count = file_limit - copied;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        ssize_t n;
        do {
            n = pread(fd, buffer, count, source_offset + (off_t)copied);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            if (n == 0) errno = EIO;
            return -1;
        }
        if (pwrite_all(fd, buffer, (size_t)n, (off_t)copied) != 0) return -1;
        copied += (size_t)n;
    }
    return ftruncate(fd, (off_t)file_limit);
}

static int truncate_rotated_if_oversize(const char* path, size_t file_limit) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)st.st_size <= file_limit) return 0;

    int fd = open_regular_log(path, O_RDWR);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || shrink_to_tail(fd, st.st_size, file_limit) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return close(fd);
}

static int rotate_logs(const char* active_path, const char* rotated_path) {
    struct stat st;
    if (lstat(rotated_path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
            errno = EINVAL;
            return -1;
        }
        if (unlink(rotated_path) != 0) return -1;
    } else if (errno != ENOENT) {
        return -1;
    }

    if (rename(active_path, rotated_path) != 0) return errno == ENOENT ? 0 : -1;
    return 0;
}

static int open_active_log(const char* active_path, const char* rotated_path,
                           size_t file_limit, size_t* current_size) {
    if (truncate_rotated_if_oversize(rotated_path, file_limit) != 0) return -1;
    int fd = open_regular_log(active_path, O_RDWR | O_CREAT);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if ((uint64_t)st.st_size > file_limit) {
        const int shrink_result = shrink_to_tail(fd, st.st_size, file_limit);
        const int close_result = close(fd);
        if (shrink_result != 0 || close_result != 0 || rotate_logs(active_path, rotated_path) != 0)
            return -1;
        fd = open_regular_log(active_path, O_RDWR | O_CREAT);
        if (fd < 0) return -1;
        *current_size = 0;
    } else {
        *current_size = (size_t)st.st_size;
    }

    if (lseek(fd, 0, SEEK_END) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int write_all(int fd, const unsigned char* bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, bytes + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

int ihg2_arm_log_write_stream(int input_fd, const char* active_path,
                              const char* rotated_path, size_t file_limit) {
    if (input_fd < 0 || !active_path || !rotated_path || file_limit == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t current_size = 0;
    int log_fd = open_active_log(active_path, rotated_path, file_limit, &current_size);
    int log_error = log_fd < 0;
    if (log_error) dprintf(STDERR_FILENO, "ihg2-arm: log writer cannot open log files: %s\n", strerror(errno));

    unsigned char buffer[4096];
    for (;;) {
        ssize_t count = read(input_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) break;
        if (count < 0) {
            int saved_errno = errno;
            if (log_fd >= 0) close(log_fd);
            errno = saved_errno;
            return -1;
        }

        size_t offset = 0;
        while (!log_error && offset < (size_t)count) {
            if (current_size == file_limit) {
                if (close(log_fd) != 0 || rotate_logs(active_path, rotated_path) != 0) {
                    log_error = 1;
                    dprintf(STDERR_FILENO, "ihg2-arm: log rotation failed: %s\n", strerror(errno));
                    log_fd = -1;
                    break;
                }
                log_fd = open_active_log(active_path, rotated_path, file_limit, &current_size);
                if (log_fd < 0) {
                    log_error = 1;
                    dprintf(STDERR_FILENO, "ihg2-arm: cannot open rotated log: %s\n", strerror(errno));
                    break;
                }
            }

            size_t available = file_limit - current_size;
            size_t remaining = (size_t)count - offset;
            size_t chunk = remaining < available ? remaining : available;
            if (write_all(log_fd, buffer + offset, chunk) != 0) {
                log_error = 1;
                dprintf(STDERR_FILENO, "ihg2-arm: log write failed; discarding further logs: %s\n",
                        strerror(errno));
                close(log_fd);
                log_fd = -1;
                break;
            }
            current_size += chunk;
            offset += chunk;
        }
    }

    if (log_fd >= 0 && close(log_fd) != 0) log_error = 1;
    return log_error ? -1 : 0;
}

int ihg2_arm_log_writer(const char* active_path, const char* rotated_path,
                        const char* fifo_path) {
    if (!active_path || !rotated_path || !fifo_path) {
        errno = EINVAL;
        return 2;
    }

    (void)umask(0077);
    if (ihg2_arm_prepare_log_pipe(fifo_path) != 0) {
        dprintf(STDERR_FILENO, "ihg2-arm: cannot prepare log pipe: %s\n", strerror(errno));
        return 1;
    }

    for (;;) {
        int fifo_fd = open(fifo_path, O_RDONLY | O_CLOEXEC);
        if (fifo_fd < 0) {
            dprintf(STDERR_FILENO, "ihg2-arm: cannot open log pipe: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        (void)ihg2_arm_log_write_stream(fifo_fd, active_path, rotated_path,
                                        IHG2_ARM_LOG_FILE_LIMIT_BYTES);
        close(fifo_fd);
    }
}
