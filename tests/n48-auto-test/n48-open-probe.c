/* Read-only IOKit routing probe: open then immediately close an N48N session.
 * No GPU allocations, submissions, or display-plane operations. */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "Navi48Accelerator";
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       IOServiceMatching(name));
    if (!service) {
        printf("service %s: not found\n", name);
        return 2;
    }
    io_connect_t connection = 0;
    const uint32_t type = 0x4e34384e; /* N48N */
    kern_return_t result = IOServiceOpen(service, mach_task_self(), type, &connection);
    printf("service %s: IOServiceOpen=0x%08x connection=0x%x\n",
           name, (unsigned)result, (unsigned)connection);
    if (result == KERN_SUCCESS && connection) IOServiceClose(connection);
    IOObjectRelease(service);
    return result == KERN_SUCCESS ? 0 : 1;
}
