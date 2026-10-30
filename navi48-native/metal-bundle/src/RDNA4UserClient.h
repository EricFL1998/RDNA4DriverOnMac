#ifndef RDNA4UserClient_h
#define RDNA4UserClient_h

#include <IOKit/IOKitLib.h>
#include <stdint.h>

// Thin wrapper over the RDNA4Driver user client.
//
// The hardware interface lives on the RDNA4Driver service (the accelerator kext
// is a separate binary and cannot call into it), so everything here opens
// "RDNA4Driver" and speaks the selector set documented in
// RDNA4Driver/src/core/RDNA4DriverUserClient.hpp:
//   0 GetDeviceInfo { vendor, device, windowGPU, windowSize }
//   1 AllocateBuffer(size) -> GPU address
//   2 FreeBuffer(GPU address)
//   3 SubmitCommands(PM4 dwords) -> fence value
//   4 WaitFence(fence value, timeout ms)
// Memory type 0 maps the driver's user window: the only VRAM a client may
// touch. Buffer contents therefore live at mapped + (gpuAddress - windowGPU).

typedef struct {
    io_connect_t connection;
    uint64_t windowGpu;    // GPU address of the mapped window (0 if unavailable)
    uint64_t windowSize;   // bytes
    void* windowCpu;       // CPU mapping, or NULL when mapping failed
} RDNA4GpuLink;

// Opens the service and maps the window on first use; returns NULL if the
// driver is not loaded or the window cannot be mapped.
RDNA4GpuLink* RDNA4GpuLinkAcquire(void);
void RDNA4GpuLinkRelease(void);

// Host-side pointer for a GPU address handed out by AllocateBuffer, or NULL
// when the address is outside the mapped window.
void* RDNA4GpuPointerForAddress(RDNA4GpuLink* link, uint64_t gpuAddress);

uint64_t RDNA4GpuAllocate(RDNA4GpuLink* link, uint64_t size);
void RDNA4GpuFree(RDNA4GpuLink* link, uint64_t gpuAddress);

// Submits dwords; *fence receives the value the driver's fence packet carries.
// The driver appends that packet itself, so callers must not emit one.
int RDNA4GpuSubmit(RDNA4GpuLink* link, const void* pm4, size_t bytes, uint64_t* fence);
int RDNA4GpuWait(RDNA4GpuLink* link, uint64_t fence, uint64_t timeoutMs);

// Same contract, but on the graphics (GFX) queue the MES mapped. Render
// command buffers use these; compute keeps using the pair above.
int RDNA4GpuSubmitGraphics(RDNA4GpuLink* link, const void* pm4, size_t bytes, uint64_t* fence);
int RDNA4GpuWaitGraphics(RDNA4GpuLink* link, uint64_t fence, uint64_t timeoutMs);

#endif /* RDNA4UserClient_h */
