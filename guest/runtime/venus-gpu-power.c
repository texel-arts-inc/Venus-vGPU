#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool video_memory_save_enabled(void)
{
    FILE *params = fopen("/proc/driver/nvidia/params", "r");
    char line[512];
    int preserve = -1;
    int notifiers = -1;

    if (!params) {
        perror("open NVIDIA parameters");
        return false;
    }
    while (fgets(line, sizeof(line), params)) {
        if (sscanf(line, "PreserveVideoMemoryAllocations: %d", &preserve) == 1) {
            continue;
        }
        if (sscanf(line, "UseKernelSuspendNotifiers: %d", &notifiers) == 1) {
            continue;
        }
    }
    fclose(params);
    return preserve == 1 && notifiers == 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "suspend") && strcmp(argv[1], "resume"))) {
        fputs("Usage: venus-gpu-power suspend|resume\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "suspend") == 0 && !video_memory_save_enabled()) {
        fputs("NVIDIA must use explicit power control and preserve all video memory\n", stderr);
        return 1;
    }

    int fd = open("/proc/driver/nvidia/suspend", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open NVIDIA power control");
        return 1;
    }
    size_t size = strlen(argv[1]);
    ssize_t written = write(fd, argv[1], size);
    if (written < 0) {
        perror("NVIDIA power transition");
    } else if ((size_t)written != size) {
        fputs("Incomplete NVIDIA power transition\n", stderr);
    }
    close(fd);
    if (written < 0 || (size_t)written != size) {
        return 1;
    }
    printf("NVIDIA power transition completed: %s\n", argv[1]);
    return 0;
}
