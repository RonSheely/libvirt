/*
 * domain_driver.c: general functions shared between hypervisor drivers
 *
 * Copyright IBM Corp. 2020
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "domain_driver.h"
#include "viralloc.h"
#include "virstring.h"
#include "vircrypto.h"
#include "virutil.h"
#include "virhostdev.h"
#include "viraccessapicheck.h"
#include "datatypes.h"
#include "driver.h"
#include "virlog.h"
#include "virsystemd.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

VIR_LOG_INIT("hypervisor.domain_driver");

VIR_ENUM_IMPL(virDomainDriverAutoShutdownScope,
              VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_LAST,
              "none",
              "persistent",
              "transient",
              "all");

char *
virDomainDriverGenerateRootHash(const char *drivername,
                                const char *root)
{
    g_autofree char *hash = NULL;

    if (virCryptoHashString(VIR_CRYPTO_HASH_SHA256, root, &hash) < 0)
        return NULL;

    /* When two embed drivers start two domains with the same @name and @id
     * we would generate a non-unique name. Include parts of hashed @root
     * which guarantees uniqueness. The first 8 characters of SHA256 ought
     * to be enough for anybody. */
    return g_strdup_printf("%s-embed-%.8s", drivername, hash);
}


#define HOSTNAME_CHARS \
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"

static void
virDomainMachineNameAppendValid(virBuffer *buf,
                                const char *name)
{
    bool skip = true;

    for (; *name; name++) {
        if (strlen(virBufferCurrentContent(buf)) >= 64)
            break;

        if (*name == '.' || *name == '-') {
            if (!skip) {
                virBufferAddChar(buf, *name);
                skip = true;
            }
            continue;
        }

        if (!strchr(HOSTNAME_CHARS, *name))
            continue;

        virBufferAddChar(buf, *name);
        skip = false;
    }

    /* trailing dashes or dots are not allowed */
    virBufferTrimChars(buf, "-.");
}

#undef HOSTNAME_CHARS

char *
virDomainDriverGenerateMachineName(const char *drivername,
                                   const char *root,
                                   int id,
                                   const char *name,
                                   bool privileged)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (root) {
        g_autofree char *hash = NULL;

        if (!(hash = virDomainDriverGenerateRootHash(drivername, root)))
            return NULL;

        virBufferAsprintf(&buf, "%s-", hash);
    } else {
        virBufferAsprintf(&buf, "%s-", drivername);
        if (!privileged) {

            g_autofree char *username = NULL;
            if (!(username = virGetUserName(geteuid())))
                return NULL;

            virBufferAsprintf(&buf, "%s-", username);
        }
    }

    virBufferAsprintf(&buf, "%d-", id);
    virDomainMachineNameAppendValid(&buf, name);

    return virBufferContentAndReset(&buf);
}


/* Modify dest_array to reflect all blkio device changes described in
 * src_array.  */
int
virDomainDriverMergeBlkioDevice(virBlkioDevice **dest_array,
                                size_t *dest_size,
                                virBlkioDevice *src_array,
                                size_t src_size,
                                const char *type)
{
    size_t i, j;
    virBlkioDevice *dest;
    virBlkioDevice *src;

    for (i = 0; i < src_size; i++) {
        bool found = false;

        src = &src_array[i];
        for (j = 0; j < *dest_size; j++) {
            dest = &(*dest_array)[j];
            if (STREQ(src->path, dest->path)) {
                found = true;

                if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
                    dest->weight = src->weight;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
                    dest->riops = src->riops;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
                    dest->wiops = src->wiops;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
                    dest->rbps = src->rbps;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                    dest->wbps = src->wbps;
                } else {
                    virReportError(VIR_ERR_INVALID_ARG, _("Unknown parameter %1$s"),
                                   type);
                    return -1;
                }
                break;
            }
        }
        if (!found) {
            if (!src->weight && !src->riops && !src->wiops && !src->rbps && !src->wbps)
                continue;
            VIR_EXPAND_N(*dest_array, *dest_size, 1);
            dest = &(*dest_array)[*dest_size - 1];

            if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
                dest->weight = src->weight;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
                dest->riops = src->riops;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
                dest->wiops = src->wiops;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
                dest->rbps = src->rbps;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                dest->wbps = src->wbps;
            } else {
                *dest_size = *dest_size - 1;
                return -1;
            }

            dest->path = g_steal_pointer(&src->path);
        }
    }

    return 0;
}


/* blkioDeviceStr in the form of /device/path,weight,/device/path,weight
 * for example, /dev/disk/by-path/pci-0000:00:1f.2-scsi-0:0:0:0,800
 */
int
virDomainDriverParseBlkioDeviceStr(char *blkioDeviceStr, const char *type,
                                   virBlkioDevice **dev, size_t *size)
{
    char *temp;
    int ndevices = 0;
    int nsep = 0;
    size_t i;
    virBlkioDevice *result = NULL;

    *dev = NULL;
    *size = 0;

    if (STREQ(blkioDeviceStr, ""))
        return 0;

    temp = blkioDeviceStr;
    while (temp) {
        temp = strchr(temp, ',');
        if (temp) {
            temp++;
            nsep++;
        }
    }

    /* A valid string must have even number of fields, hence an odd
     * number of commas.  */
    if (!(nsep & 1))
        goto parse_error;

    ndevices = (nsep + 1) / 2;

    result = g_new0(virBlkioDevice, ndevices);

    i = 0;
    temp = blkioDeviceStr;
    while (temp) {
        char *p = temp;

        /* device path */
        p = strchr(p, ',');
        if (!p)
            goto parse_error;

        result[i].path = g_strndup(temp, p - temp);

        /* value */
        temp = p + 1;

        if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].weight) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].riops) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].wiops) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
            if (virStrToLong_ullp(temp, &p, 10, &result[i].rbps) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
            if (virStrToLong_ullp(temp, &p, 10, &result[i].wbps) < 0)
                goto number_error;
        } else {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unknown parameter '%1$s'"), type);
            goto cleanup;
        }

        i++;

        if (*p == '\0')
            break;
        else if (*p != ',')
            goto parse_error;
        temp = p + 1;
    }

    if (!i)
        VIR_FREE(result);

    *dev = result;
    *size = i;

    return 0;

 parse_error:
    virReportError(VIR_ERR_INVALID_ARG,
                   _("unable to parse blkio device '%1$s' '%2$s'"),
                   type, blkioDeviceStr);
    goto cleanup;

 number_error:
    virReportError(VIR_ERR_INVALID_ARG,
                   _("invalid value '%1$s' for parameter '%2$s' of device '%3$s'"),
                   temp, type, result[i].path);

 cleanup:
    if (result) {
        virBlkioDeviceArrayClear(result, ndevices);
        VIR_FREE(result);
    }
    return -1;
}


int
virDomainDriverSetupPersistentDefBlkioParams(virDomainDef *persistentDef,
                                             virTypedParameterPtr params,
                                             int nparams)
{
    size_t i;
    int ret = 0;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_BLKIO_WEIGHT)) {
            persistentDef->blkio.weight = param->value.ui;
        } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT) ||
                   STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS) ||
                   STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS) ||
                   STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS) ||
                   STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
            virBlkioDevice *devices = NULL;
            size_t ndevices;

            if (virDomainDriverParseBlkioDeviceStr(param->value.s,
                                             param->field,
                                             &devices,
                                             &ndevices) < 0) {
                ret = -1;
                continue;
            }

            if (virDomainDriverMergeBlkioDevice(&persistentDef->blkio.devices,
                                                &persistentDef->blkio.ndevices,
                                                devices, ndevices,
                                                param->field) < 0)
                ret = -1;

            virBlkioDeviceArrayClear(devices, ndevices);
            g_free(devices);
        }
    }

    return ret;
}


int
virDomainDriverNodeDeviceGetPCIInfo(virNodeDeviceDef *def,
                                    virPCIDeviceAddress *devAddr)
{
    virNodeDevCapsDef *cap;

    cap = def->caps;
    while (cap) {
        if (cap->data.type == VIR_NODE_DEV_CAP_PCI_DEV) {
            devAddr->domain = cap->data.pci_dev.domain;
            devAddr->bus = cap->data.pci_dev.bus;
            devAddr->slot = cap->data.pci_dev.slot;
            devAddr->function = cap->data.pci_dev.function;
            break;
        }

        cap = cap->next;
    }

    if (!cap) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("device %1$s is not a PCI device"), def->name);
        return -1;
    }

    return 0;
}


int
virDomainDriverNodeDeviceReset(virNodeDevicePtr dev,
                               virHostdevManager *hostdevMgr)
{
    g_autoptr(virPCIDevice) pci = NULL;
    virPCIDeviceAddress devAddr = { 0 };
    g_autoptr(virNodeDeviceDef) def = NULL;
    g_autofree char *xml = NULL;
    g_autoptr(virConnect) nodeconn = NULL;
    g_autoptr(virNodeDevice) nodedev = NULL;

    if (!(nodeconn = virGetConnectNodeDev()))
        return -1;

    /* 'dev' is associated with virConnectPtr, so for split
     * daemons, we need to get a copy that is associated with
     * the virnodedevd daemon. */
    if (!(nodedev = virNodeDeviceLookupByName(
              nodeconn, virNodeDeviceGetName(dev))))
        return -1;

    xml = virNodeDeviceGetXMLDesc(nodedev, 0);
    if (!xml)
        return -1;

    def = virNodeDeviceDefParse(xml, NULL, EXISTING_DEVICE, NULL, NULL, NULL, false);
    if (!def)
        return -1;

    /* ACL check must happen against original 'dev',
     * not the new 'nodedev' we acquired */
    if (virNodeDeviceResetEnsureACL(dev->conn, def) < 0)
        return -1;

    if (virDomainDriverNodeDeviceGetPCIInfo(def, &devAddr) < 0)
        return -1;

    pci = virPCIDeviceNew(&devAddr);
    if (!pci)
        return -1;

    return virHostdevPCINodeDeviceReset(hostdevMgr, pci);
}


int
virDomainDriverNodeDeviceReAttach(virNodeDevicePtr dev,
                                  virHostdevManager *hostdevMgr)
{
    g_autoptr(virPCIDevice) pci = NULL;
    virPCIDeviceAddress devAddr = { 0 };
    g_autoptr(virNodeDeviceDef) def = NULL;
    g_autofree char *xml = NULL;
    g_autoptr(virConnect) nodeconn = NULL;
    g_autoptr(virNodeDevice) nodedev = NULL;

    if (!(nodeconn = virGetConnectNodeDev()))
        return -1;

    /* 'dev' is associated with virConnectPtr, so for split
     * daemons, we need to get a copy that is associated with
     * the virnodedevd daemon. */
    if (!(nodedev = virNodeDeviceLookupByName(
              nodeconn, virNodeDeviceGetName(dev))))
        return -1;

    xml = virNodeDeviceGetXMLDesc(nodedev, 0);
    if (!xml)
        return -1;

    def = virNodeDeviceDefParse(xml, NULL, EXISTING_DEVICE, NULL, NULL, NULL, false);
    if (!def)
        return -1;

    /* ACL check must happen against original 'dev',
     * not the new 'nodedev' we acquired */
    if (virNodeDeviceReAttachEnsureACL(dev->conn, def) < 0)
        return -1;

    if (virDomainDriverNodeDeviceGetPCIInfo(def, &devAddr) < 0)
        return -1;

    pci = virPCIDeviceNew(&devAddr);
    if (!pci)
        return -1;

    return virHostdevPCINodeDeviceReAttach(hostdevMgr, pci);
}

int
virDomainDriverNodeDeviceDetachFlags(virNodeDevicePtr dev,
                                     virHostdevManager *hostdevMgr,
                                     virPCIStubDriver driverType,
                                     const char *driverName)
{
    g_autoptr(virPCIDevice) pci = NULL;
    virPCIDeviceAddress devAddr = { 0 };
    g_autoptr(virNodeDeviceDef) def = NULL;
    g_autofree char *xml = NULL;
    g_autoptr(virConnect) nodeconn = NULL;
    g_autoptr(virNodeDevice) nodedev = NULL;

    if (driverType == VIR_PCI_STUB_DRIVER_NONE) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("driver type not set"));
        return -1;
    }

    if (!(nodeconn = virGetConnectNodeDev()))
        return -1;

    /* 'dev' is associated with virConnectPtr, so for split
     * daemons, we need to get a copy that is associated with
     * the virnodedevd daemon. */
    if (!(nodedev = virNodeDeviceLookupByName(nodeconn,
                                              virNodeDeviceGetName(dev))))
        return -1;

    xml = virNodeDeviceGetXMLDesc(nodedev, 0);
    if (!xml)
        return -1;

    def = virNodeDeviceDefParse(xml, NULL, EXISTING_DEVICE, NULL, NULL, NULL, false);
    if (!def)
        return -1;

    /* ACL check must happen against original 'dev',
     * not the new 'nodedev' we acquired */
    if (virNodeDeviceDetachFlagsEnsureACL(dev->conn, def) < 0)
        return -1;

    if (virDomainDriverNodeDeviceGetPCIInfo(def, &devAddr) < 0)
        return -1;

    pci = virPCIDeviceNew(&devAddr);
    if (!pci)
        return -1;

    virPCIDeviceSetStubDriverType(pci, driverType);
    virPCIDeviceSetStubDriverName(pci, driverName);

    return virHostdevPCINodeDeviceDetach(hostdevMgr, pci);
}

/**
 * virDomainDriverAddIOThreadCheck:
 * @def: domain definition
 * @iothread_id: iothread id
 *
 * Returns -1 if an IOThread is already using the given iothread id
 */
int
virDomainDriverAddIOThreadCheck(virDomainDef *def,
                                unsigned int iothread_id)
{
    if (virDomainIOThreadIDFind(def, iothread_id)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("an IOThread is already using iothread_id '%1$u'"),
                       iothread_id);
        return -1;
    }

    return 0;
}


static bool
virDomainIothreadMappingDefHasIothread(GSList *iothreads,
                                       unsigned int iothread_id)
{
    GSList *n;

    for (n = iothreads; n; n = n->next) {
        virDomainIothreadMappingDef *iothread = n->data;

        if (iothread->id == iothread_id)
            return true;
    }

    return false;
}

/**
 * virDomainDriverDelIOThreadCheck:
 * @def: domain definition
 * @iothread_id: iothread id
 *
 * Returns -1 if there is no IOThread using the given iothread id
 */
int
virDomainDriverDelIOThreadCheck(virDomainDef *def,
                                unsigned int iothread_id)
{
    size_t i;

    if (!virDomainIOThreadIDFind(def, iothread_id)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("cannot find IOThread '%1$u' in iothreadids list"),
                       iothread_id);
        return -1;
    }

    for (i = 0; i < def->ndisks; i++) {
        if (virDomainIothreadMappingDefHasIothread(def->disks[i]->iothreads, iothread_id) ||
            def->disks[i]->iothread == iothread_id) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("cannot remove IOThread %1$u since it is being used by disk '%2$s'"),
                           iothread_id, def->disks[i]->dst);
            return -1;
        }
    }

    for (i = 0; i < def->ncontrollers; i++) {
        if (virDomainIothreadMappingDefHasIothread(def->controllers[i]->iothreads, iothread_id) ||
            def->controllers[i]->iothread == iothread_id) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("cannot remove IOThread '%1$u' since it is being used by controller"),
                           iothread_id);
            return -1;
        }
    }

    return 0;
}

/**
 * virDomainDriverGetIOThreadsConfig:
 * @targetDef: domain definition
 * @info: information about the IOThread in a domain
 * @bitmap_size: generate bitmap with bitmap_size, 0 for getting the size
 * from host
 *
 * Returns the number of IOThreads in the given domain or -1 in case of error
 */
int
virDomainDriverGetIOThreadsConfig(virDomainDef *targetDef,
                                  virDomainIOThreadInfoPtr **info,
                                  unsigned int bitmap_size)
{
    virDomainIOThreadInfoPtr *info_ret = NULL;
    size_t i;
    int ret = -1;

    if (targetDef->niothreadids == 0)
        return 0;

    info_ret = g_new0(virDomainIOThreadInfoPtr, targetDef->niothreadids);

    for (i = 0; i < targetDef->niothreadids; i++) {
        g_autoptr(virBitmap) bitmap = NULL;
        virBitmap *cpumask = NULL;
        info_ret[i] = g_new0(virDomainIOThreadInfo, 1);

        /* IOThread ID's are taken from the iothreadids list */
        info_ret[i]->iothread_id = targetDef->iothreadids[i]->iothread_id;

        cpumask = targetDef->iothreadids[i]->cpumask;
        if (!cpumask) {
            if (targetDef->cpumask) {
                cpumask = targetDef->cpumask;
            } else {
                if (bitmap_size) {
                    if (!(bitmap = virBitmapNew(bitmap_size)))
                        goto cleanup;
                    virBitmapSetAll(bitmap);
                } else {
                    if (!(bitmap = virHostCPUGetAvailableCPUsBitmap()))
                        goto cleanup;
                }
                cpumask = bitmap;
            }
        }
        virBitmapToData(cpumask, &info_ret[i]->cpumap, &info_ret[i]->cpumaplen);
    }

    *info = g_steal_pointer(&info_ret);
    ret = targetDef->niothreadids;

 cleanup:
    if (info_ret) {
        for (i = 0; i < targetDef->niothreadids; i++)
            virDomainIOThreadInfoFree(info_ret[i]);
        VIR_FREE(info_ret);
    }

    return ret;
}

typedef struct _virDomainDriverAutoStartState {
    virDomainDriverAutoStartConfig *cfg;
    bool first;
} virDomainDriverAutoStartState;

static int
virDomainDriverAutoStartOne(virDomainObj *vm,
                            void *opaque)
{
    virDomainDriverAutoStartState *state = opaque;

    virObjectLock(vm);
    virObjectRef(vm);

    VIR_DEBUG("Autostart %s: autostart=%d autostartOnce=%d autostartOnceLink=%s",
              vm->def->name, vm->autostart, vm->autostartOnce,
              NULLSTR(vm->autostartOnceLink));

    if ((vm->autostart || vm->autostartOnce) &&
        !virDomainObjIsActive(vm)) {
        virResetLastError();
        if (state->cfg->delayMS) {
            if (!state->first) {
                g_usleep(state->cfg->delayMS * 1000ull);
            } else {
                state->first = false;
            }
        }

        state->cfg->callback(vm, state->cfg->opaque);
        vm->autostartOnce = 0;
    }

    if (vm->autostartOnceLink) {
        unlink(vm->autostartOnceLink);
        g_clear_pointer(&vm->autostartOnceLink, g_free);
    }

    virDomainObjEndAPI(&vm);
    virResetLastError();

    return 0;
}

void
virDomainDriverAutoStart(virDomainObjList *domains,
                         virDomainDriverAutoStartConfig *cfg)
{
    virDomainDriverAutoStartState state = { .cfg = cfg, .first = true };
    bool autostart;
    VIR_DEBUG("Run autostart stateDir=%s", cfg->stateDir);
    if (virDriverShouldAutostart(cfg->stateDir, &autostart) < 0 ||
        !autostart) {
        VIR_DEBUG("Autostart already processed");
        return;
    }

    virDomainObjListForEach(domains, false, virDomainDriverAutoStartOne, &state);
}


bool
virDomainDriverAutoShutdownActive(virDomainDriverAutoShutdownConfig *cfg)
{
    return cfg->trySave != VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_NONE ||
        cfg->tryShutdown != VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_NONE ||
        cfg->poweroff != VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_NONE;
}


enum {
    VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SAVE = 1 << 1,
    VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SHUTDOWN = 1 << 2,
    VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_POWEROFF = 1 << 3,
    VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_RESTORE = 1 << 4,
} virDomainDriverAutoShutdownModeFlag;


static void
virDomainDriverAutoShutdownDoSave(virDomainPtr *domains,
                                  unsigned int *modes,
                                  size_t numDomains,
                                  virDomainDriverAutoShutdownConfig *cfg)
{
    g_autofree unsigned int *flags = g_new0(unsigned int, numDomains);
    bool hasSave = false;
    size_t i;

    for (i = 0; i < numDomains; i++) {
        int state;

        if (!(modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SAVE))
            continue;

        hasSave = true;

        virSystemdNotifyStatus("Suspending '%s' (%zu of %zu)",
                               virDomainGetName(domains[i]), i + 1, numDomains);
        VIR_INFO("Suspending '%s'", virDomainGetName(domains[i]));

        /*
         * Pause all VMs to make them stop dirtying pages,
         * so save is quicker. We remember if any VMs were
         * paused so we can restore that on resume.
         */
        flags[i] = VIR_DOMAIN_SAVE_RUNNING;
        if (virDomainGetState(domains[i], &state, NULL, 0) == 0) {
            if (state == VIR_DOMAIN_PAUSED)
                flags[i] = VIR_DOMAIN_SAVE_PAUSED;
        }
        if (cfg->saveBypassCache)
            flags[i] |= VIR_DOMAIN_SAVE_BYPASS_CACHE;

        if (flags[i] & VIR_DOMAIN_SAVE_RUNNING)
            virDomainSuspend(domains[i]);
    }

    if (!hasSave)
        return;

    for (i = 0; i < numDomains; i++) {
        if (!(modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SAVE))
            continue;

        virSystemdNotifyStatus("Saving '%s' (%zu of %zu)",
                               virDomainGetName(domains[i]), i + 1, numDomains);
        VIR_INFO("Saving '%s'", virDomainGetName(domains[i]));

        if (virDomainManagedSave(domains[i], flags[i]) < 0) {
            VIR_WARN("auto-shutdown: unable to perform managed save of '%s': %s",
                     domains[i]->name,
                     virGetLastErrorMessage());
            if (flags[i] & VIR_DOMAIN_SAVE_RUNNING)
                virDomainResume(domains[i]);
            continue;
        }

        modes[i] = 0;
    }
}


static void
virDomainDriverAutoShutdownDoShutdown(virDomainPtr *domains,
                                      unsigned int *modes,
                                      size_t numDomains,
                                      virDomainDriverAutoShutdownConfig *cfg)
{
    GTimer *timer = NULL;
    bool hasShutdown = false;
    size_t i;

    for (i = 0; i < numDomains; i++) {
        if (!(modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SHUTDOWN))
            continue;

        hasShutdown = true;

        virSystemdNotifyStatus("Shutting down '%s' (%zu of %zu)",
                               virDomainGetName(domains[i]), i + 1, numDomains);
        VIR_INFO("Shutting down '%s'", virDomainGetName(domains[i]));

        if (virDomainShutdown(domains[i]) < 0) {
            VIR_WARN("auto-shutdown: unable to request graceful shutdown of '%s': %s",
                     domains[i]->name,
                     virGetLastErrorMessage());
            break;
        }
    }

    if (!hasShutdown)
        return;

    timer = g_timer_new();
    virSystemdNotifyStatus("Waiting %u secs for VM shutdown completion",
                           cfg->waitShutdownSecs);
    VIR_INFO("Waiting %u secs for VM shutdown completion", cfg->waitShutdownSecs);

    while (1) {
        bool anyRunning = false;
        for (i = 0; i < numDomains; i++) {
            if (!(modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SHUTDOWN))
                continue;

            if (virDomainIsActive(domains[i]) == 1) {
                anyRunning = true;
            } else {
                modes[i] = 0;
            }
        }

        if (!anyRunning)
            break;
        if (g_timer_elapsed(timer, NULL) > cfg->waitShutdownSecs)
            break;
        g_usleep(1000*500);
    }
    g_timer_destroy(timer);
}


static void
virDomainDriverAutoShutdownDoPoweroff(virDomainPtr *domains,
                                      unsigned int *modes,
                                      size_t numDomains)
{
    size_t i;

    for (i = 0; i < numDomains; i++) {
        if (!(modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_POWEROFF))
            continue;

        virSystemdNotifyStatus("Destroying '%s' (%zu of %zu)",
                               virDomainGetName(domains[i]), i + 1, numDomains);
        VIR_INFO("Destroying '%s'", virDomainGetName(domains[i]));
        /*
         * NB might fail if we gave up on waiting for
         * virDomainShutdown, but it then completed anyway,
         * hence we're not checking for failure
         */
        virDomainDestroy(domains[i]);

        modes[i] = 0;
    }
}

static unsigned int
virDomainDriverAutoShutdownGetMode(virDomainPtr domain,
                                   virDomainDriverAutoShutdownConfig *cfg)
{
    unsigned int mode = 0;

    if (virDomainIsPersistent(domain) != 0) {
        if (cfg->trySave == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
            cfg->trySave == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_PERSISTENT)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SAVE;

        if (cfg->tryShutdown == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
            cfg->tryShutdown == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_PERSISTENT)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SHUTDOWN;

        if (cfg->poweroff == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
            cfg->poweroff == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_PERSISTENT)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_POWEROFF;

        /* Don't restore VMs which weren't selected for auto-shutdown */
        if (mode != 0 && cfg->autoRestore)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_RESTORE;
    } else {
        if (cfg->tryShutdown == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
            cfg->tryShutdown == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_TRANSIENT)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_SHUTDOWN;

        if (cfg->poweroff == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
            cfg->poweroff == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_TRANSIENT)
            mode |= VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_POWEROFF;

        if (cfg->autoRestore)
            VIR_DEBUG("Cannot auto-restore transient VM '%s'",
                      virDomainGetName(domain));
    }

    return mode;
}


void
virDomainDriverAutoShutdown(virDomainDriverAutoShutdownConfig *cfg)
{
    g_autoptr(virConnect) conn = NULL;
    int numDomains = 0;
    size_t i;
    virDomainPtr *domains = NULL;
    g_autofree unsigned int *modes = NULL;

    VIR_DEBUG("Run autoshutdown uri=%s trySave=%s tryShutdown=%s poweroff=%s waitShutdownSecs=%u saveBypassCache=%d autoRestore=%d",
              cfg->uri,
              virDomainDriverAutoShutdownScopeTypeToString(cfg->trySave),
              virDomainDriverAutoShutdownScopeTypeToString(cfg->tryShutdown),
              virDomainDriverAutoShutdownScopeTypeToString(cfg->poweroff),
              cfg->waitShutdownSecs, cfg->saveBypassCache, cfg->autoRestore);

    /*
     * Ideally guests will shutdown in a few seconds, but it would
     * not be unsual for it to take a while longer, especially under
     * load, or if the guest OS has inhibitors slowing down shutdown.
     *
     * If we wait too long, then guests which ignore the shutdown
     * request will significantly delay host shutdown.
     *
     * Pick 30 seconds as a moderately safe default, assuming that
     * most guests are well behaved.
     */
    if (cfg->waitShutdownSecs <= 0)
        cfg->waitShutdownSecs = 30;

    /* We expect the hypervisor driver to enforce this when loading
     * their config file, but in case some bad setting gets through,
     * disabling saving of transient VMs since it is a guaranteed
     * error scenario...
     */
    if (cfg->trySave == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ||
        cfg->trySave == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_TRANSIENT) {
        VIR_WARN("auto-shutdown: managed save not supported for transient VMs");
        cfg->trySave = (cfg->trySave == VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_ALL ?
                        VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_PERSISTENT :
                        VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_SCOPE_NONE);
    }

    /* Short-circuit if all actions are disabled */
    if (!virDomainDriverAutoShutdownActive(cfg))
        return;

    if (!(conn = virConnectOpen(cfg->uri)))
        return;

    if ((numDomains = virConnectListAllDomains(conn,
                                               &domains,
                                               VIR_CONNECT_LIST_DOMAINS_ACTIVE)) < 0)
        return;

    VIR_DEBUG("Auto shutdown with %d running domains", numDomains);

    modes = g_new0(unsigned int, numDomains);

    for (i = 0; i < numDomains; i++) {
        modes[i] = virDomainDriverAutoShutdownGetMode(domains[i], cfg);

        if (modes[i] == 0) {
            /* VM wasn't selected for any of the shutdown modes. There's not
             * much we can do about that as the host is powering off, logging
             * at least lets admins know */
            VIR_WARN("auto-shutdown: domain '%s' not successfully shut off by any action",
                     domains[i]->name);
        }

        if (modes[i] & VIR_DOMAIN_DRIVER_AUTO_SHUTDOWN_MODE_RESTORE) {
            VIR_DEBUG("Mark '%s' for autostart on next boot",
                      virDomainGetName(domains[i]));
            if (virDomainSetAutostartOnce(domains[i], 1) < 0) {
                VIR_WARN("Unable to mark domain '%s' for auto restore: %s",
                         virDomainGetName(domains[i]),
                         virGetLastErrorMessage());
            }
        }
    }

    virDomainDriverAutoShutdownDoSave(domains, modes, numDomains, cfg);
    virDomainDriverAutoShutdownDoShutdown(domains, modes, numDomains, cfg);
    virDomainDriverAutoShutdownDoPoweroff(domains, modes, numDomains);

    virSystemdNotifyStatus("Processed %d domains", numDomains);
    VIR_INFO("Processed %d domains", numDomains);

    for (i = 0; i < numDomains; i++)
        virObjectUnref(domains[i]);

    VIR_FREE(domains);
}
