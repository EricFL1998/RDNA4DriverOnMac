/* Test-only process launcher for Mesa's G6 accelerator-route selector.
 * It changes no driver or kernel state; N48N admission is still enforced. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: %s /path/to/libvulkan_radeon.dylib\n", argv[0]);
            return 64;
        }
        void *handle = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            return 1;
        }
        int (*set_service)(const char *) = dlsym(handle, "radv_darwin_set_service_class");
        if (!set_service) {
            fprintf(stderr, "setter not found\n");
            return 1;
        }
        int rc = set_service("Navi48Accelerator");
        printf("radv_darwin_set_service_class(Navi48Accelerator) = %d\n", rc);
        return rc;
    }
}

/* A dylib (not an executable) can be inserted before mtlprobe for the same
 * route setup. N48_LOADER_RADV points at the production RADV dylib; its
 * ordinary companion shim is already present in GPUBundles. */
__attribute__((constructor)) static void select_accelerator(void) {
    const char *path = getenv("N48_LOADER_RADV");
    if (!path || !*path) return;
    const char *shim = getenv("N48_LOADER_SHIM");
    if (shim && *shim && !dlopen(shim, RTLD_NOW | RTLD_GLOBAL))
        fprintf(stderr, "loader shim: %s\n", dlerror());
    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "loader dlopen: %s\n", dlerror());
        return;
    }
    int (*set_service)(const char *) = dlsym(handle, "radv_darwin_set_service_class");
    if (set_service) {
        int rc = set_service("Navi48Accelerator");
        fprintf(stderr, "loader: accelerator route rc=%d\n", rc);
    } else {
        fprintf(stderr, "loader: setter not found\n");
    }
}
