/*
 * virsh.h: a shell to exercise the libvirt API
 *
 * Copyright (C) 2005, 2007-2016 Red Hat, Inc.
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

#pragma once

#include <unistd.h>

#include "internal.h"
#include "virpolkit.h"
#include "vsh.h"
#include "virsh-completer.h"
#include "virenum.h"

#define VIRSH_PROMPT_RW    "virsh # "
#define VIRSH_PROMPT_RO    "virsh > "

#define VIR_FROM_THIS VIR_FROM_NONE

/*
 * Command group types
 */
#define VIRSH_CMD_GRP_CHECKPOINT       "Checkpoint"
#define VIRSH_CMD_GRP_DOM_MANAGEMENT   "Domain Management"
#define VIRSH_CMD_GRP_DOM_MONITORING   "Domain Monitoring"
#define VIRSH_CMD_GRP_DOM_EVENTS       "Domain Events"
#define VIRSH_CMD_GRP_STORAGE_POOL     "Storage Pool"
#define VIRSH_CMD_GRP_STORAGE_VOL      "Storage Volume"
#define VIRSH_CMD_GRP_NETWORK          "Networking"
#define VIRSH_CMD_GRP_NODEDEV          "Node Device"
#define VIRSH_CMD_GRP_IFACE            "Interface"
#define VIRSH_CMD_GRP_NWFILTER         "Network Filter"
#define VIRSH_CMD_GRP_SECRET           "Secret"
#define VIRSH_CMD_GRP_SNAPSHOT         "Snapshot"
#define VIRSH_CMD_GRP_BACKUP           "Backup"
#define VIRSH_CMD_GRP_HOST_AND_HV      "Host and Hypervisor"
#define VIRSH_CMD_GRP_VIRSH            "Virsh itself"

/*
 * Common command options
 */
#define VIRSH_COMMON_OPT_POOL(_helpstr, cflags) \
    {.name = "pool", \
     .type = VSH_OT_STRING, \
     .positional = true, \
     .required = true, \
     .help = _helpstr, \
     .completer = virshStoragePoolNameCompleter, \
     .completer_flags = cflags, \
    }

#define VIRSH_COMMON_OPT_DOMAIN(_helpstr, cflags) \
    {.name = "domain", \
     .type = VSH_OT_STRING, \
     .positional = true, \
     .required = true, \
     .help = _helpstr, \
     .completer = virshDomainNameCompleter, \
     .completer_flags = cflags, \
    }

#define VIRSH_COMMON_OPT_DOMAIN_FULL(cflags) \
    VIRSH_COMMON_OPT_DOMAIN(N_("domain name, id or uuid"), cflags)

#define VIRSH_COMMON_OPT_CONFIG(_helpstr) \
    {.name = "config", \
     .type = VSH_OT_BOOL, \
     .help = _helpstr \
    }

#define VIRSH_COMMON_OPT_LIVE(_helpstr) \
    {.name = "live", \
     .type = VSH_OT_BOOL, \
     .help = _helpstr \
    }

#define VIRSH_COMMON_OPT_CURRENT(_helpstr) \
    {.name = "current", \
     .type = VSH_OT_BOOL, \
     .help = _helpstr \
    }

/* Use this only for files which are existing and used locally by virsh */
#define VIRSH_COMMON_OPT_FILE(_helpstr) \
    {.name = "file", \
     .type = VSH_OT_STRING, \
     .required = true, \
     .positional = true, \
     .completer = vshCompletePathLocalExisting, \
     .help = _helpstr \
    }

typedef struct _virshControl virshControl;

typedef struct _virshCtrlData virshCtrlData;

/*
 * vshControl
 */
struct _virshControl {
    virConnectPtr conn;         /* connection to hypervisor (MAY BE NULL) */
    bool readonly;              /* connect readonly (first time only, not
                                 * during explicit connect command)
                                 */
    bool useGetInfo;            /* must use virDomainGetInfo, since
                                   virDomainGetState is not supported */
    bool useSnapshotOld;        /* cannot use virDomainSnapshotGetParent or
                                   virDomainSnapshotNumChildren */
    bool blockJobNoBytes;       /* true if _BANDWIDTH_BYTE blockjob flags
                                   are missing */
    const char *escapeChar;     /* String representation of
                                   console escape character */
};

/* Typedefs, function prototypes for job progress reporting.
 * There are used by some long lingering commands like
 * migrate, dump, save, managedsave.
 */
struct _virshCtrlData {
    vshControl *ctl;
    const vshCmd *cmd;
    GMainLoop *eventLoop;
    int ret;
    virConnectPtr dconn;
};

/* Filter flags for various vshCommandOpt*By() functions */
typedef enum {
    VIRSH_BYID   = (1 << 1),
    VIRSH_BYUUID = (1 << 2),
    VIRSH_BYNAME = (1 << 3),
    VIRSH_BYMAC  = (1 << 4),
} virshLookupByFlags;

virConnectPtr virshConnect(vshControl *ctl, const char *uri, bool readonly);
