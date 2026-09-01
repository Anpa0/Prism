/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linux-side GPU and session introspection.
 *
 * DXGI tells Prism.exe an adapter's description, vendor/device ID and VRAM, but
 * nothing about PCI topology, which kernel driver is bound, or what the PCIe
 * link actually trained at. That last one matters here: the second GPU is meant
 * to arrive over M.2-to-OCuLink, where a link can quietly come up at Gen3 x2
 * instead of Gen4 x4, and the adapter itself will never say so.
 *
 * Everything is read straight from sysfs. No card0/card1 assumptions anywhere:
 * DRM numbering depends on probe order, so devices are keyed by PCI address.
 */

#include "prism_log.h"
#include "sysinfo.h"

#include <glib.h>

#include <dirent.h>
#include <ctype.h>

#define PCI_CLASS_DISPLAY 0x030000u
#define PCI_CLASS_MASK    0xff0000u

/* PRISM_SYSFS_ROOT relocates the sysfs and /dev/dri lookups so the parser can
 * be exercised against a recorded tree. Unset in normal use. */
static const char* sysfs_root(void)
{
    const char* root = getenv("PRISM_SYSFS_ROOT");
    return (root && *root) ? root : "";
}

static int read_text_file(const char* path, char* out, size_t out_size)
{
    FILE*  file = fopen(path, "re");
    size_t length;

    if(!file)
        return -1;

    if(!fgets(out, (int)out_size, file))
    {
        fclose(file);
        out[0] = '\0';
        return -1;
    }
    fclose(file);

    length = strlen(out);
    while(length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r' || out[length - 1] == ' '))
        out[--length] = '\0';
    return 0;
}

static unsigned int read_hex_file(const char* path)
{
    char buffer[64];

    if(read_text_file(path, buffer, sizeof(buffer)) < 0)
        return 0;
    return (unsigned int)strtoul(buffer, NULL, 16);
}

static unsigned int read_uint_file(const char* path)
{
    char buffer[64];

    if(read_text_file(path, buffer, sizeof(buffer)) < 0)
        return 0;
    return (unsigned int)strtoul(buffer, NULL, 10);
}

/* DRIVER=amdgpu lives in the device's uevent; the symlink target of driver/
 * says the same thing but is absent when nothing is bound. */
static void read_driver(const char* pci_path, char* out, size_t out_size)
{
    char  path[512];
    FILE* file;
    char  line[256];

    out[0] = '\0';
    g_snprintf(path, sizeof(path), "%s/uevent", pci_path);
    file = fopen(path, "re");
    if(!file)
        return;

    while(fgets(line, sizeof(line), file))
    {
        if(strncmp(line, "DRIVER=", 7) == 0)
        {
            char* value = line + 7;
            value[strcspn(value, "\r\n")] = '\0';
            g_strlcpy(out, value, out_size);
            break;
        }
    }
    fclose(file);
}

/* card<N> and renderD<N> under <pci>/drm/. Their numbers are not stable across
 * boots, which is exactly why they are reported rather than assumed. */
static void read_drm_nodes(const char* pci_path, PrismGpuInfo* gpu)
{
    char           path[512];
    DIR*           dir;
    struct dirent* entry;

    g_snprintf(path, sizeof(path), "%s/drm", pci_path);
    dir = opendir(path);
    if(!dir)
        return;

    while((entry = readdir(dir)) != NULL)
    {
        if(strncmp(entry->d_name, "card", 4) == 0 && isdigit((unsigned char)entry->d_name[4]))
            g_strlcpy(gpu->drm_card, entry->d_name, sizeof(gpu->drm_card));
        else if(strncmp(entry->d_name, "renderD", 7) == 0)
            g_strlcpy(gpu->drm_render, entry->d_name, sizeof(gpu->drm_render));
    }
    closedir(dir);
}

/* The stable name a KWIN_DRM_DEVICES or DRI_PRIME setting should use. */
static void resolve_by_path(PrismGpuInfo* gpu)
{
    char candidate[256];

    g_snprintf(candidate, sizeof(candidate), "%s/dev/dri/by-path/pci-%s-card", sysfs_root(), gpu->pci_address);
    if(access(candidate, F_OK) == 0)
    {
        g_strlcpy(gpu->by_path, candidate + strlen(sysfs_root()), sizeof(gpu->by_path));
        return;
    }

    g_snprintf(candidate, sizeof(candidate), "%s/dev/dri/by-path/pci-%s-render", sysfs_root(), gpu->pci_address);
    if(access(candidate, F_OK) == 0)
        g_strlcpy(gpu->by_path, candidate + strlen(sysfs_root()), sizeof(gpu->by_path));
}

/* Human-readable name from hwdata's pci.ids. Falls back to the vendor name and
 * raw IDs, which is still enough to tell two cards apart. */
static void resolve_device_name(PrismGpuInfo* gpu)
{
    static const char* const id_paths[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                                           "/usr/share/pci.ids"};
    const char*              override    = getenv("PRISM_PCI_IDS");
    const char*              vendor_name = "Unknown vendor";
    FILE*                    file        = NULL;
    char                     line[512];
    int                      in_vendor = 0;

    switch(gpu->vendor_id)
    {
    case 0x1002: vendor_name = "AMD"; break;
    case 0x10de: vendor_name = "NVIDIA"; break;
    case 0x8086: vendor_name = "Intel"; break;
    default: break;
    }
    g_snprintf(gpu->name, sizeof(gpu->name), "%s %04x:%04x", vendor_name, gpu->vendor_id, gpu->device_id);

    if(override && *override)
        file = fopen(override, "re");
    for(size_t i = 0; i < G_N_ELEMENTS(id_paths) && !file; i++)
        file = fopen(id_paths[i], "re");
    if(!file)
        return;

    while(fgets(line, sizeof(line), file))
    {
        if(line[0] == '#' || line[0] == '\n')
            continue;

        if(line[0] != '\t')
        {
            unsigned int id = (unsigned int)strtoul(line, NULL, 16);
            in_vendor       = (id == gpu->vendor_id);
            continue;
        }

        /* Two tabs is a subsystem entry; only single-tab device lines match. */
        if(in_vendor && line[1] != '\t')
        {
            unsigned int id = (unsigned int)strtoul(line + 1, NULL, 16);
            if(id == gpu->device_id)
            {
                char* text = strchr(line + 1, ' ');
                while(text && *text == ' ')
                    text++;
                if(text)
                {
                    text[strcspn(text, "\r\n")] = '\0';
                    g_snprintf(gpu->name, sizeof(gpu->name), "%s %s", vendor_name, text);
                }
                break;
            }
        }
    }
    fclose(file);
}

static void collect_gpu_env(char* out, size_t out_size)
{
    static const char* const names[] = {
        "DRI_PRIME",           "MESA_VK_DEVICE_SELECT", "VK_DEVICE_SELECT_PCI_BUS_ID",
        "VK_LOADER_DEVICE_SELECT", "DXVK_FILTER_DEVICE_NAME", "__NV_PRIME_RENDER_OFFLOAD",
        "__GLX_VENDOR_LIBRARY_NAME", "__VK_LAYER_NV_optimus", "KWIN_DRM_DEVICES",
    };
    size_t used = 0;

    out[0] = '\0';
    for(size_t i = 0; i < G_N_ELEMENTS(names); i++)
    {
        const char* value = getenv(names[i]);
        int         written;

        if(!value || !*value)
            continue;
        written = g_snprintf(out + used, out_size - used, "%s%s=%s", used ? "  " : "", names[i], value);
        if(written <= 0 || (size_t)written >= out_size - used)
            break;
        used += (size_t)written;
    }

    if(!used)
        g_strlcpy(out, "(none set - the session default applies)", out_size);
}

void prism_sysinfo_query(PrismSystemInfo* out)
{
    DIR*           dir;
    struct dirent* entry;
    const char*    value;

    if(!out)
        return;
    memset(out, 0, sizeof(*out));

    value = getenv("XDG_SESSION_TYPE");
    g_strlcpy(out->session_type, value ? value : "unknown", sizeof(out->session_type));
    value = getenv("XDG_CURRENT_DESKTOP");
    g_strlcpy(out->desktop, value ? value : "unknown", sizeof(out->desktop));
    g_strlcpy(out->app_id, PRISM_APP_ID, sizeof(out->app_id));
    collect_gpu_env(out->gpu_env, sizeof(out->gpu_env));

    {
        char devices_root[512];
        g_snprintf(devices_root, sizeof(devices_root), "%s/sys/bus/pci/devices", sysfs_root());
        dir = opendir(devices_root);
        if(!dir)
        {
            prism_warn("cannot read %s; GPU details unavailable", devices_root);
            return;
        }
    }

    while((entry = readdir(dir)) != NULL && out->gpu_count < PRISM_GPU_MAX)
    {
        char          pci_path[512];
        char          field[512];
        unsigned int  class_code;
        PrismGpuInfo* gpu;

        if(entry->d_name[0] == '.')
            continue;

        g_snprintf(pci_path, sizeof(pci_path), "%s/sys/bus/pci/devices/%s", sysfs_root(), entry->d_name);

        g_snprintf(field, sizeof(field), "%s/class", pci_path);
        class_code = read_hex_file(field);
        if((class_code & PCI_CLASS_MASK) != PCI_CLASS_DISPLAY)
            continue;

        gpu = &out->gpus[out->gpu_count];
        g_strlcpy(gpu->pci_address, entry->d_name, sizeof(gpu->pci_address));

        g_snprintf(field, sizeof(field), "%s/vendor", pci_path);
        gpu->vendor_id = read_hex_file(field) & 0xffffu;
        g_snprintf(field, sizeof(field), "%s/device", pci_path);
        gpu->device_id = read_hex_file(field) & 0xffffu;
        g_snprintf(field, sizeof(field), "%s/boot_vga", pci_path);
        gpu->boot_vga = read_uint_file(field);

        /* The negotiated link, not the advertised capability. An OCuLink dock
         * that trained at Gen3 x2 reports it right here. */
        g_snprintf(field, sizeof(field), "%s/current_link_speed", pci_path);
        read_text_file(field, gpu->link_speed_cur, sizeof(gpu->link_speed_cur));
        g_snprintf(field, sizeof(field), "%s/max_link_speed", pci_path);
        read_text_file(field, gpu->link_speed_max, sizeof(gpu->link_speed_max));
        g_snprintf(field, sizeof(field), "%s/current_link_width", pci_path);
        gpu->link_width_cur = read_uint_file(field);
        g_snprintf(field, sizeof(field), "%s/max_link_width", pci_path);
        gpu->link_width_max = read_uint_file(field);

        read_driver(pci_path, gpu->driver, sizeof(gpu->driver));
        read_drm_nodes(pci_path, gpu);
        resolve_by_path(gpu);
        resolve_device_name(gpu);

        out->gpu_count++;
    }
    closedir(dir);
}
