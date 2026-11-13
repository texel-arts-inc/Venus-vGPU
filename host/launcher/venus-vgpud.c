#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libfdt.h"

#define RESOURCES "/Library/Frameworks/Venus.framework/Resources/MicroVM"
#define PM_PORT "com.texel-arts.venus-vgpu.pm"
#define POOL_SIZE (INT64_C(1536) << 20)

extern char **environ;
int venus_vgpu_add_pool(void *tree, size_t capacity, const uint8_t identity[16]);

struct Runtime {
    char appdir[PATH_MAX];
    char dir[64];
    char pointer[PATH_MAX];
    char console[108];
    char console_log[128];
    char qemu_log[128];
    char doorbell[108];
    char pm_socket[108];
    pid_t server;
    pid_t qemu;
};

static volatile sig_atomic_t stopping;

static void on_signal(int signum)
{
    (void)signum;
    stopping = 1;
}

static bool path_join(char *dst, size_t size, const char *parent, const char *name)
{
    int n = snprintf(dst, size, "%s/%s", parent, name);
    return n >= 0 && (size_t)n < size;
}

#define PATH_TO(dst, parent, name) path_join((dst), sizeof(dst), (parent), (name))

static bool owned_directory(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0777) == 0700;
}

static bool append_all(int fd, const void *data, size_t size)
{
    const char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

static bool is_venus_driver(io_service_t service)
{
    CFTypeRef id = IORegistryEntryCreateCFProperty(service, CFSTR("CFBundleIdentifier"),
                                                  NULL, 0);
    bool found = id && CFEqual(id, CFSTR("com.texel-arts.venus-vgpu.driver"));
    if (id) {
        CFRelease(id);
    }
    return found;
}

static bool has_user_client(io_service_t service)
{
    io_iterator_t children;
    if (IORegistryEntryGetChildIterator(service, kIOServicePlane, &children) !=
        KERN_SUCCESS) {
        return true;
    }
    bool busy = false;
    io_object_t child;
    while ((child = IOIteratorNext(children))) {
        CFTypeRef creator = IORegistryEntryCreateCFProperty(
            child, CFSTR("IOUserClientCreator"), NULL, 0);
        if (creator) {
            busy = true;
            CFRelease(creator);
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
    return busy;
}

static bool find_gpu(unsigned bdf[3])
{
    io_iterator_t services;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
          IOServiceNameMatching("VFIOUserPCIDriver"), &services) != KERN_SUCCESS) {
        return false;
    }
    unsigned matches = 0;
    bool busy = false;
    io_service_t service;
    while ((service = IOIteratorNext(services))) {
        io_registry_entry_t pci;
        /* The Venus extension matches tunnelled PCI display controllers. */
        if (is_venus_driver(service) &&
            IORegistryEntryGetParentEntry(service, kIOServicePlane, &pci) == KERN_SUCCESS) {
            CFTypeRef value = IORegistryEntryCreateCFProperty(pci, CFSTR("pcidebug"),
                                                             NULL, 0);
            char text[48], trailing;
            unsigned bus, device, function;
            if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
                CFStringGetCString((CFStringRef)value, text, sizeof(text), kCFStringEncodingASCII) &&
                sscanf(text, "%x:%x:%x%c", &bus, &device, &function, &trailing) == 3 &&
                bus <= 255 && device <= 31 && function <= 7) {
                bdf[0] = bus;
                bdf[1] = device;
                bdf[2] = function;
                matches++;
                busy |= has_user_client(service);
            }
            if (value) {
                CFRelease(value);
            }
            IOObjectRelease(pci);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(services);
    if (matches != 1) {
        fputs(matches ? "More than one external GPU is attached to the Venus driver.\n" :
                        "No external GPU with a PCI address is attached to the Venus driver.\n",
              stderr);
        return false;
    }
    if (busy) {
        fputs("The external GPU already has a DriverKit client. Close that VM first.\n",
              stderr);
        return false;
    }
    return true;
}

static pid_t spawn(const char *path, char *const args[], const char *log)
{
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        errno = error;
        return -1;
    }
    pid_t pid = -1;
    error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log,
                                           O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
    if (!error) {
        error = posix_spawn(&pid, path, &actions, NULL, args, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    errno = error;
    return error ? -1 : pid;
}

static bool exited(pid_t *pid)
{
    if (*pid <= 0) {
        return true;
    }
    int status;
    pid_t result = waitpid(*pid, &status, WNOHANG);
    if (result == *pid || (result < 0 && errno == ECHILD)) {
        *pid = 0;
        return true;
    }
    return false;
}

static bool ready_socket(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid();
}

static bool guest_ready(const char *path, bool enforcing)
{
    const char *marker = enforcing ? "VENUS MICROVM READY enforcing=1" :
                                      "VENUS MICROVM READY";
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char line[512];
    bool ready = false;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, marker, strlen(marker))) {
            ready = true;
            break;
        }
    }
    fclose(file);
    return ready;
}

static bool prepare_ram(const char *pool, const char *base, const char *dtb)
{
    const size_t cap = 1024 * 1024 + 4096;
    uint8_t id[16];
    uint8_t *tree = calloc(1, cap);
    FILE *source = fopen(base, "rb");
    bool ok = false;
    int fd = -1;
    arc4random_buf(id, sizeof(id));

    if (!tree || !source || fread(tree, 1, cap, source) < sizeof(struct fdt_header) ||
        venus_vgpu_add_pool(tree, cap, id)) {
        goto done;
    }
    fd = open(pool, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, POOL_SIZE) < 0 || !append_all(fd, id, sizeof(id))) {
        goto done;
    }
    close(fd);
    fd = open(dtb, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    ok = fd >= 0 && append_all(fd, tree, fdt_totalsize(tree));

done:
    if (fd >= 0) {
        close(fd);
    }
    if (source) {
        fclose(source);
    }
    free(tree);
    return ok;
}

static bool publish(struct Runtime *rt)
{
    char temporary[PATH_MAX], record[80];
    if (!PATH_TO(temporary, rt->appdir, "runtime.XXXXXX")) {
        return false;
    }
    int fd = mkstemp(temporary);
    int length = snprintf(record, sizeof(record), "%s\n", rt->dir);
    if (fd < 0) {
        return false;
    }
    bool written = append_all(fd, record, (size_t)length);
    close(fd);
    bool ok = written && rename(temporary, rt->pointer) == 0;
    if (!ok) {
        unlink(temporary);
    }
    return ok;
}

static void request_guest_shutdown(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        return;
    }
    strcpy(address.sun_path, path);
    address.sun_len = SUN_LEN(&address);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
        connect(fd, (struct sockaddr *)&address, address.sun_len) == 0) {
        (void)append_all(fd, "poweroff\n", 9);
    }
    close(fd);
}

static void clean_runtime(struct Runtime *rt)
{
    unlink(rt->pointer);
    if (!exited(&rt->qemu)) {
        request_guest_shutdown(rt->console);
        for (int i = 0; i < 60 && !exited(&rt->qemu); i++) {
            usleep(250000);
        }
    }
    if (!exited(&rt->qemu)) {
        kill(rt->qemu, SIGTERM);
        for (int i = 0; i < 20 && !exited(&rt->qemu); i++) {
            usleep(250000);
        }
        if (!exited(&rt->qemu)) {
            fprintf(stderr, "QEMU did not stop. Preserved its resources at %s.\n", rt->dir);
            return;
        }
    }
    if (!exited(&rt->server)) {
        kill(rt->server, SIGTERM);
        waitpid(rt->server, NULL, 0);
    }
    char path[PATH_MAX];
    if (PATH_TO(path, rt->appdir, "last-guest.log")) {
        rename(rt->console_log, path);
    }
    if (PATH_TO(path, rt->appdir, "last-qemu.log")) {
        rename(rt->qemu_log, path);
    }
    const char *files[] = { "pool.ram", "pool.dtb", "vram.qcow2", "doorbell.sock",
                           "doorbell.sock.lock", "pm-link.sock", "venus-pm.sock",
                           "console.sock" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (PATH_TO(path, rt->dir, files[i])) {
            unlink(path);
        }
    }
    rmdir(rt->dir);
}

static bool boot(struct Runtime *rt, const char *resources, const unsigned bdf[3],
                 bool enforcing)
{
    char helper[PATH_MAX], helpers[PATH_MAX], qemu_path[PATH_MAX];
    char image_tool[PATH_MAX], doorbell_server[PATH_MAX], qemu_data[PATH_MAX];
    char kernel[PATH_MAX], initrd[PATH_MAX], base[PATH_MAX], template[PATH_MAX];
    char root[PATH_MAX], root_drive[PATH_MAX + 64];
    char pool[128], dtb[128], scratch[128], link[128];
    uint32_t helper_size = sizeof(helper);
    if (_NSGetExecutablePath(helper, &helper_size) ||
        !realpath(helper, helpers)) {
        fputs("Cannot locate the Venus runtime. Reinstall Venus vGPU.\n", stderr);
        return false;
    }
    *strrchr(helpers, '/') = '\0';
    if (!PATH_TO(qemu_path, helpers, "qemu-system-aarch64-vfio") ||
        !PATH_TO(image_tool, helpers, "qemu-img") ||
        !PATH_TO(doorbell_server, helpers, "ivshmem-server") ||
        !PATH_TO(qemu_data, helpers, "../Resources/QEMU") ||
        access(qemu_path, X_OK) || access(image_tool, X_OK) ||
        access(doorbell_server, X_OK) || access(qemu_data, R_OK | X_OK)) {
        fputs("The bundled QEMU runtime is incomplete. Reinstall Venus vGPU.\n", stderr);
        return false;
    }
    if (!PATH_TO(kernel, resources, "Image") ||
        !PATH_TO(initrd, resources, "guest.initrd") ||
        !PATH_TO(base, resources, "base.dtb") ||
        !PATH_TO(template, resources, "vram.qcow2") ||
        !PATH_TO(root, resources, "venus-root.qcow2") ||
        !PATH_TO(pool, rt->dir, "pool.ram") ||
        !PATH_TO(dtb, rt->dir, "pool.dtb") ||
        !PATH_TO(scratch, rt->dir, "vram.qcow2") ||
        !PATH_TO(link, rt->dir, "pm-link.sock") ||
        access(kernel, R_OK) || access(initrd, R_OK) || access(template, R_OK) ||
        !prepare_ram(pool, base, dtb)) {
        fputs("Cannot prepare the bundled microVM kernel, guest or RAM.\n", stderr);
        return false;
    }
    char *img[] = { image_tool, "create", "-q", "-f", "qcow2",
                    "-F", "qcow2", "-b", template, scratch, NULL };
    int status;
    pid_t disk = spawn(img[0], img, rt->qemu_log);
    if (disk <= 0 || waitpid(disk, &status, 0) != disk ||
        !WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("Cannot create the microVM VRAM scratch disk.\n", stderr);
        return false;
    }
    char *server[] = { doorbell_server, "-F", "-S", rt->doorbell,
                       "-m", rt->dir, "-l", "32M", "-n", "1", NULL };
    rt->server = spawn(server[0], server, rt->qemu_log);
    for (int i = 0; i < 50 && !stopping && !exited(&rt->server); i++) {
        if (ready_socket(rt->doorbell)) {
            break;
        }
        usleep(100000);
    }
    if (stopping || !ready_socket(rt->doorbell) || chmod(rt->doorbell, 0600)) {
        fputs("IVSHMEM did not create the Venus socket.\n", stderr);
        return false;
    }

    char memory[240], drive[240], guest_chardev[240], host_chardev[240];
    char serial[360], ivshmem[240], gpu[400], dma[240];
    snprintf(memory, sizeof(memory), "memory-backend-file,id=pool,size=1536M,mem-path=%s,share=on", pool);
    snprintf(drive, sizeof(drive), "file=%s,if=none,id=vram,format=qcow2", scratch);
    snprintf(root_drive, sizeof(root_drive), "file=%s,if=none,id=venusroot,format=qcow2,readonly=on", root);
    snprintf(guest_chardev, sizeof(guest_chardev), "socket,id=venusport,path=%s,server=on,wait=off", link);
    snprintf(host_chardev, sizeof(host_chardev), "socket,id=venushost,path=%s,server=off", link);
    snprintf(serial, sizeof(serial), "socket,id=console,path=%s,server=on,wait=off,logfile=%s", rt->console, rt->console_log);
    snprintf(ivshmem, sizeof(ivshmem), "socket,id=ivshmem,path=%s", rt->doorbell);
    snprintf(gpu, sizeof(gpu), "vfio-apple-pci,id=rtx,host=%02x:%02x.%x,bus=gpuport,addr=0,dma-companion=off,dma-bounce-size=0,venus-pm-socket=%s,venus-pm-chardev=venushost",
             bdf[0], bdf[1], bdf[2], rt->pm_socket);
    snprintf(dma, sizeof(dma), "apple-dma-pci,bus=dmaport,addr=0,managed-bdf=256,x-apple-host-bus=%u,x-apple-host-device=%u,x-apple-host-function=%u",
             bdf[0], bdf[1], bdf[2]);
    char *qemu[] = {
        qemu_path, "-L", qemu_data, "-nodefaults",
        "-machine", "virt,accel=hvf", "-cpu", "host", "-smp", "2", "-m", "3072",
        "-object", "memory-backend-ram,id=ram0,size=1536M", "-object", memory,
        "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0", "-numa", "node,nodeid=1,memdev=pool",
        "-display", "none", "-monitor", "none", "-chardev", serial, "-serial", "chardev:console",
        "-kernel", kernel, "-initrd", initrd, "-dtb", dtb,
        "-append", enforcing ?
            "console=ttyAMA0,115200n8 rdinit=/init enforcing=1 module.sig_enforce=1 pci=hpmmiosize=32M,hpmmioprefsize=512M" :
            "console=ttyAMA0,115200n8 rdinit=/init pci=hpmmiosize=32M,hpmmioprefsize=512M",
        "-drive", drive, "-device", "virtio-blk-pci,drive=vram,addr=0x8",
        "-device", "virtio-serial-pci,addr=0x9", "-chardev", guest_chardev, "-chardev", host_chardev,
        "-device", "virtserialport,chardev=venusport,name=" PM_PORT,
        "-device", "pcie-root-port,id=gpuport,bus=pcie.0,addr=0x3,slot=3,chassis=3,mem-reserve=32M,pref64-reserve=512M,bus-reserve=1",
        "-device", "pcie-root-port,id=dmaport,bus=pcie.0,addr=0x4,slot=4,chassis=4,mem-reserve=8M",
        "-chardev", ivshmem, "-device", "ivshmem-doorbell,chardev=ivshmem,vectors=1,ioeventfd=off,addr=0x5",
        "-device", gpu, "-device", dma,
        enforcing ? "-drive" : NULL, root_drive, "-device",
        "virtio-blk-pci,drive=venusroot,addr=0xa", NULL,
    };
    rt->qemu = spawn(qemu[0], qemu, rt->qemu_log);
    return rt->qemu > 0;
}

int main(int argc, char **argv)
{
    bool probe = argc == 2 && !strcmp(argv[1], "--probe");
    const char *resources = RESOURCES;
    if (argc == 3 && !strcmp(argv[1], "--resources")) {
        resources = argv[2];
    } else if (argc != 1 && !probe) {
        return 2;
    }
    unsigned bdf[3];
    if (!find_gpu(bdf)) {
        return 1;
    }
    if (probe) {
        printf("Available Venus GPU: %02x:%02x.%x\n", bdf[0], bdf[1], bdf[2]);
        return 0;
    }
    char root[PATH_MAX];
    if (!PATH_TO(root, resources, "venus-root.qcow2")) {
        return 1;
    }
    bool enforcing = access(root, R_OK) == 0;
    if (argc == 1 && !enforcing) {
        fputs("The enforcing Venus guest image is missing. Reinstall Venus vGPU.\n", stderr);
        return 1;
    }
    struct passwd *user = getpwuid(geteuid());
    struct Runtime rt = { .dir = "/private/tmp/venus-vgpu.XXXXXX" };
    char support[PATH_MAX], lock[PATH_MAX];
    umask(077);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!user || !geteuid() || !PATH_TO(support, user->pw_dir, "Library/Application Support") ||
        !PATH_TO(rt.appdir, support, "Venus vGPU") ||
        (mkdir(rt.appdir, 0700) && errno != EEXIST) || !owned_directory(rt.appdir) ||
        !PATH_TO(lock, rt.appdir, "manager.lock") ||
        !PATH_TO(rt.pointer, rt.appdir, "runtime")) {
        fputs("Cannot create the private Venus vGPU support directory.\n", stderr);
        return 1;
    }
    int fd = open(lock, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        fputs("A Venus vGPU manager is already running or its lock is unavailable.\n", stderr);
        return 1;
    }
    if (!mkdtemp(rt.dir) ||
        !PATH_TO(rt.console, rt.dir, "console.sock") ||
        !PATH_TO(rt.console_log, rt.dir, "guest.log") ||
        !PATH_TO(rt.qemu_log, rt.dir, "qemu.log") ||
        !PATH_TO(rt.doorbell, rt.dir, "doorbell.sock") ||
        !PATH_TO(rt.pm_socket, rt.dir, "venus-pm.sock")) {
        fputs("Cannot create the Venus vGPU runtime directory.\n", stderr);
        return 1;
    }
    unlink(rt.pointer);
    puts("Starting the Venus microVM…");
    bool ready = false;
    if (boot(&rt, resources, bdf, enforcing)) {
        for (int i = 0; i < 900 && !stopping &&
             !exited(&rt.qemu) && !exited(&rt.server); i++) {
            if (ready_socket(rt.pm_socket) && guest_ready(rt.console_log, enforcing)) {
                ready = publish(&rt);
                break;
            }
            usleep(100000);
        }
    }
    if (ready) {
        puts("Venus vGPU ready");
        while (!stopping && !exited(&rt.qemu) && !exited(&rt.server)) {
            usleep(250000);
        }
    }
    if (!stopping) {
        fprintf(stderr, "%s. See %s/last-guest.log and last-qemu.log.\n",
                ready ? "Venus microVM exited unexpectedly" : "Venus renderer did not become ready",
                rt.appdir);
    }
    clean_runtime(&rt);
    close(fd);
    return stopping ? 0 : 1;
}
