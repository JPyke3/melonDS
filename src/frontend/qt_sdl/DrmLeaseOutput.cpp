#include "DrmLeaseOutput.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef __linux__
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

DrmLeaseOutput::~DrmLeaseOutput()
{
#ifdef __linux__
    if (scanoutMemory)
        munmap(scanoutMemory, scanoutSize);

    if (leaseFd >= 0)
        close(leaseFd);
#endif
}

#ifdef __linux__

static uint32_t FindProperty(
    int fd,
    uint32_t object,
    uint32_t objectType,
    const char* name)
{
    drmModeObjectProperties* properties =
        drmModeObjectGetProperties(fd, object, objectType);

    if (!properties)
        return 0;

    uint32_t result = 0;

    for (uint32_t i = 0; i < properties->count_props; i++)
    {
        drmModePropertyRes* property =
            drmModeGetProperty(fd, properties->props[i]);

        if (!property)
            continue;

        if (std::strcmp(property->name, name) == 0)
            result = property->prop_id;

        drmModeFreeProperty(property);

        if (result != 0)
            break;
    }

    drmModeFreeObjectProperties(properties);
    return result;
}

static bool AddProperty(
    int fd,
    drmModeAtomicReq* request,
    uint32_t object,
    uint32_t objectType,
    const char* name,
    uint64_t value)
{
    uint32_t property =
        FindProperty(fd, object, objectType, name);

    if (!property)
    {
        std::fprintf(
            stderr,
            "[drm-lease] property %s not found on object %u\n",
            name,
            object
        );
        return false;
    }

    return drmModeAtomicAddProperty(
        request,
        object,
        property,
        value
    ) >= 0;
}

#endif

void DrmLeaseOutput::PrintResources() const
{
#ifndef __linux__
    return;
#else
    if (leaseFd < 0)
        return;

    // What DRM driver owns this lease?
    drmVersionPtr version = drmGetVersion(leaseFd);

    if (version)
    {
        std::fprintf(
            stderr,
            "[drm-lease] driver: %.*s\n",
            static_cast<int>(version->name_len),
            version->name
        );

        drmFreeVersion(version);
    }
    else
    {
        std::fprintf(
            stderr,
            "[drm-lease] drmGetVersion(): %s\n",
            std::strerror(errno)
        );
    }

    // Ask for the modern KMS API we're eventually going to use.
    if (drmSetClientCap(
            leaseFd,
            DRM_CLIENT_CAP_UNIVERSAL_PLANES,
            1) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] failed to enable universal planes: %s\n",
            std::strerror(errno)
        );
    }

    if (drmSetClientCap(
            leaseFd,
            DRM_CLIENT_CAP_ATOMIC,
            1) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] failed to enable atomic modesetting: %s\n",
            std::strerror(errno)
        );
    }

    // Enumerate the connector and CRTC objects visible through the lease.
    drmModeRes* resources = drmModeGetResources(leaseFd);

    if (!resources)
    {
        std::fprintf(
            stderr,
            "[drm-lease] drmModeGetResources(): %s\n",
            std::strerror(errno)
        );
        return;
    }

    std::fprintf(stderr, "[drm-lease] connectors:");
    for (int i = 0; i < resources->count_connectors; i++)
        std::fprintf(stderr, " %u", resources->connectors[i]);
    std::fprintf(stderr, "\n");

    std::fprintf(stderr, "[drm-lease] crtcs:");
    for (int i = 0; i < resources->count_crtcs; i++)
        std::fprintf(stderr, " %u", resources->crtcs[i]);
    std::fprintf(stderr, "\n");

    // Get human-readable connector names such as DSI-1.
    for (int i = 0; i < resources->count_connectors; i++)
    {
        drmModeConnector* connector =
            drmModeGetConnector(leaseFd, resources->connectors[i]);

        if (!connector)
            continue;

        const char* type =
            drmModeGetConnectorTypeName(connector->connector_type);

        std::fprintf(
            stderr,
            "[drm-lease] connector %u: %s-%u, %d modes\n",
            connector->connector_id,
            type ? type : "unknown",
            connector->connector_type_id,
            connector->count_modes
        );

        drmModeFreeConnector(connector);
    }

    drmModeFreeResources(resources);

    // Planes aren't part of drmModeGetResources(), so they're queried
    // separately after enabling universal planes.
    drmModePlaneRes* planes = drmModeGetPlaneResources(leaseFd);

    if (!planes)
    {
        std::fprintf(
            stderr,
            "[drm-lease] drmModeGetPlaneResources(): %s\n",
            std::strerror(errno)
        );
        return;
    }

    std::fprintf(stderr, "[drm-lease] planes:");
    for (uint32_t i = 0; i < planes->count_planes; i++)
        std::fprintf(stderr, " %u", planes->planes[i]);
    std::fprintf(stderr, "\n");

    drmModeFreePlaneResources(planes);
#endif
}

bool DrmLeaseOutput::InitializeOutput()
{
#ifndef __linux__
    return false;
#else
    if (leaseFd < 0)
        return false;

    drmSetClientCap(
        leaseFd,
        DRM_CLIENT_CAP_UNIVERSAL_PLANES,
        1
    );

    if (drmSetClientCap(
            leaseFd,
            DRM_CLIENT_CAP_ATOMIC,
            1) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] atomic modesetting unavailable: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    drmModeRes* resources = drmModeGetResources(leaseFd);

    if (!resources ||
        resources->count_connectors < 1 ||
        resources->count_crtcs < 1)
    {
        std::fprintf(stderr, "[drm-lease] missing connector or CRTC\n");

        if (resources)
            drmModeFreeResources(resources);

        return false;
    }

    uint32_t connectorId = resources->connectors[0];
    uint32_t crtcId = resources->crtcs[0];

    drmModeConnector* connector =
        drmModeGetConnector(leaseFd, connectorId);

    if (!connector || connector->count_modes < 1)
    {
        std::fprintf(stderr, "[drm-lease] connector has no modes\n");

        if (connector)
            drmModeFreeConnector(connector);

        drmModeFreeResources(resources);
        return false;
    }

    // Start with the first mode, then prefer one explicitly marked preferred.
    drmModeModeInfo mode = connector->modes[0];

    for (int i = 0; i < connector->count_modes; i++)
    {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED)
        {
            mode = connector->modes[i];
            break;
        }
    }

    const uint32_t width = mode.hdisplay;
    const uint32_t height = mode.vdisplay;

    drmModePlaneRes* planes =
        drmModeGetPlaneResources(leaseFd);

    if (!planes || planes->count_planes < 1)
    {
        std::fprintf(stderr, "[drm-lease] no leased plane\n");

        if (planes)
            drmModeFreePlaneResources(planes);

        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return false;
    }

    uint32_t planeId = planes->planes[0];

    std::fprintf(
        stderr,
        "[drm-lease] output: %ux%u, connector=%u crtc=%u plane=%u\n",
        width,
        height,
        connectorId,
        crtcId,
        planeId
    );

    drmModeFreePlaneResources(planes);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);

    //
    // Allocate a simple CPU-writable scanout buffer.
    //

    drm_mode_create_dumb dumb{};
    dumb.width = width;
    dumb.height = height;
    dumb.bpp = 32;

    if (drmIoctl(
            leaseFd,
            DRM_IOCTL_MODE_CREATE_DUMB,
            &dumb) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] CREATE_DUMB failed: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    drm_mode_map_dumb mapRequest{};
    mapRequest.handle = dumb.handle;

    if (drmIoctl(
            leaseFd,
            DRM_IOCTL_MODE_MAP_DUMB,
            &mapRequest) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] MAP_DUMB failed: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    void* memory = mmap(
        nullptr,
        dumb.size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        leaseFd,
        mapRequest.offset
    );

    if (memory == MAP_FAILED)
    {
        std::fprintf(
            stderr,
            "[drm-lease] mmap failed: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    // Start with a black framebuffer until the first DS frame arrives.
    std::memset(memory, 0, dumb.size);


    scanoutMemory = memory;
    scanoutSize = dumb.size;
    scanoutPitch = dumb.pitch;
    scanoutWidth = width;
    scanoutHeight = height;

    //
    // Turn that buffer into a DRM framebuffer.
    //

    uint32_t framebuffer = 0;

    uint32_t handles[4] = { dumb.handle };
    uint32_t pitches[4] = { dumb.pitch };
    uint32_t offsets[4] = {};

    if (drmModeAddFB2(
            leaseFd,
            width,
            height,
            DRM_FORMAT_XRGB8888,
            handles,
            pitches,
            offsets,
            &framebuffer,
            0) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] drmModeAddFB2 failed: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    //
    // Create the mode blob used by the CRTC's MODE_ID property.
    //

    uint32_t modeBlob = 0;

    if (drmModeCreatePropertyBlob(
            leaseFd,
            &mode,
            sizeof(mode),
            &modeBlob) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] failed to create mode blob: %s\n",
            std::strerror(errno)
        );
        return false;
    }

    drmModeAtomicReq* request = drmModeAtomicAlloc();

    if (!request)
        return false;

    bool okay = true;

    //
    // DSI-1 -> CRTC 110
    //

    okay &= AddProperty(
        leaseFd, request,
        connectorId, DRM_MODE_OBJECT_CONNECTOR,
        "CRTC_ID", crtcId
    );

    //
    // Enable CRTC 110 with our display mode.
    //

    okay &= AddProperty(
        leaseFd, request,
        crtcId, DRM_MODE_OBJECT_CRTC,
        "MODE_ID", modeBlob
    );

    okay &= AddProperty(
        leaseFd, request,
        crtcId, DRM_MODE_OBJECT_CRTC,
        "ACTIVE", 1
    );

    //
    // framebuffer -> plane 49 -> CRTC 110
    //

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "FB_ID", framebuffer
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "CRTC_ID", crtcId
    );

    //
    // Source rectangle.
    // DRM represents these values in 16.16 fixed point.
    //

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "SRC_X", 0
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "SRC_Y", 0
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "SRC_W", static_cast<uint64_t>(width) << 16
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "SRC_H", static_cast<uint64_t>(height) << 16
    );

    //
    // Destination rectangle on the physical display.
    //

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "CRTC_X", 0
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "CRTC_Y", 0
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "CRTC_W", width
    );

    okay &= AddProperty(
        leaseFd, request,
        planeId, DRM_MODE_OBJECT_PLANE,
        "CRTC_H", height
    );

    if (!okay)
    {
        std::fprintf(stderr, "[drm-lease] failed to construct atomic request\n");
        drmModeAtomicFree(request);
        drmModeDestroyPropertyBlob(leaseFd, modeBlob);
        return false;
    }

    //
    // First ask the kernel "would this work?"
    //

    std::fprintf(stderr, "[drm-lease] testing atomic modeset...\n");

    if (drmModeAtomicCommit(
            leaseFd,
            request,
            DRM_MODE_ATOMIC_TEST_ONLY |
                DRM_MODE_ATOMIC_ALLOW_MODESET,
            nullptr) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] atomic TEST_ONLY failed: %s\n",
            std::strerror(errno)
        );

        drmModeAtomicFree(request);
        drmModeDestroyPropertyBlob(leaseFd, modeBlob);
        return false;
    }

    std::fprintf(stderr, "[drm-lease] atomic test passed\n");
    std::fprintf(stderr, "[drm-lease] committing initial framebuffer...\n");

    //
    // Actually do it.
    //

    if (drmModeAtomicCommit(
            leaseFd,
            request,
            DRM_MODE_ATOMIC_ALLOW_MODESET,
            nullptr) != 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] atomic commit failed: %s\n",
            std::strerror(errno)
        );

        drmModeAtomicFree(request);
        drmModeDestroyPropertyBlob(leaseFd, modeBlob);
        return false;
    }

    drmModeAtomicFree(request);
    drmModeDestroyPropertyBlob(leaseFd, modeBlob);

    const uint32_t logicalWidth = scanoutHeight;
    const uint32_t logicalHeight = scanoutWidth;

    constexpr uint32_t dsWidth = 256;
    constexpr uint32_t dsHeight = 192;

    uint32_t contentWidth = logicalWidth;
    uint32_t contentHeight =
        (contentWidth * dsHeight) / dsWidth;

    if (contentHeight > logicalHeight)
    {
        contentHeight = logicalHeight;
        contentWidth =
            (contentHeight * dsWidth) / dsHeight;
    }

    const uint32_t offsetX =
        (logicalWidth - contentWidth) / 2;

    const uint32_t offsetY =
        (logicalHeight - contentHeight) / 2;

    sourceXMap.assign(logicalWidth, -1);
    sourceYMap.assign(logicalHeight, -1);

    for (uint32_t x = 0; x < contentWidth; x++)
    {
        sourceXMap[offsetX + x] =
            (x * dsWidth) / contentWidth;
    }

    for (uint32_t y = 0; y < contentHeight; y++)
    {
        sourceYMap[offsetY + y] =
            (y * dsHeight) / contentHeight;
    }

    const char* rotation =
        std::getenv("MELONDS_DRM_ROTATION");

    rotateLeft =
        rotation && std::strcmp(rotation, "left") == 0;

    outputActive = true;

    std::fprintf(
        stderr,
        "[drm-lease] scanout ready: logical %ux%u, rotation=%s\n",
        logicalWidth,
        logicalHeight,
        rotateLeft ? "left" : "right"
    );

    std::fprintf(
        stderr,
        "[drm-lease] output active\n"
    );

    return true;
#endif
}

void DrmLeaseOutput::PresentBottomScreen(const uint32_t* pixels)
{
#ifndef __linux__
    return;
#else
    if (!outputActive || !scanoutMemory || !pixels)
        return;

    const uint32_t logicalWidth = scanoutHeight;
    const uint32_t logicalHeight = scanoutWidth;

    for (uint32_t physicalY = 0;
         physicalY < scanoutHeight;
         physicalY++)
    {
        auto* output =
            reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(scanoutMemory) +
                physicalY * scanoutPitch
            );

        for (uint32_t physicalX = 0;
             physicalX < scanoutWidth;
             physicalX++)
        {
            uint32_t logicalX;
            uint32_t logicalY;

            if (rotateLeft)
            {
                logicalX =
                    logicalWidth - 1 - physicalY;

                logicalY =
                    physicalX;
            }
            else
            {
                logicalX =
                    physicalY;

                logicalY =
                    logicalHeight - 1 - physicalX;
            }

            const int sourceX =
                sourceXMap[logicalX];

            const int sourceY =
                sourceYMap[logicalY];

            if (sourceX < 0 || sourceY < 0)
            {
                output[physicalX] = 0x00000000;
                continue;
            }

            output[physicalX] =
                pixels[(sourceY * 256) + sourceX];
        }
    }

    if (!loggedFirstFrame)
    {
        std::fprintf(
            stderr,
            "[drm-lease] presenting DS bottom framebuffer\n"
        );

        loggedFirstFrame = true;
    }
#endif
}
