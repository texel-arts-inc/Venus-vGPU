#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    struct stat st;
    char enforcing;
    if (chroot("/newroot") || chdir("/")) { perror("chroot"); return 1; }
    int flag = open("/sys/fs/selinux/enforce", O_RDONLY);
    if (flag < 0 || read(flag, &enforcing, 1) != 1 || enforcing != '1') {
        fputs("SELinux enforcing flag is required\n", stderr);
        return 1;
    }
    close(flag);
    int p = open("/etc/venus-policy", O_RDONLY | O_CLOEXEC);
    int l = open("/sys/fs/selinux/load", O_WRONLY | O_CLOEXEC);
    if (p < 0 || l < 0 || fstat(p, &st) < 0) { perror("open/stat policy"); return 1; }
    void *buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, p, 0);
    if (buf == MAP_FAILED || write(l, buf, st.st_size) != st.st_size) { perror("load policy"); return 1; }
    fputs("Venus bootstrap loaded SELinux policy in enforcing mode\n", stderr);
    execl("/init", "/init", (char *)NULL);
    perror("exec labeled init");
    return 1;
}
