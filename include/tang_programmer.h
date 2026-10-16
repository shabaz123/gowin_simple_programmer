#ifndef TANG_PROGRAMMER_H
#define TANG_PROGRAMMER_H
#include <stdint.h>
#ifdef _WIN32
#define TANG_API __declspec(dllexport)
#else
#define TANG_API
#endif
#ifdef __cplusplus
extern "C"
{
#endif
    typedef enum
    {
        TANG_SRAM = 1,
        TANG_FLASH = 2
    } tang_operation;
    typedef void (*tang_progress_fn)(const char *json_line, void *user);
    /* Returns 0 on verified success; nonzero on failure. error is always NUL-terminated if capacity > 0. */
    TANG_API int tang_program(const char *svf_path, tang_operation operation, int device_index,
                              tang_progress_fn callback, void *user, char *error, uint32_t error_capacity);
    TANG_API int tang_audit(const char *svf_path, tang_operation operation, char *error, uint32_t error_capacity);
#ifdef __cplusplus
}
#endif
#endif
