#ifndef IHG2_ARM_LOG_WRITER_H
#define IHG2_ARM_LOG_WRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IHG2_ARM_LOG_FILE_LIMIT_BYTES (5U * 1024U * 1024U)

int ihg2_arm_prepare_log_pipe(const char* fifo_path);
int ihg2_arm_log_write_stream(int input_fd, const char* active_path,
                              const char* rotated_path, size_t file_limit);
int ihg2_arm_log_writer(const char* active_path, const char* rotated_path,
                        const char* fifo_path);

#ifdef __cplusplus
}
#endif

#endif
