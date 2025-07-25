/*
 * bhyve_process.c: bhyve process management
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2014 Roman Bogorodskiy
 * Copyright (C) 2025 The FreeBSD Foundation
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
 *
 */

#include <config.h>

#include <fcntl.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <net/if.h>
#include <net/if_tap.h>

#include "bhyve_device.h"
#include "bhyve_driver.h"
#include "bhyve_command.h"
#include "bhyve_firmware.h"
#include "bhyve_monitor.h"
#include "bhyve_process.h"
#include "datatypes.h"
#include "virerror.h"
#include "virhook.h"
#include "virlog.h"
#include "virfile.h"
#include "viralloc.h"
#include "vircommand.h"
#include "virstring.h"
#include "virpidfile.h"
#include "virprocess.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"

#define VIR_FROM_THIS   VIR_FROM_BHYVE

VIR_LOG_INIT("bhyve.bhyve_process");

static void
bhyveProcessAutoDestroy(virDomainObj *vm,
                        virConnectPtr conn G_GNUC_UNUSED)
{
    bhyveDomainObjPrivate *priv = vm->privateData;
    struct _bhyveConn *driver = priv->driver;

    virBhyveProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED);

    if (!vm->persistent)
        virDomainObjListRemove(driver->domains, vm);
}

static void
bhyveNetCleanup(virDomainObj *vm)
{
    size_t i;
    g_autoptr(virConnect) conn = NULL;

    for (i = 0; i < vm->def->nnets; i++) {
        virDomainNetDef *net = vm->def->nets[i];
        virDomainNetType actualType = virDomainNetGetActualType(net);

        if (net->ifname) {
            ignore_value(virNetDevBridgeRemovePort(
                             virDomainNetGetActualBridgeName(net),
                             net->ifname));
            ignore_value(virNetDevTapDelete(net->ifname, NULL));
        }
        if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK) {
            if (conn || (conn = virGetConnectNetwork()))
                virDomainNetReleaseActualDevice(conn, net);
            else
                VIR_WARN("Unable to release network device '%s'", NULLSTR(net->ifname));
        }
    }
}

static void
virBhyveFormatDevMapFile(const char *vm_name, char **fn_out)
{
    *fn_out = g_strdup_printf("%s/grub_bhyve-%s-device.map", BHYVE_STATE_DIR, vm_name);
}

static int
bhyveProcessStartHook(struct _bhyveConn *driver,
                      virDomainObj *vm,
                      virHookBhyveOpType op)
{
    g_autofree char *xml = NULL;

    if (!virHookPresent(VIR_HOOK_DRIVER_BHYVE))
        return 0;

    xml = virDomainDefFormat(vm->def, driver->xmlopt, 0);

    return virHookCall(VIR_HOOK_DRIVER_BHYVE, vm->def->name, op,
                       VIR_HOOK_SUBOP_BEGIN, NULL, xml, NULL);
}

static void
bhyveProcessStopHook(struct _bhyveConn *driver,
                     virDomainObj *vm,
                     virHookBhyveOpType op)
{
    g_autofree char *xml = NULL;
    if (!virHookPresent(VIR_HOOK_DRIVER_BHYVE))
        return;

    xml = virDomainDefFormat(vm->def, driver->xmlopt, 0);

    virHookCall(VIR_HOOK_DRIVER_BHYVE, vm->def->name, op,
                VIR_HOOK_SUBOP_END, NULL, xml, NULL);
}

static int
virBhyveProcessStartImpl(struct _bhyveConn *driver,
                         virDomainObj *vm,
                         virDomainRunningReason reason)
{
    g_autofree char *devmap_file = NULL;
    g_autofree char *devicemap = NULL;
    g_autofree char *logfile = NULL;
    VIR_AUTOCLOSE logfd = -1;
    g_autoptr(virCommand) cmd = NULL;
    g_autoptr(virCommand) load_cmd = NULL;
    bhyveDomainObjPrivate *priv = vm->privateData;
    int ret = -1, rc;

    logfile = g_strdup_printf("%s/%s.log", BHYVE_LOG_DIR, vm->def->name);
    if ((logfd = open(logfile, O_WRONLY | O_APPEND | O_CREAT,
                      S_IRUSR | S_IWUSR)) < 0) {
        virReportSystemError(errno,
                             _("Failed to open '%1$s'"),
                             logfile);
        goto cleanup;
    }

    VIR_FREE(driver->pidfile);
    if (!(driver->pidfile = virPidFileBuildPath(BHYVE_STATE_DIR,
                                                vm->def->name))) {
        virReportSystemError(errno,
                             "%s", _("Failed to build pidfile path"));
        goto cleanup;
    }

    if (unlink(driver->pidfile) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("Cannot remove stale PID file %1$s"),
                             driver->pidfile);
        goto cleanup;
    }

    if (bhyveDomainAssignAddresses(vm->def, NULL) < 0)
        goto cleanup;

    /* Call bhyve to start the VM */
    if (!(cmd = virBhyveProcessBuildBhyveCmd(driver, vm->def, false)))
        goto cleanup;

    virCommandSetOutputFD(cmd, &logfd);
    virCommandSetErrorFD(cmd, &logfd);
    virCommandWriteArgLog(cmd, logfd);
    virCommandSetPidFile(cmd, driver->pidfile);
    virCommandDaemonize(cmd);

    if (vm->def->os.loader == NULL) {
        /* Now bhyve command is constructed, meaning the
         * domain is ready to be started, so we can build
         * and execute bhyveload command */

        virBhyveFormatDevMapFile(vm->def->name, &devmap_file);

        if (!(load_cmd = virBhyveProcessBuildLoadCmd(driver, vm->def,
                                                     devmap_file, &devicemap)))
            goto cleanup;
        virCommandSetOutputFD(load_cmd, &logfd);
        virCommandSetErrorFD(load_cmd, &logfd);

        if (devicemap != NULL) {
            rc = virFileWriteStr(devmap_file, devicemap, 0644);
            if (rc) {
                virReportSystemError(errno,
                                     _("Cannot write device.map '%1$s'"),
                                     devmap_file);
                goto cleanup;
            }
        }

        /* Log generated command line */
        virCommandWriteArgLog(load_cmd, logfd);

        VIR_DEBUG("Loading domain '%s'", vm->def->name);
        if (virCommandRun(load_cmd, NULL) < 0)
            goto cleanup;
    }

    if (bhyveProcessStartHook(driver, vm, VIR_HOOK_BHYVE_OP_START) < 0)
        goto cleanup;

    /* Now we can start the domain */
    VIR_DEBUG("Starting domain '%s'", vm->def->name);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virPidFileReadPath(driver->pidfile, &vm->pid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Domain %1$s didn't show up"), vm->def->name);
        goto cleanup;
    }

    vm->def->id = vm->pid;
    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, reason);
    priv->mon = bhyveMonitorOpen(vm, driver);

    if (virDomainObjSave(vm, driver->xmlopt,
                         BHYVE_STATE_DIR) < 0)
        goto cleanup;

    if (bhyveProcessStartHook(driver, vm, VIR_HOOK_BHYVE_OP_STARTED) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (devicemap != NULL) {
        rc = unlink(devmap_file);
        if (rc < 0 && errno != ENOENT)
            virReportSystemError(errno, _("cannot unlink file '%1$s'"),
                                 devmap_file);
    }

    if (ret < 0) {
        int exitstatus; /* Needed to avoid logging non-zero status */
        g_autoptr(virCommand) destroy_cmd = NULL;
        if ((destroy_cmd = virBhyveProcessBuildDestroyCmd(driver,
                                                          vm->def)) != NULL) {
            virCommandSetOutputFD(load_cmd, &logfd);
            virCommandSetErrorFD(load_cmd, &logfd);
            ignore_value(virCommandRun(destroy_cmd, &exitstatus));
        }

        bhyveNetCleanup(vm);
    }

    return ret;
}

int
bhyveProcessPrepareDomain(bhyveConn *driver,
                          virDomainObj *vm,
                          unsigned int flags)
{
    if (bhyveFirmwareFillDomain(driver, vm->def, flags) < 0)
        return -1;

    return 0;
}


struct bhyvePrepareNVRAMHelperData {
    int srcFD;
    const char *srcPath;
};


static int
bhyvePrepareNVRAMHelper(int dstFD,
                        const char *dstPath,
                        const void *opaque)
{
    const struct bhyvePrepareNVRAMHelperData *data = opaque;
    ssize_t r;

    do {
        char buf[1024];

        if ((r = saferead(data->srcFD, buf, sizeof(buf))) < 0) {
            virReportSystemError(errno,
                                 _("Unable to read from file '%1$s'"),
                                 data->srcPath);
            return -2;
        }

        if (safewrite(dstFD, buf, r) < 0) {
            virReportSystemError(errno,
                                 _("Unable to write to file '%1$s'"),
                                 dstPath);
            return -1;
        }
    } while (r);

    return 0;
}


static int
bhyvePrepareNVRAMFile(bhyveConn *driver G_GNUC_UNUSED,
                      virDomainLoaderDef *loader)
{
    VIR_AUTOCLOSE srcFD = -1;
    struct bhyvePrepareNVRAMHelperData data;

    if (virFileExists(loader->nvram->path))
        return 0;

    if (!loader->nvramTemplate) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("unable to find any master var store for loader: %1$s"),
                       loader->path);
        return -1;
    }

    /* If 'nvramTemplateFormat' is empty it means that it's a user-provided
     * template which we couldn't verify. Assume the user knows what they're doing */
    if (loader->nvramTemplateFormat != VIR_STORAGE_FILE_NONE &&
        loader->nvram->format != loader->nvramTemplateFormat) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("conversion of the nvram template to another target format is not supported"));
        return -1;
    }

    if ((srcFD = virFileOpenAs(loader->nvramTemplate, O_RDONLY,
                               0, -1, -1, 0)) < 0) {
        virReportSystemError(-srcFD,
                             _("Failed to open file '%1$s'"),
                             loader->nvramTemplate);
        return -1;
    }

    data.srcFD = srcFD;
    data.srcPath = loader->nvramTemplate;

    if (virFileRewrite(loader->nvram->path,
                       S_IRUSR | S_IWUSR,
                       0, 0,
                       bhyvePrepareNVRAMHelper,
                       &data) < 0) {
        return -1;
    }

    return 0;
}


int
bhyvePrepareNVRAM(bhyveConn *driver,
                  virDomainDef *def)
{
    virDomainLoaderDef *loader = def->os.loader;

    if (!loader || !loader->nvram)
        return 0;

    VIR_DEBUG("nvram='%s'", NULLSTR(loader->nvram->path));

    switch (virStorageSourceGetActualType(loader->nvram)) {
    case VIR_STORAGE_TYPE_FILE:
        return bhyvePrepareNVRAMFile(driver, loader);
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_NETWORK:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_VHOST_USER:
    case VIR_STORAGE_TYPE_VHOST_VDPA:
    case VIR_STORAGE_TYPE_LAST:
    case VIR_STORAGE_TYPE_NONE:
        break;
    }

    return 0;
}

int
bhyveProcessPrepareHost(bhyveConn *driver,
                        virDomainDef *def,
                        unsigned int flags)
{
    virCheckFlags(0, -1);

    if (bhyvePrepareNVRAM(driver, def) < 0)
        return -1;

    return 0;
}

int
virBhyveProcessStart(bhyveConn *driver,
                     virConnectPtr conn,
                     virDomainObj *vm,
                     virDomainRunningReason reason,
                     unsigned int flags)
{
    /* Run an early hook to setup missing devices. */
    if (bhyveProcessStartHook(driver, vm, VIR_HOOK_BHYVE_OP_PREPARE) < 0)
        return -1;

    if (flags & VIR_BHYVE_PROCESS_START_AUTODESTROY)
        virCloseCallbacksDomainAdd(vm, conn, bhyveProcessAutoDestroy);

    if (bhyveProcessPrepareDomain(driver, vm, flags) < 0)
        return -1;

    if (bhyveProcessPrepareHost(driver, vm->def, flags) < 0)
        return -1;

    return virBhyveProcessStartImpl(driver, vm, reason);
}

static void
bhyveProcessRemoveDomainStatus(const char *statusDir,
                               const char *name)
{
    g_autofree char *file = virDomainConfigFile(statusDir, name);

    if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR) {
        VIR_WARN("Failed to remove domain XML for %s: %s",
                 name, g_strerror(errno));
    }
}

int
virBhyveProcessStop(struct _bhyveConn *driver,
                    virDomainObj *vm,
                    virDomainShutoffReason reason)
{
    int ret = -1;
    g_autoptr(virCommand) cmd = NULL;
    bhyveDomainObjPrivate *priv = vm->privateData;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("VM '%s' not active", vm->def->name);
        return 0;
    }

    if (vm->pid == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid PID %1$d for VM"),
                       (int)vm->pid);
        return -1;
    }

    if (!(cmd = virBhyveProcessBuildDestroyCmd(driver, vm->def)))
        return -1;

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if ((priv != NULL) && (priv->mon != NULL))
         bhyveMonitorClose(priv->mon);

    bhyveProcessStopHook(driver, vm, VIR_HOOK_BHYVE_OP_STOPPED);

    /* Cleanup network interfaces */
    bhyveNetCleanup(vm);

    /* VNC autoport cleanup */
    if ((vm->def->ngraphics == 1) &&
        vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        if (virPortAllocatorRelease(vm->def->graphics[0]->data.vnc.port) < 0) {
            VIR_WARN("Failed to release VNC port for '%s'",
                     vm->def->name);
        }
    }

    ret = 0;

    virCloseCallbacksDomainRemove(vm, NULL, bhyveProcessAutoDestroy);

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);
    vm->pid = 0;
    vm->def->id = -1;

    bhyveProcessStopHook(driver, vm, VIR_HOOK_BHYVE_OP_RELEASE);

 cleanup:
    virPidFileDelete(BHYVE_STATE_DIR, vm->def->name);
    bhyveProcessRemoveDomainStatus(BHYVE_STATE_DIR, vm->def->name);

    return ret;
}

int
virBhyveProcessShutdown(virDomainObj *vm)
{
    if (vm->pid == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid PID %1$d for VM"),
                       (int)vm->pid);
        return -1;
    }

    /* Bhyve tries to perform ACPI shutdown when it receives
     * SIGTERM signal. So we just issue SIGTERM here and rely
     * on the bhyve monitor to clean things up if process disappears.
     */
    if (virProcessKill(vm->pid, SIGTERM) != 0) {
        VIR_WARN("Failed to terminate bhyve process for VM '%s': %s",
                 vm->def->name, virGetLastErrorMessage());
        return -1;
    }

    return 0;
}

int
virBhyveProcessRestart(struct _bhyveConn *driver,
                       virDomainObj *vm)
{
    if (virBhyveProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN) < 0)
        return -1;

    if (virBhyveProcessStartImpl(driver, vm, VIR_DOMAIN_RUNNING_BOOTED) < 0)
        return -1;

    return 0;
}

int
virBhyveGetDomainTotalCpuStats(virDomainObj *vm,
                               unsigned long long *cpustats)
{
    struct kinfo_proc *kp;
    kvm_t *kd;
    g_autofree char *errbuf = g_new0(char, _POSIX2_LINE_MAX);
    int nprocs;
    int ret = -1;

    if ((kd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, errbuf)) == NULL) {
        virReportError(VIR_ERR_SYSTEM_ERROR,
                       _("Unable to get kvm descriptor: %1$s"),
                       errbuf);
        return -1;

    }

    kp = kvm_getprocs(kd, KERN_PROC_PID, vm->pid, &nprocs);
    if (kp == NULL || nprocs != 1) {
        virReportError(VIR_ERR_SYSTEM_ERROR,
                       _("Unable to obtain information about pid: %1$d"),
                       (int)vm->pid);
        goto cleanup;
    }

    *cpustats = kp->ki_runtime * 1000ull;

    ret = 0;

 cleanup:
    kvm_close(kd);

    return ret;
}

struct bhyveProcessReconnectData {
    struct _bhyveConn *driver;
    kvm_t *kd;
};

static int
virBhyveProcessReconnect(virDomainObj *vm,
                         void *opaque)
{
    struct bhyveProcessReconnectData *data = opaque;
    struct kinfo_proc *kp;
    int nprocs;
    char **proc_argv;
    char *expected_proctitle = NULL;
    bhyveDomainObjPrivate *priv = vm->privateData;
    g_autoptr(virConnect) conn = NULL;
    size_t i;
    int ret = -1;

    if (!virDomainObjIsActive(vm))
        return 0;

    if (vm->pid == 0)
        return 0;

    virObjectLock(vm);

    kp = kvm_getprocs(data->kd, KERN_PROC_PID, vm->pid, &nprocs);
    if (kp == NULL || nprocs != 1)
        goto cleanup;

    expected_proctitle = g_strdup_printf("bhyve: %s", vm->def->name);

    proc_argv = kvm_getargv(data->kd, kp, 0);
    if (proc_argv && proc_argv[0]) {
         if (STREQ(expected_proctitle, proc_argv[0])) {
             ret = 0;
             priv->mon = bhyveMonitorOpen(vm, data->driver);
             if (vm->def->ngraphics == 1 &&
                 vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
                 int vnc_port = vm->def->graphics[0]->data.vnc.port;
                 if (virPortAllocatorSetUsed(vnc_port) < 0) {
                     VIR_WARN("Failed to mark VNC port '%d' as used by '%s'",
                              vnc_port, vm->def->name);
                 }
             }
         }
    }

    for (i = 0; i < vm->def->nnets; i++) {
        virDomainNetDef *net = vm->def->nets[i];
        if (net->type == VIR_DOMAIN_NET_TYPE_NETWORK && !conn)
            conn = virGetConnectNetwork();

        virDomainNetNotifyActualDevice(conn, vm->def, net);
    }

 cleanup:
    if (ret < 0) {
        /* If VM is reported to be in active state, but we cannot find
         * its PID, then we clear information about the PID and
         * set state to 'shutdown' */
        vm->pid = 0;
        vm->def->id = -1;
        virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF,
                             VIR_DOMAIN_SHUTOFF_UNKNOWN);
        ignore_value(virDomainObjSave(vm, data->driver->xmlopt,
                                      BHYVE_STATE_DIR));
    }

    virObjectUnlock(vm);
    VIR_FREE(expected_proctitle);

    return ret;
}

void
virBhyveProcessReconnectAll(struct _bhyveConn *driver)
{
    kvm_t *kd;
    struct bhyveProcessReconnectData data;
    g_autofree char *errbuf = g_new0(char, _POSIX2_LINE_MAX);

    if ((kd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, errbuf)) == NULL) {
        virReportError(VIR_ERR_SYSTEM_ERROR,
                       _("Unable to get kvm descriptor: %1$s"),
                       errbuf);
        return;

    }

    data.driver = driver;
    data.kd = kd;

    virDomainObjListForEach(driver->domains, false, virBhyveProcessReconnect, &data);

    kvm_close(kd);
}
