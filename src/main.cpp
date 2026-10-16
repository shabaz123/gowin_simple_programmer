#include "tang_programmer.h"
#include <cstdio>
#include <cstring>
#include <string>
static void progress(const char *s, void *)
{
    std::puts(s);
    std::fflush(stdout);
}
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "Usage: tang_programmer sram|flash <file.svf|file.fs> [--audit] [--device-index N] [--confirm-flash-write]\n");
        return 2;
    }
    tang_operation op = std::strcmp(argv[1], "sram") == 0 ? TANG_SRAM : std::strcmp(argv[1], "flash") == 0 ? TANG_FLASH
                                                                                                           : (tang_operation)0;
    int index = 0;
    bool audit = false;
    bool confirm = false;
    for (int i = 3; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--audit") == 0)
            audit = true;
        else if (std::strcmp(argv[i], "--confirm-flash-write") == 0)
            confirm = true;
        else if (std::strcmp(argv[i], "--device-index") == 0 && i + 1 < argc)
            index = std::stoi(argv[++i]);
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    char error[2048]{};
    if (op == TANG_FLASH && !audit && !confirm)
    {
        std::fprintf(stderr, "Flash programming requires --confirm-flash-write. Run --audit first.\n");
        return 2;
    }
    int r = audit ? tang_audit(argv[2], op, error, sizeof(error)) : tang_program(argv[2], op, index, progress, nullptr, error, sizeof(error));
    if (r)
        std::printf("{\"event\":\"result\",\"success\":false,\"message\":\"%s\"}\n", error);
    else
        std::puts(audit ? "{\"event\":\"audit\",\"success\":true,\"hardware_access\":false}" : "{\"event\":\"result\",\"success\":true}");
    return r;
}
