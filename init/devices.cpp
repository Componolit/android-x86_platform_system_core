/*
 * Copyright (C) 2007-2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fnmatch.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>

#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>
#include <selinux/avc.h>

#include <private/android_filesystem_config.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <android-base/file.h>
#include <cutils/list.h>
#include <cutils/probe_module.h>
#include <cutils/uevent.h>

#include "devices.h"
#include "ueventd_parser.h"
#include "util.h"
#include "log.h"
#include "parser.h"

#define SYSFS_PREFIX    "/sys"
#if defined(__i386__) || defined(__x86_64__)
static const char *firmware_dirs[] = { "/system/lib/firmware" };
#else
static const char *firmware_dirs[] = { "/etc/firmware",
                                       "/vendor/firmware",
                                       "/firmware/image" };
#endif

#define MODULES_BLKLST  "/system/etc/modules.blacklist"
#define READ_MODULES_ALIAS	1
#define READ_MODULES_BLKLST	2

extern struct selabel_handle *sehandle;

static int device_fd = -1;

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *firmware;
    const char *partition_name;
    const char *device_name;
    const char *modalias;
    int partition_num;
    int major;
    int minor;
};

struct perms_ {
    char *name;
    char *attr;
    mode_t perm;
    unsigned int uid;
    unsigned int gid;
    unsigned short prefix;
    unsigned short wildcard;
};

struct perm_node {
    struct perms_ dp;
    struct listnode plist;
};

struct platform_node {
    char *name;
    char *path;
    int path_len;
    struct listnode list;
};

struct module_alias_node {
    char *name;
    char *pattern;
    struct listnode list;
};

struct module_blacklist_node {
    char *name;
    bool deferred;
    struct listnode list;
};

static list_declare(sys_perms);
static list_declare(dev_perms);
static list_declare(platform_names);
static list_declare(modules_aliases_map);
static list_declare(modules_blacklist);
static list_declare(deferred_module_loading_list);

static int read_modules_aliases();
static int read_modules_blacklist();

int add_dev_perms(const char *name, const char *attr,
                  mode_t perm, unsigned int uid, unsigned int gid,
                  unsigned short prefix,
                  unsigned short wildcard) {
    struct perm_node *node = (perm_node*) calloc(1, sizeof(*node));
    if (!node)
        return -ENOMEM;

    node->dp.name = strdup(name);
    if (!node->dp.name)
        return -ENOMEM;

    if (attr) {
        node->dp.attr = strdup(attr);
        if (!node->dp.attr)
            return -ENOMEM;
    }

    node->dp.perm = perm;
    node->dp.uid = uid;
    node->dp.gid = gid;
    node->dp.prefix = prefix;
    node->dp.wildcard = wildcard;

    if (attr)
        list_add_tail(&sys_perms, &node->plist);
    else
        list_add_tail(&dev_perms, &node->plist);

    return 0;
}

void fixup_sys_perms(const char *upath)
{
    char buf[512];
    struct listnode *node;
    struct perms_ *dp;

    /* upaths omit the "/sys" that paths in this list
     * contain, so we add 4 when comparing...
     */
    list_for_each(node, &sys_perms) {
        dp = &(node_to_item(node, struct perm_node, plist))->dp;
        if (dp->prefix) {
            if (strncmp(upath, dp->name + 4, strlen(dp->name + 4)))
                continue;
        } else if (dp->wildcard) {
            if (fnmatch(dp->name + 4, upath, FNM_PATHNAME) != 0)
                continue;
        } else {
            if (strcmp(upath, dp->name + 4))
                continue;
        }

        if ((strlen(upath) + strlen(dp->attr) + 6) > sizeof(buf))
            break;

        snprintf(buf, sizeof(buf), "/sys%s/%s", upath, dp->attr);
        INFO("fixup %s %d %d 0%o\n", buf, dp->uid, dp->gid, dp->perm);
        chown(buf, dp->uid, dp->gid);
        chmod(buf, dp->perm);
    }

    // Now fixup SELinux file labels
    int len = snprintf(buf, sizeof(buf), "/sys%s", upath);
    if ((len < 0) || ((size_t) len >= sizeof(buf))) {
        // Overflow
        return;
    }
    if (access(buf, F_OK) == 0) {
        INFO("restorecon_recursive: %s\n", buf);
        restorecon_recursive(buf);
    }
}

static bool perm_path_matches(const char *path, struct perms_ *dp)
{
    if (dp->prefix) {
        if (strncmp(path, dp->name, strlen(dp->name)) == 0)
            return true;
    } else if (dp->wildcard) {
        if (fnmatch(dp->name, path, FNM_PATHNAME) == 0)
            return true;
    } else {
        if (strcmp(path, dp->name) == 0)
            return true;
    }

    return false;
}

static mode_t get_device_perm(const char *path, const char **links,
                unsigned *uid, unsigned *gid)
{
    struct listnode *node;
    struct perm_node *perm_node;
    struct perms_ *dp;

    /* search the perms list in reverse so that ueventd.$hardware can
     * override ueventd.rc
     */
    list_for_each_reverse(node, &dev_perms) {
        bool match = false;

        perm_node = node_to_item(node, struct perm_node, plist);
        dp = &perm_node->dp;

        if (perm_path_matches(path, dp)) {
            match = true;
        } else {
            if (links) {
                int i;
                for (i = 0; links[i]; i++) {
                    if (perm_path_matches(links[i], dp)) {
                        match = true;
                        break;
                    }
                }
            }
        }

        if (match) {
            *uid = dp->uid;
            *gid = dp->gid;
            return dp->perm;
        }
    }
    /* Default if nothing found. */
    *uid = 0;
    *gid = 0;
    return 0600;
}

static void make_device(const char *path,
                        const char */*upath*/,
                        int block, int major, int minor,
                        const char **links)
{
    unsigned uid;
    unsigned gid;
    mode_t mode;
    dev_t dev;
    char *secontext = NULL;

    mode = get_device_perm(path, links, &uid, &gid) | (block ? S_IFBLK : S_IFCHR);

    if (selabel_lookup_best_match(sehandle, &secontext, path, links, mode)) {
        ERROR("Device '%s' not created; cannot find SELinux label (%s)\n",
                path, strerror(errno));
        return;
    }
    setfscreatecon(secontext);

    dev = makedev(major, minor);
    /* Temporarily change egid to avoid race condition setting the gid of the
     * device node. Unforunately changing the euid would prevent creation of
     * some device nodes, so the uid has to be set with chown() and is still
     * racy. Fixing the gid race at least fixed the issue with system_server
     * opening dynamic input devices under the AID_INPUT gid. */
    setegid(gid);
    /* If the node already exists update its SELinux label to handle cases when
     * it was created with the wrong context during coldboot procedure. */
    if (mknod(path, mode, dev) && (errno == EEXIST)) {
        if (lsetfilecon(path, secontext)) {
            ERROR("Cannot set '%s' SELinux label on '%s' device (%s)\n",
                    secontext, path, strerror(errno));
        }
    }
    chown(path, uid, -1);
    setegid(AID_ROOT);

    freecon(secontext);
    setfscreatecon(NULL);
}

static void add_platform_device(const char *path)
{
    int path_len = strlen(path);
    struct platform_node *bus;
    const char *name = path;

    if (!strncmp(path, "/devices/", 9)) {
        name += 9;
        if (!strncmp(name, "platform/", 9))
            name += 9;
    }

    INFO("adding platform device %s (%s)\n", name, path);

    bus = (platform_node*) calloc(1, sizeof(struct platform_node));
    bus->path = strdup(path);
    bus->path_len = path_len;
    bus->name = bus->path + (name - path);
    list_add_tail(&platform_names, &bus->list);
}

/*
 * given a path that may start with a platform device, find the length of the
 * platform device prefix.  If it doesn't start with a platform device, return
 * 0.
 */
static struct platform_node *find_platform_device(const char *path)
{
    int path_len = strlen(path);
    struct listnode *node;
    struct platform_node *bus;

    list_for_each_reverse(node, &platform_names) {
        bus = node_to_item(node, struct platform_node, list);
        if ((bus->path_len < path_len) &&
                (path[bus->path_len] == '/') &&
                !strncmp(path, bus->path, bus->path_len))
            return bus;
    }

    return NULL;
}

static void remove_platform_device(const char *path)
{
    struct listnode *node;
    struct platform_node *bus;

    list_for_each_reverse(node, &platform_names) {
        bus = node_to_item(node, struct platform_node, list);
        if (!strcmp(path, bus->path)) {
            INFO("removing platform device %s\n", bus->name);
            free(bus->path);
            list_remove(node);
            free(bus);
            return;
        }
    }
}

/* Given a path that may start with a PCI device, populate the supplied buffer
 * with the PCI domain/bus number and the peripheral ID and return 0.
 * If it doesn't start with a PCI device, or there is some error, return -1 */
static int find_pci_device_prefix(const char *path, char *buf, ssize_t buf_sz)
{
    const char *start, *end;

    if (strncmp(path, "/devices/pci", 12))
        return -1;

    /* Beginning of the prefix is the initial "pci" after "/devices/" */
    start = path + 9;

    /* End of the prefix is two path '/' later, capturing the domain/bus number
     * and the peripheral ID. Example: pci0000:00/0000:00:1f.2 */
    end = strchr(start, '/');
    if (!end)
        return -1;
    end = strchr(end + 1, '/');
    if (!end)
        return -1;

    /* Make sure we have enough room for the string plus null terminator */
    if (end - start + 1 > buf_sz)
        return -1;

    strncpy(buf, start, end - start);
    buf[end - start] = '\0';
    return 0;
}

static void parse_event(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->firmware = "";
    uevent->major = -1;
    uevent->minor = -1;
    uevent->partition_name = NULL;
    uevent->partition_num = -1;
    uevent->device_name = NULL;
    uevent->modalias = NULL;

        /* currently ignoring SEQNUM */
    while(*msg) {
        if(!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if(!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if(!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if(!strncmp(msg, "FIRMWARE=", 9)) {
            msg += 9;
            uevent->firmware = msg;
        } else if(!strncmp(msg, "MAJOR=", 6)) {
            msg += 6;
            uevent->major = atoi(msg);
        } else if(!strncmp(msg, "MINOR=", 6)) {
            msg += 6;
            uevent->minor = atoi(msg);
        } else if(!strncmp(msg, "PARTN=", 6)) {
            msg += 6;
            uevent->partition_num = atoi(msg);
        } else if(!strncmp(msg, "PARTNAME=", 9)) {
            msg += 9;
            uevent->partition_name = msg;
        } else if(!strncmp(msg, "DEVNAME=", 8)) {
            msg += 8;
            uevent->device_name = msg;
        } else if(!strncmp(msg, "MODALIAS=", 9)) {
            msg += 9;
            uevent->modalias = msg;
        }

        /* advance to after the next \0 */
        while(*msg++)
            ;
    }

    if (LOG_UEVENTS) {
        INFO("event { '%s', '%s', '%s', '%s', %d, %d }\n",
             uevent->action, uevent->path, uevent->subsystem,
             uevent->firmware, uevent->major, uevent->minor);
    }
}

static char **get_character_device_symlinks(struct uevent *uevent)
{
    const char *parent;
    char *slash;
    char **links;
    int link_num = 0;
    int width;
    struct platform_node *pdev;

    pdev = find_platform_device(uevent->path);
    if (!pdev)
        return NULL;

    links = (char**) malloc(sizeof(char *) * 2);
    if (!links)
        return NULL;
    memset(links, 0, sizeof(char *) * 2);

    /* skip "/devices/platform/<driver>" */
    parent = strchr(uevent->path + pdev->path_len, '/');
    if (!parent)
        goto err;

    if (!strncmp(parent, "/usb", 4)) {
        /* skip root hub name and device. use device interface */
        while (*++parent && *parent != '/');
        if (*parent)
            while (*++parent && *parent != '/');
        if (!*parent)
            goto err;
        slash = strchr(++parent, '/');
        if (!slash)
            goto err;
        width = slash - parent;
        if (width <= 0)
            goto err;

        if (asprintf(&links[link_num], "/dev/usb/%s%.*s", uevent->subsystem, width, parent) > 0)
            link_num++;
        else
            links[link_num] = NULL;
        mkdir("/dev/usb", 0755);
    }
    else {
        goto err;
    }

    return links;
err:
    free(links);
    return NULL;
}

static char **get_block_device_symlinks(struct uevent *uevent)
{
    const char *device;
    struct platform_node *pdev;
    char *slash;
    const char *type;
    char buf[256];
    char link_path[256];
    int link_num = 0;
    char *p;

    pdev = find_platform_device(uevent->path);
    if (pdev) {
        device = pdev->name;
        type = "platform";
    } else if (!find_pci_device_prefix(uevent->path, buf, sizeof(buf))) {
        device = buf;
        type = "pci";
    } else {
        return NULL;
    }

    char **links = (char**) malloc(sizeof(char *) * 4);
    if (!links)
        return NULL;
    memset(links, 0, sizeof(char *) * 4);

    INFO("found %s device %s\n", type, device);

    snprintf(link_path, sizeof(link_path), "/dev/block/%s/%s", type, device);

    if (uevent->partition_name) {
        p = strdup(uevent->partition_name);
        sanitize(p);
        if (strcmp(uevent->partition_name, p))
            NOTICE("Linking partition '%s' as '%s'\n", uevent->partition_name, p);
        if (asprintf(&links[link_num], "%s/by-name/%s", link_path, p) > 0)
            link_num++;
        else
            links[link_num] = NULL;
        free(p);
    }

    if (uevent->partition_num >= 0) {
        if (asprintf(&links[link_num], "%s/by-num/p%d", link_path, uevent->partition_num) > 0)
            link_num++;
        else
            links[link_num] = NULL;
    }

    slash = strrchr(uevent->path, '/');
    if (asprintf(&links[link_num], "%s/%s", link_path, slash + 1) > 0)
        link_num++;
    else
        links[link_num] = NULL;

    return links;
}

static void handle_device(const char *action, const char *devpath,
        const char *path, int block, int major, int minor, char **links)
{
    int i;

    if(!strcmp(action, "add")) {
        make_device(devpath, path, block, major, minor, (const char **)links);
        if (links) {
            for (i = 0; links[i]; i++)
                make_link_init(devpath, links[i]);
        }
    }

    if(!strcmp(action, "remove")) {
        if (links) {
            for (i = 0; links[i]; i++)
                remove_link(devpath, links[i]);
        }
        unlink(devpath);
    }

    if (links) {
        for (i = 0; links[i]; i++)
            free(links[i]);
        free(links);
    }
}

static void handle_platform_device_event(struct uevent *uevent)
{
    const char *path = uevent->path;

    if (!strcmp(uevent->action, "add"))
        add_platform_device(path);
    else if (!strcmp(uevent->action, "remove"))
        remove_platform_device(path);
}

static const char *parse_device_name(struct uevent *uevent, unsigned int len)
{
    const char *name;

    /* if it's not a /dev device, nothing else to do */
    if((uevent->major < 0) || (uevent->minor < 0))
        return NULL;

    /* do we have a name? */
    name = strrchr(uevent->path, '/');
    if(!name)
        return NULL;
    name++;

    /* too-long names would overrun our buffer */
    if(strlen(name) > len) {
        ERROR("DEVPATH=%s exceeds %u-character limit on filename; ignoring event\n",
                name, len);
        return NULL;
    }

    return name;
}

static void handle_block_device_event(struct uevent *uevent)
{
    const char *base = "/dev/block/";
    const char *name;
    char devpath[96];
    char **links = NULL;

    name = parse_device_name(uevent, 64);
    if (!name)
        return;

    snprintf(devpath, sizeof(devpath), "%s%s", base, name);
    make_dir(base, 0755);

    if (!strncmp(uevent->path, "/devices/", 9))
        links = get_block_device_symlinks(uevent);

    handle_device(uevent->action, devpath, uevent->path, 1,
            uevent->major, uevent->minor, links);
}

#define DEVPATH_LEN 96

static bool assemble_devpath(char *devpath, const char *dirname,
        const char *devname)
{
    int s = snprintf(devpath, DEVPATH_LEN, "%s/%s", dirname, devname);
    if (s < 0) {
        ERROR("failed to assemble device path (%s); ignoring event\n",
                strerror(errno));
        return false;
    } else if (s >= DEVPATH_LEN) {
        ERROR("%s/%s exceeds %u-character limit on path; ignoring event\n",
                dirname, devname, DEVPATH_LEN);
        return false;
    }
    return true;
}

static void mkdir_recursive_for_devpath(const char *devpath)
{
    char dir[DEVPATH_LEN];
    char *slash;

    strcpy(dir, devpath);
    slash = strrchr(dir, '/');
    *slash = '\0';
    mkdir_recursive(dir, 0755);
}

static void handle_generic_device_event(struct uevent *uevent)
{
    const char *base;
    const char *name;
    char devpath[DEVPATH_LEN] = {0};
    char **links = NULL;

    name = parse_device_name(uevent, 64);
    if (!name)
        return;

    struct ueventd_subsystem *subsystem =
            ueventd_subsystem_find_by_name(uevent->subsystem);

    if (subsystem) {
        const char *devname;

        switch (subsystem->devname_src) {
        case DEVNAME_UEVENT_DEVNAME:
            devname = uevent->device_name;
            break;

        case DEVNAME_UEVENT_DEVPATH:
            devname = name;
            break;

        default:
            ERROR("%s subsystem's devpath option is not set; ignoring event\n",
                    uevent->subsystem);
            return;
        }

        if (!assemble_devpath(devpath, subsystem->dirname, devname))
            return;
        mkdir_recursive_for_devpath(devpath);
    } else if (!strncmp(uevent->subsystem, "usb", 3)) {
         if (!strcmp(uevent->subsystem, "usb") || !strcmp(uevent->subsystem, "usbmisc")) {
            if (uevent->device_name) {
                if (!assemble_devpath(devpath, "/dev", uevent->device_name))
                    return;
                mkdir_recursive_for_devpath(devpath);
             }
             else {
                 /* This imitates the file system that would be created
                  * if we were using devfs instead.
                  * Minors are broken up into groups of 128, starting at "001"
                  */
                 int bus_id = uevent->minor / 128 + 1;
                 int device_id = uevent->minor % 128 + 1;
                 /* build directories */
                 make_dir("/dev/bus", 0755);
                 make_dir("/dev/bus/usb", 0755);
                 snprintf(devpath, sizeof(devpath), "/dev/bus/usb/%03d", bus_id);
                 make_dir(devpath, 0755);
                 snprintf(devpath, sizeof(devpath), "/dev/bus/usb/%03d/%03d", bus_id, device_id);
             }
         } else {
             /* ignore other USB events */
             return;
         }
     } else if (!strncmp(uevent->subsystem, "graphics", 8)) {
         base = "/dev/graphics/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "drm", 3)) {
         base = "/dev/dri/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "oncrpc", 6)) {
         base = "/dev/oncrpc/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "adsp", 4)) {
         base = "/dev/adsp/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "msm_camera", 10)) {
         base = "/dev/msm_camera/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "input", 5)) {
         base = "/dev/input/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "mtd", 3)) {
         base = "/dev/mtd/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "sound", 5)) {
         base = "/dev/snd/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "misc", 4) &&
                 !strncmp(name, "log_", 4)) {
         INFO("kernel logger is deprecated\n");
         base = "/dev/log/";
         make_dir(base, 0755);
         name += 4;
     } else
         base = "/dev/";
     links = get_character_device_symlinks(uevent);

     if (!devpath[0])
         snprintf(devpath, sizeof(devpath), "%s%s", base, name);

     handle_device(uevent->action, devpath, uevent->path, 0,
             uevent->major, uevent->minor, links);
}

static int is_module_blacklisted_or_deferred(const char *name, bool need_deferred)
{
    struct listnode *blklst_node;
    struct module_blacklist_node *blacklist;
    int ret = 0;

    if (!name) goto out;

    /* See if module is blacklisted, skip if it is */
    list_for_each(blklst_node, &modules_blacklist) {
        blacklist = node_to_item(blklst_node,
                                 struct module_blacklist_node,
                                 list);
        if (!strcmp(name, blacklist->name)) {
            INFO("modules %s is blacklisted\n", name);
            ret = blacklist->deferred ? (need_deferred ? 2 : 0) : 1;
            goto out;
        }
    }

out:
    return ret;
}

static int load_module_by_device_modalias(const char *id, bool need_deferred)
{
    struct listnode *alias_node;
    struct module_alias_node *alias;
    int ret = -1;

    list_for_each(alias_node, &modules_aliases_map) {
        alias = node_to_item(alias_node, struct module_alias_node, list);

        if (alias && alias->name && alias->pattern) {
            if (fnmatch(alias->pattern, id, 0) == 0) {
                INFO("trying to load module %s due to uevents\n", alias->name);

                ret = is_module_blacklisted_or_deferred(alias->name, need_deferred);
                if (ret == 0) {
                    if ((ret = insmod_by_dep(alias->name, "", NULL, 0, NULL))) {
                        /* cannot load module. try another one since
                         * there may be another match.
                         */
                        NOTICE("failed to load %s for modalias %s\n",
                             alias->name, id);
                    } else {
                        /* loading was successful */
                        INFO("loaded module %s due to uevents\n", alias->name);
                    }
                } else {
                    NOTICE("blacklisted module %s: %d\n", alias->name, ret);
                }
            }
        }
    }

    return ret;
}

static void handle_deferred_module_loading()
{
    /* try to read the module alias mapping if map is empty
     * if succeed, loading all the modules in the queue
     */
    if (!list_empty(&modules_aliases_map)) {
        struct listnode *node = NULL;
        struct listnode *next = NULL;
        struct module_alias_node *alias = NULL;

        list_for_each_safe(node, next, &deferred_module_loading_list) {
            alias = node_to_item(node, struct module_alias_node, list);

            if (alias && alias->pattern) {
                INFO("deferred loading of module for %s\n", alias->pattern);
                load_module_by_device_modalias(alias->pattern, false);
                free(alias->pattern);
                list_remove(node);
                free(alias);
            }
        }
    }
}

static int module_probe(int argc, char **argv)
{
    if (list_empty(&modules_aliases_map)) {
        read_modules_aliases();
        read_modules_blacklist();
    }

    // is it a modalias?
    int ret = load_module_by_device_modalias(argv[1], false);
    if (ret) {
        // treat it as a module name
        std::string options;
        if (argc > 2) {
            options = argv[2];
            for (int i = 3; i < argc; ++i) {
                options += ' ';
                options += argv[i];
            }
        }
        ret = insmod_by_dep(argv[1], options.c_str(), NULL, 0, NULL);
    }
    return ret;
}

int modprobe_main(int argc, char **argv)
{
    const char *prog = argv[0];

    /* We only accept requests from root user (kernel) */
    if (getuid())
        return -EPERM;

    /* Kernel will launch a user space program specified by
     * /proc/sys/kernel/modprobe to load modules.
     * No deferred loading in this case.
     */
    while (argc > 1 && (!strcmp(argv[1], "-q") || !strcmp(argv[1], "--"))) {
        klog_set_level(KLOG_NOTICE_LEVEL);
        argc--, argv++;
    }

    if (argc < 2) {
        /* it is called without enough arguments */
        return -EINVAL;
    }

    NOTICE("%s %s\n", prog, argv[1]);
    return module_probe(argc, argv);
}

static int is_booting(void)
{
    return access("/dev/.booting", F_OK) == 0;
}

static void handle_module_loading(const char *modalias)
{
    struct module_alias_node *node;

    /* once modules.alias can be read,
     * we load all the deferred ones
     */
    if (list_empty(&modules_aliases_map)) {
        if (read_modules_aliases() == 0) {
            read_modules_blacklist();
        }
    }

    if (!modalias) return;

    if (list_empty(&modules_aliases_map) ||
            load_module_by_device_modalias(modalias, is_booting()) == 2) {
        /* if module alias mapping is empty,
         * queue it for loading later
         */
        node = (module_alias_node *) calloc(1, sizeof(*node));
        if (node) {
            node->pattern = strdup(modalias);
            if (!node->pattern) {
                free(node);
            } else {
                list_add_tail(&deferred_module_loading_list, &node->list);
                INFO("add to queue for deferred module loading: %s",
                        node->pattern);
            }
        } else {
            ERROR("failed to allocate memory to store device id for deferred module loading.\n");
        }
    }
}

static void handle_device_event(struct uevent *uevent)
{
    if (!strcmp(uevent->action,"add")) {
        handle_module_loading(uevent->modalias);
    }

    if (!strcmp(uevent->action,"add") || !strcmp(uevent->action, "change") || !strcmp(uevent->action, "online"))
        fixup_sys_perms(uevent->path);

    if (!strncmp(uevent->subsystem, "block", 5)) {
        handle_block_device_event(uevent);
    } else if (!strncmp(uevent->subsystem, "platform", 8)) {
        handle_platform_device_event(uevent);
    } else {
        handle_generic_device_event(uevent);
    }
}

static int load_firmware(int fw_fd, int loading_fd, int data_fd)
{
    struct stat st;
    long len_to_copy;
    int ret = 0;

    if(fstat(fw_fd, &st) < 0)
        return -1;
    len_to_copy = st.st_size;

    write(loading_fd, "1", 1);  /* start transfer */

    while (len_to_copy > 0) {
        char buf[PAGE_SIZE];
        ssize_t nr;

        nr = read(fw_fd, buf, sizeof(buf));
        if(!nr)
            break;
        if(nr < 0) {
            ret = -1;
            break;
        }
        if (!android::base::WriteFully(data_fd, buf, nr)) {
            ret = -1;
            break;
        }
        len_to_copy -= nr;
    }

    if(!ret)
        write(loading_fd, "0", 1);  /* successful end of transfer */
    else
        write(loading_fd, "-1", 2); /* abort transfer */

    return ret;
}

static void process_firmware_event(struct uevent *uevent)
{
    char *root, *loading, *data;
    int l, loading_fd, data_fd, fw_fd;
    size_t i;
    int booting = is_booting();

    NOTICE("firmware: loading '%s' for '%s'\n",
         uevent->firmware, uevent->path);

    l = asprintf(&root, SYSFS_PREFIX"%s/", uevent->path);
    if (l == -1)
        return;

    l = asprintf(&loading, "%sloading", root);
    if (l == -1)
        goto root_free_out;

    l = asprintf(&data, "%sdata", root);
    if (l == -1)
        goto loading_free_out;

    loading_fd = open(loading, O_WRONLY|O_CLOEXEC);
    if(loading_fd < 0)
        goto data_free_out;

    data_fd = open(data, O_WRONLY|O_CLOEXEC);
    if(data_fd < 0)
        goto loading_close_out;

try_loading_again:
    for (i = 0; i < ARRAY_SIZE(firmware_dirs); i++) {
        char *file = NULL;
        l = asprintf(&file, "%s/%s", firmware_dirs[i], uevent->firmware);
        if (l == -1)
            goto data_free_out;
        fw_fd = open(file, O_RDONLY|O_CLOEXEC);
        free(file);
        if (fw_fd >= 0) {
            if(!load_firmware(fw_fd, loading_fd, data_fd))
                INFO("firmware: copy success { '%s', '%s' }\n", root, uevent->firmware);
            else
                INFO("firmware: copy failure { '%s', '%s' }\n", root, uevent->firmware);
            break;
        }
    }
    if (fw_fd < 0) {
        if (booting) {
            /* If we're not fully booted, we may be missing
             * filesystems needed for firmware, wait and retry.
             */
            usleep(100000);
            booting = is_booting();
            goto try_loading_again;
        }
        INFO("firmware: could not open '%s': %s\n", uevent->firmware, strerror(errno));
        write(loading_fd, "-1", 2);
        goto data_close_out;
    }

    close(fw_fd);
data_close_out:
    close(data_fd);
loading_close_out:
    close(loading_fd);
data_free_out:
    free(data);
loading_free_out:
    free(loading);
root_free_out:
    free(root);
}

static void handle_firmware_event(struct uevent *uevent)
{
    if(strcmp(uevent->subsystem, "firmware"))
        return;

    if(strcmp(uevent->action, "add"))
        return;

    process_firmware_event(uevent);
}

static void parse_line_module_alias(struct parse_state *state, int nargs, char **args)
{
    struct module_alias_node *node;

    if (!args ||
        (nargs != 3) ||
        !args[0] || !args[1] || !args[2]) {
        /* empty line or not enough arguments */
        return;
    }

    node = (module_alias_node *) calloc(1, sizeof(*node));
    if (!node) return;

    node->name = strdup(args[2]);
    if (!node->name) {
        free(node);
        return;
    }

    node->pattern = strdup(args[1]);
    if (!node->pattern) {
        free(node->name);
        free(node);
        return;
    }

    list_add_tail(&modules_aliases_map, &node->list);
}

static void parse_line_module_blacklist(struct parse_state *state, int nargs, char **args)
{
    struct module_blacklist_node *node;
    bool deferred;

    if (!args ||
        (nargs != 2) ||
        !args[0] || !args[1]) {
        /* empty line or not enough arguments */
        return;
    }

    /* this line does not being with "blacklist" or "deferred" */
    if (!strncmp(args[0], "blacklist", 9))
        deferred = false;
    else if (!strncmp(args[0], "deferred", 8))
        deferred = true;
    else
        return;

    node = (module_blacklist_node *) calloc(1, sizeof(*node));
    if (!node) return;

    node->name = strdup(args[1]);
    if (!node->name) {
        free(node);
        return;
    }
    node->deferred = deferred;

    list_add_tail(&modules_blacklist, &node->list);
}

static int __read_modules_desc_file(int mode)
{
    struct parse_state state;
    char *args[3];
    int nargs;
    char fn[PATH_MAX];
    int fd = -1;
    int ret = -1;
    int args_to_read = 0;
    std::string data;

    if (mode == READ_MODULES_ALIAS) {
        /* read modules.alias */
        strcat(get_default_mod_path(fn), "modules.alias");
    } else if (mode == READ_MODULES_BLKLST) {
        /* read modules.blacklist */
        strcpy(fn, MODULES_BLKLST);
    } else {
        /* unknown mode */
        goto out;
    }

    fd = open(fn, O_RDONLY);
    if (fd == -1) {
        goto out;
    }

    /* read the whole file */
    if (!read_file(fn, &data)) {
        goto out;
    }

    /* invoke tokenizer */
    nargs = 0;
    state.filename = fn;
    state.line = 1;
    state.ptr = &data[0];
    state.nexttoken = 0;
    if (mode == READ_MODULES_ALIAS) {
        state.parse_line = parse_line_module_alias;
        args_to_read = 3;
    } else if (mode == READ_MODULES_BLKLST) {
        state.parse_line = parse_line_module_blacklist;
        args_to_read = 2;
    }
    for (;;) {
        int token = next_token(&state);
        switch (token) {
        case T_EOF:
            state.parse_line(&state, 0, 0);
            ret = 0;
            goto out;
        case T_NEWLINE:
            if (nargs) {
                state.parse_line(&state, nargs, args);
                nargs = 0;
            }
            break;
        case T_TEXT:
            if (nargs < args_to_read) {
                args[nargs++] = state.text;
            }
            break;
        }
    }
    ret = 0;

out:
    if (fd != -1) {
        close(fd);
    }
    return ret;
}

static int read_modules_aliases() {
    return __read_modules_desc_file(READ_MODULES_ALIAS);
}

static int read_modules_blacklist() {
    return __read_modules_desc_file(READ_MODULES_BLKLST);
}

#define UEVENT_MSG_LEN  2048
void handle_device_fd(bool child)
{
    char msg[UEVENT_MSG_LEN+2];
    int n;
    while ((n = uevent_kernel_multicast_recv(device_fd, msg, UEVENT_MSG_LEN)) > 0) {
        if(n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        struct uevent uevent;
        parse_event(msg, &uevent);

        if (selinux_status_updated() > 0) {
            struct selabel_handle *sehandle2;
            sehandle2 = selinux_android_file_context_handle();
            if (sehandle2) {
                selabel_close(sehandle);
                sehandle = sehandle2;
            }
        }

        if (child) {
            handle_firmware_event(&uevent);
        } else {
            handle_device_event(&uevent);
        }
    }
}

/* Coldboot walks parts of the /sys tree and pokes the uevent files
** to cause the kernel to regenerate device add events that happened
** before init's device manager was started
**
** We drain any pending events from the netlink socket every time
** we poke another uevent file to make sure we don't overrun the
** socket's buffer.
*/

static void do_coldboot(DIR *d)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if(fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
        handle_device_fd();
    }

    while((de = readdir(d))) {
        DIR *d2;

        if(de->d_type != DT_DIR || de->d_name[0] == '.')
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            continue;

        d2 = fdopendir(fd);
        if(d2 == 0)
            close(fd);
        else {
            do_coldboot(d2);
            closedir(d2);
        }
    }
}

static void coldboot(const char *path)
{
    DIR *d = opendir(path);
    if(d) {
        do_coldboot(d);
        closedir(d);
    }
}

void device_init(bool child)
{
    sehandle = selinux_android_file_context_handle();
    selinux_status_open(true);

    /* is 8MB enough? udev uses 16MB! */
    device_fd = uevent_open_socket(8 * 1024 * 1024, true);
    if (device_fd == -1) {
        return;
    }
    fcntl(device_fd, F_SETFL, O_NONBLOCK);

    if (child) {
        return; // don't do coldboot in child
    }
    if (access(COLDBOOT_DONE, F_OK) == 0) {
        NOTICE("Skipping coldboot, already done!\n");
        return;
    }

    Timer t;
    coldboot("/sys/class");
    coldboot("/sys/block");
    coldboot("/sys/devices");
    handle_deferred_module_loading();
    close(open(COLDBOOT_DONE, O_WRONLY|O_CREAT|O_CLOEXEC, 0000));
    NOTICE("Coldboot took %.2fs.\n", t.duration());
}

int get_device_fd()
{
    return device_fd;
}
