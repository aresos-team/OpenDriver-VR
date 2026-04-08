#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <xf86drmMode.h>
#include <stdio.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>

#define LOG(f, ...) fprintf(stderr, "OPENDRIVER SHIM: " f "\n", ##__VA_ARGS__)

// --- Real DRM Functions ---
static drmModeResPtr       (*real_drmModeGetResources)(int) = NULL;
static drmModeConnectorPtr (*real_drmModeGetConnector)(int, uint32_t) = NULL;
static void (*real_drmModeFreeResources)(drmModeResPtr) = NULL;
static void (*real_drmModeFreeConnector)(drmModeConnectorPtr) = NULL;
static drmModeEncoderPtr   (*real_drmModeGetEncoder)(int, uint32_t) = NULL;
static void (*real_drmModeFreeEncoder)(drmModeEncoderPtr) = NULL;

// Track which pointers WE allocated so we know how to free them safely
static drmModeResPtr       g_fake_res_ptr = NULL;
static drmModeConnectorPtr g_fake_conn_ptr = NULL;

static int drm_fd_global = -1;
static uint32_t drm_connector_id_global = 0;

static void load_drm_fns() {
    if (real_drmModeGetResources) return;
    real_drmModeGetResources = (drmModeResPtr (*)(int))dlsym(RTLD_NEXT, "drmModeGetResources");
    real_drmModeGetConnector = (drmModeConnectorPtr (*)(int, uint32_t))dlsym(RTLD_NEXT, "drmModeGetConnector");
    real_drmModeFreeResources = (void (*)(drmModeResPtr))dlsym(RTLD_NEXT, "drmModeFreeResources");
    real_drmModeFreeConnector = (void (*)(drmModeConnectorPtr))dlsym(RTLD_NEXT, "drmModeFreeConnector");
    real_drmModeGetEncoder = (drmModeEncoderPtr (*)(int, uint32_t))dlsym(RTLD_NEXT, "drmModeGetEncoder");
    real_drmModeFreeEncoder = (void (*)(drmModeEncoderPtr))dlsym(RTLD_NEXT, "drmModeFreeEncoder");
}

static void open_drm_fd() {
    if (drm_fd_global >= 0) return;
    load_drm_fns();

    DIR *dir = opendir("/dev/dri");
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                drmModeResPtr res = real_drmModeGetResources(fd);
                if (res) {
                    for (int i = 0; i < (int)res->count_connectors; ++i) {
                        drmModeConnectorPtr conn = real_drmModeGetConnector(fd, res->connectors[i]);
                        if (conn && conn->connection == DRM_MODE_CONNECTED) {
                            drm_fd_global = fd;
                            drm_connector_id_global = res->connectors[i];
                            real_drmModeFreeConnector(conn);
                            real_drmModeFreeResources(res);
                            closedir(dir);
                            LOG("Selected real DRM node %s connector %u", path, drm_connector_id_global);
                            return;
                        }
                        if (conn) real_drmModeFreeConnector(conn);
                    }
                    real_drmModeFreeResources(res);
                }
                close(fd);
            }
        }
    }
    closedir(dir);
    
    // Fallback: search for any card if none are connected (headless mode)
    dir = opendir("/dev/dri");
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                drm_fd_global = fd;
                drm_connector_id_global = 999; // Fake connector ID
                LOG("Fallback to DRM node %s (no connected detect)", path);
                closedir(dir);
                return;
            }
        }
    }
    if(dir) closedir(dir);
}

// --- Wayland Shim ---
typedef struct wl_proxy wl_proxy;
typedef struct wl_interface wl_interface;

static int (*real_wl_proxy_add_listener)(wl_proxy*, void (**)(void), void*) = NULL;
static wl_proxy* (*real_wl_proxy_marshal_array_flags)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, uint32_t, void*) = NULL;

static char fake_device_id[8];
static char fake_connector_id[8];
static char fake_lease_request_id[8];
static char fake_lease_id[8];

extern "C" int wl_proxy_add_listener(wl_proxy *proxy, void (**implementation)(void), void *data) {
    if (!real_wl_proxy_add_listener) real_wl_proxy_add_listener = (int (*)(wl_proxy*, void (**)(void), void*))dlsym(RTLD_NEXT, "wl_proxy_add_listener");

    if (proxy == (wl_proxy*)&fake_connector_id) {
        typedef void (*name_fn)(void*, void*, const char*);
        typedef void (*desc_fn)(void*, void*, const char*);
        typedef void (*id_fn)(void*, void*, uint32_t);
        typedef void (*done_fn)(void*, void*);
        
        void **impl = (void**)implementation;
        ((name_fn)impl[0])(data, (wl_proxy*)&fake_connector_id, "OpenDriver_Virtual_HMD");
        ((desc_fn)impl[1])(data, (wl_proxy*)&fake_connector_id, "Virtual Display (Shimmed)");
        ((id_fn)impl[2])(data, (wl_proxy*)&fake_connector_id, drm_connector_id_global);
        ((done_fn)impl[3])(data, (wl_proxy*)&fake_connector_id);
        return 0;
    }
    
    if (proxy == (wl_proxy*)&fake_lease_id) {
        typedef void (*fd_fn)(void*, void*, int);
        void **impl = (void**)implementation;
        open_drm_fd();
        int lease_fd = (drm_fd_global >= 0) ? dup(drm_fd_global) : -1;
        LOG("Providing fake lease FD %d", lease_fd);
        ((fd_fn)impl[0])(data, (wl_proxy*)&fake_lease_id, lease_fd);
        return 0;
    }

    if (proxy == (wl_proxy*)&fake_device_id) {
        typedef void (*fd_fn)(void*, void*, int);
        typedef void (*conn_fn)(void*, void*, void*);
        typedef void (*done_fn)(void*, void*);
        
        void **impl = (void**)implementation;
        open_drm_fd();
        ((fd_fn)impl[0])(data, (wl_proxy*)&fake_device_id, drm_fd_global);
        ((conn_fn)impl[1])(data, (wl_proxy*)&fake_device_id, (wl_proxy*)&fake_connector_id);
        ((done_fn)impl[2])(data, (wl_proxy*)&fake_device_id);
        return 0;
    }

    return real_wl_proxy_add_listener(proxy, implementation, data);
}

// Improved variadic forwarding helper (mostly for x86_64)
extern "C" wl_proxy* wl_proxy_marshal_array_flags(wl_proxy *proxy, uint32_t opcode, const wl_interface *intf, uint32_t ver, uint32_t flags, void *args) {
    if (!real_wl_proxy_marshal_array_flags) real_wl_proxy_marshal_array_flags = (wl_proxy* (*)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, uint32_t, void*))dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");

    if (proxy == (wl_proxy*)&fake_device_id && opcode == 0) return (wl_proxy*)&fake_lease_request_id;
    if (proxy == (wl_proxy*)&fake_lease_request_id && opcode == 1) return (wl_proxy*)&fake_lease_id;

    return real_wl_proxy_marshal_array_flags(proxy, opcode, intf, ver, flags, args);
}

extern "C" void* wl_registry_bind(void* proxy, uint32_t name, const void* interface, uint32_t version) {
    if (interface && strcmp(((const char**)interface)[0], "wp_drm_lease_device_v1") == 0) {
        LOG("Injecting fake DRM lease device via Wayland bind");
        return (wl_proxy*)&fake_device_id;
    }
    static void* (*real_bind)(void*, uint32_t, const void*, uint32_t) = NULL;
    if (!real_bind) real_bind = (void* (*)(void*, uint32_t, const void*, uint32_t))dlsym(RTLD_NEXT, "wl_registry_bind");
    return real_bind(proxy, name, interface, version);
}

// --- Direct DRM Hooks to prevent vrcompositor crashes ---

extern "C" drmModeResPtr drmModeGetResources(int fd) {
    load_drm_fns();
    drmModeResPtr res = real_drmModeGetResources(fd);
    if (!res && fd == drm_fd_global) {
        LOG("Mocking drmModeGetResources for hijacked FD %d", fd);
        res = (drmModeResPtr)calloc(1, sizeof(drmModeRes));
        res->count_connectors = 1;
        res->connectors = (uint32_t*)malloc(sizeof(uint32_t));
        res->connectors[0] = drm_connector_id_global;
        g_fake_res_ptr = res;  // remember for safe free
    }
    return res;
}

extern "C" drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connector_id) {
    load_drm_fns();
    drmModeConnectorPtr conn = real_drmModeGetConnector(fd, connector_id);
    if (!conn && connector_id == drm_connector_id_global) {
        LOG("Mocking drmModeGetConnector for connector %u", connector_id);
        conn = (drmModeConnectorPtr)calloc(1, sizeof(drmModeConnector));
        conn->connector_id = connector_id;
        conn->connection   = DRM_MODE_CONNECTED;
        conn->count_modes  = 1;
        conn->modes        = (drmModeModeInfoPtr)calloc(1, sizeof(drmModeModeInfo));
        strcpy(conn->modes[0].name, "OpenDriver_Virtual");
        conn->modes[0].hdisplay = 1920;
        conn->modes[0].vdisplay = 1080;
        conn->modes[0].vrefresh = 90;
        g_fake_conn_ptr = conn;  // remember for safe free
    }
    return conn;
}

// ─── CRITICAL: Free shims ────────────────────────────────────────────────────
// Without these, SteamVR calls the real drmModeFreeResources() on our fake
// malloc'd structs which can corrupt the heap if allocator internals differ.

extern "C" void drmModeFreeResources(drmModeResPtr res) {
    if (!res) return;
    if (res == g_fake_res_ptr) {
        // Free our fake structure properly
        if (res->connectors) free(res->connectors);
        free(res);
        g_fake_res_ptr = NULL;
        return;
    }
    load_drm_fns();
    if (real_drmModeFreeResources) real_drmModeFreeResources(res);
}

extern "C" void drmModeFreeConnector(drmModeConnectorPtr conn) {
    if (!conn) return;
    if (conn == g_fake_conn_ptr) {
        if (conn->modes) free(conn->modes);
        // conn->encoders, conn->props etc. are NULL (calloc'd)
        free(conn);
        g_fake_conn_ptr = NULL;
        return;
    }
    load_drm_fns();
    if (real_drmModeFreeConnector) real_drmModeFreeConnector(conn);
}

extern "C" int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *user_data) {
    // If vrcompositor tries to pageflip on our fake resources, just return success.
    // This prevents "Invalid Argument" or "Permission Denied" crashes.
    if (fd == drm_fd_global) {
        return 0; // Success
    }
    static int (*real_pageflip)(int, uint32_t, uint32_t, uint32_t, void*) = NULL;
    if (!real_pageflip) real_pageflip = (int (*)(int, uint32_t, uint32_t, uint32_t, void*))dlsym(RTLD_NEXT, "drmModePageFlip");
    return real_pageflip(fd, crtc_id, fb_id, flags, user_data);
}

__attribute__((constructor)) static void init() {
    LOG("Shim v2 loaded - Better DRM Error Catching. pid=%d", (int)getpid());
    
    // Check if we are in vrcompositor
    char cmdline[256] = {0};
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd >= 0) {
        read(fd, cmdline, sizeof(cmdline)-1);
        close(fd);
    }
    
    if (strstr(cmdline, "vrcompositor")) {
        LOG("Detected vrcompositor, keeping shim active for child threads.");
    } else {
        // In vrserver or other processes, we might want to unset it to avoid 
        // passing it to the game, but we must be careful. 
        // SteamVR 2.0+ often launches vrcompositor as a child of vrserver.
        // For now, let's keep it but maybe set a guard env var.
        if (getenv("OPDRIVER_SHIM_NO_UNSET")) {
             LOG("LD_PRELOAD unset skipped via env var.");
        } else {
             // unsetenv("LD_PRELOAD"); // Potentially dangerous, disabled for now to ensure vrcompositor gets it
        }
    }
}
