#include <stdio.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(void)
{
    sync();
    if (reboot(RB_POWER_OFF)) {
        perror("poweroff");
        return 1;
    }
    return 0;
}
