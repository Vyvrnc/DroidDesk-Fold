/* GPU probe for DroidDesk: dlopen the given libraries (which system graphics libraries a process
 * can load in its linker namespace and environment), then with --vulkan create a Vulkan instance
 * through the system loader and list the physical devices.
 *   dlprobe [--vulkan] LIB...
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags; const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} InstanceCreateInfo;
typedef struct {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType; char deviceName[256];
    char rest[1024];
} PhysicalDeviceProperties;
typedef int32_t (*CreateInstance)(const InstanceCreateInfo *, const void *, void **);
typedef int32_t (*EnumerateDevices)(void *, uint32_t *, void **);
typedef void (*GetProperties)(void *, PhysicalDeviceProperties *);
typedef void *(*GetInstanceProcAddr)(void *, const char *);

static void vulkan(void) {
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("VULKAN dlopen libvulkan.so: %s\n", dlerror()); return; }
    GetInstanceProcAddr gipa = (GetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
    CreateInstance create = (CreateInstance) gipa(NULL, "vkCreateInstance");
    InstanceCreateInfo info = {1, NULL, 0, NULL, 0, NULL, 0, NULL};
    void *instance = NULL;
    int32_t r = create(&info, NULL, &instance);
    printf("VULKAN vkCreateInstance = %d\n", r);
    if (r != 0) return;
    EnumerateDevices enumerate = (EnumerateDevices) gipa(instance, "vkEnumeratePhysicalDevices");
    GetProperties props = (GetProperties) gipa(instance, "vkGetPhysicalDeviceProperties");
    void *devices[8];
    uint32_t count = 8;
    r = enumerate(instance, &count, devices);
    printf("VULKAN vkEnumeratePhysicalDevices = %d, %u device(s)\n", r, count);
    for (uint32_t i = 0; i < count && i < 8; i++) {
        PhysicalDeviceProperties p;
        memset(&p, 0, sizeof p);
        props(devices[i], &p);
        printf("VULKAN device %u: %s (type %u, api %u.%u.%u, vendor 0x%x)\n", i, p.deviceName, p.deviceType,
               p.apiVersion >> 22, (p.apiVersion >> 12) & 0x3ff, p.apiVersion & 0xfff, p.vendorID);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--vulkan") == 0) { vulkan(); continue; }
        void *h = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (h) printf("OK   %s\n", argv[i]);
        else printf("FAIL %s: %s\n", argv[i], dlerror());
    }
    return 0;
}
