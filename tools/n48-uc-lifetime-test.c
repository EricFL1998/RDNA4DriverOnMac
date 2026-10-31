// Allocation lifecycle regression; no GPU commands are submitted.
// clang -arch x86_64 -Wall -Wextra -Werror tools/n48-uc-lifetime-test.c \
//   -framework IOKit -framework CoreFoundation -o /tmp/n48-uc-lifetime-test
#include <IOKit/IOKitLib.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static const uint64_t ringBytes = 11075583;

static io_connect_t openDriver(void) {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RDNA4Driver"));
    io_connect_t connection = IO_OBJECT_NULL;
    if (!service) return connection;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    return kr == KERN_SUCCESS ? connection : IO_OBJECT_NULL;
}

static uint64_t allocate(io_connect_t connection, uint64_t bytes) {
    uint64_t address = 0;
    uint32_t count = 1;
    kern_return_t kr = IOConnectCallMethod(connection, 1, &bytes, 1, NULL, 0,
                                          &address, &count, NULL, NULL);
    if (kr != KERN_SUCCESS || count != 1 || !address) {
        fprintf(stderr, "AllocateBuffer(%llu): kr=0x%x address=0x%llx\n",
                (unsigned long long)bytes, kr, (unsigned long long)address);
        return 0;
    }
    return address;
}

static kern_return_t release(io_connect_t connection, uint64_t address) {
    return IOConnectCallMethod(connection, 2, &address, 1, NULL, 0,
                               NULL, NULL, NULL, NULL);
}

static int probeClose(void) {
    io_connect_t first = openDriver();
    if (!first) return 1;
    uint64_t before = allocate(first, 4096);
    IOServiceClose(first);
    if (!before) return 1;
    io_connect_t second = openDriver();
    if (!second) return 1;
    uint64_t after = allocate(second, 4096);
    printf("close/reopen: before=0x%llx after=0x%llx\n",
           (unsigned long long)before, (unsigned long long)after);
    if (after) release(second, after);
    // On the old driver, reclaim the leaked probe through its unrestricted
    // FreeBuffer API so this diagnostic does not exhaust the freshly booted GPU.
    if (before != after) release(second, before);
    IOServiceClose(second);
    if (before != after) fprintf(stderr, "FAIL: closed connection's page was not reused\n");
    return before == after ? 0 : 1;
}

static int testClose(void) {
    // Eight live allocations cannot fit simultaneously in the 32 MiB window.
    // Each closed connection must return its range to the next connection.
    for (int i = 0; i < 8; ++i) {
        io_connect_t connection = openDriver();
        if (!connection) return 1;
        uint64_t address = allocate(connection, ringBytes);
        kern_return_t kr = IOServiceClose(connection); // intentionally no FreeBuffer
        if (!address || kr != KERN_SUCCESS) {
            fprintf(stderr, "FAIL: close/reopen cycle %d\n", i + 1);
            return 1;
        }
    }
    puts("PASS: 8 ring allocations reclaimed by IOServiceClose");
    return 0;
}

static int testDeath(const char *executable) {
    for (int i = 0; i < 8; ++i) {
        pid_t child = 0;
        char *arguments[] = {(char *)executable, "--exit-with-allocation", NULL};
        if (posix_spawn(&child, executable, NULL, NULL, arguments, environ)) return 1;
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL: process-exit cycle %d\n", i + 1);
            return 1;
        }
    }
    puts("PASS: 8 exiting processes leave no ring allocation behind");
    return 0;
}

static int testOwnership(void) {
    io_connect_t owner = openDriver(), other = openDriver();
    if (!owner || !other) return 1;
    uint64_t address = allocate(owner, 4096);
    int failed = !address;
    if (address) {
        if (release(other, address) != kIOReturnBadArgument) {
            fprintf(stderr, "FAIL: another connection can free the owner's buffer\n");
            failed = 1;
        }
        if (release(owner, address) != KERN_SUCCESS) failed = 1;
    }
    IOServiceClose(other);
    IOServiceClose(owner);
    if (!failed) puts("PASS: allocation ownership survives another client's free/close");
    return failed;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--exit-with-allocation")) {
        io_connect_t connection = openDriver();
        // _exit bypasses user-space cleanup; the kernel owns reclamation.
        _exit(connection && allocate(connection, ringBytes) ? 0 : 1);
    }
    if (argc == 2 && !strcmp(argv[1], "--close")) return testClose();
    if (argc == 2 && !strcmp(argv[1], "--probe-close")) return probeClose();
    if (argc != 1) return 2;
    if (testClose() || testDeath(argv[0]) || testOwnership()) return 1;
    puts("PASS: all user VRAM lifecycle tests");
    return 0;
}
