/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#include <asm/statfs.h>
#endif
#include <linux/obd.h>
#include <linux/obd_class.h>
#include <linux/lprocfs_status.h>
#include "mds_internal.h"

#ifndef LPROCFS

struct lprocfs_vars lprocfs_mds_obd_vars[]  = { {0} };
struct lprocfs_vars lprocfs_mds_module_vars[] = { {0} };
struct lprocfs_vars lprocfs_mdt_obd_vars[] = { {0} };
struct lprocfs_vars lprocfs_mdt_module_vars[] = { {0} };

#else

static int lprocfs_mds_rd_mntdev(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        struct obd_device* obd = (struct obd_device *)data;

        LASSERT(obd != NULL);
        LASSERT(obd->u.mds.mds_vfsmnt->mnt_devname);
        *eof = 1;

        return snprintf(page, count, "%s\n",
                        obd->u.mds.mds_vfsmnt->mnt_devname);
}

static int lprocfs_mds_wr_evict_client(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct obd_export *doomed_exp = NULL;
        struct obd_uuid doomed;
        struct list_head *p;
        char tmpbuf[sizeof(doomed)];

        sscanf(buffer, "%40s", tmpbuf);
        obd_str2uuid(&doomed, tmpbuf);

        spin_lock(&obd->obd_dev_lock);
        list_for_each(p, &obd->obd_exports) {
                doomed_exp = list_entry(p, struct obd_export, exp_obd_chain);
                if (obd_uuid_equals(&doomed, &doomed_exp->exp_client_uuid)) {
                        class_export_get(doomed_exp);
                        break;
                }
                doomed_exp = NULL;
        }
        spin_unlock(&obd->obd_dev_lock);

        if (doomed_exp == NULL) {
                CERROR("can't disconnect %s: no export found\n",
                       doomed.uuid);
        } else {
                CERROR("evicting %s at adminstrative request\n",
                       doomed.uuid);
                ptlrpc_fail_export(doomed_exp);
                class_export_put(doomed_exp);
        }
        return count;
}

static int lprocfs_mds_wr_config_update(struct file *file, const char *buffer,
                                        unsigned long count, void *data)
{
        struct obd_device *obd = data;
        ENTRY;

        mds_dt_update_config(obd, 0);
        RETURN(count);
}

static int lprocfs_rd_filesopen(char *page, char **start, off_t off,
                                int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        LASSERT(obd != NULL);
        *eof = 1;

        return snprintf(page, count, "%d\n",
                        atomic_read(&obd->u.mds.mds_open_count));
}

static int lprocfs_rd_last_fid(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct mds_obd *mds = &obd->u.mds;
        __u64 last_fid;

        spin_lock(&mds->mds_last_fid_lock);
        last_fid = mds->mds_last_fid;
        spin_unlock(&mds->mds_last_fid_lock);

        *eof = 1;
        return snprintf(page, count, LPD64"\n", last_fid);
}

static int lprocfs_rd_group(char *page, char **start, off_t off,
                            int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct mds_obd *mds = &obd->u.mds;

        *eof = 1;
        return snprintf(page, count, "%lu\n",
                        (unsigned long)mds->mds_num);
}

static int lprocfs_rd_capa(char *page, char **start, off_t off,
                           int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%d\n",
                        obd->u.mds.mds_capa_stat);
}

static int lprocfs_wr_capa(struct file *file, const char *buffer,
                           unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mds_update_capa_stat(obd, val);
        return count;
}

static int lprocfs_rd_capa_timeout(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%lu\n",
                        obd->u.mds.mds_capa_timeout);
}

static int lprocfs_wr_capa_timeout(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mds_update_capa_timeout(obd, val);
        return count;
}

static int lprocfs_rd_capa_key_timeout(char *page, char **start, off_t off,
                                           int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%lu\n",
                        obd->u.mds.mds_capa_key_timeout);
}

static int lprocfs_wr_capa_key_timeout(struct file *file, const char *buffer,
                                           unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        rc = mds_update_capa_key_timeout(obd, val);
        return rc ?: count;
}

struct lprocfs_vars lprocfs_mds_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { "blocksize",    lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",  lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",   lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",  lprocfs_rd_kbytesavail, 0, 0 },
        { "fstype",       lprocfs_rd_fstype,      0, 0 },
        { "filestotal",   lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",    lprocfs_rd_filesfree,   0, 0 },
        { "filesopen",    lprocfs_rd_filesopen,   0, 0 },
        { "mntdev",       lprocfs_mds_rd_mntdev,  0, 0 },
        { "last_fid",     lprocfs_rd_last_fid,    0, 0 },
        { "group",        lprocfs_rd_group,       0, 0 },
        { "recovery_status",  lprocfs_obd_rd_recovery_status, 0, 0 },
        { "evict_client", 0,  lprocfs_mds_wr_evict_client, 0 },
        { "config_update", 0, lprocfs_mds_wr_config_update, 0 },
        { "num_exports",      lprocfs_rd_num_exports, 0, 0 },
        { "capa",             lprocfs_rd_capa,
                              lprocfs_wr_capa, 0 },
        { "capa_timeout",     lprocfs_rd_capa_timeout,
                              lprocfs_wr_capa_timeout, 0 },
        { "capa_key_timeout", lprocfs_rd_capa_key_timeout,
                              lprocfs_wr_capa_key_timeout, 0 },
        { 0 }
};

/*
 * LSD proc entry handlers
 */
static int lprocfs_wr_lsd_downcall(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct upcall_cache *cache = __mds_get_global_lsd_cache();
        struct lsd_downcall_args param;
        gid_t   gids_local[NGROUPS_SMALL];
        gid_t  *gids = NULL;

        if (count != sizeof(param)) {
                CERROR("invalid data size %lu\n", count);
                goto do_err_downcall;
        }
        if (copy_from_user(&param, buffer, count)) {
                CERROR("broken downcall\n");
                goto do_err_downcall;
        }

        if (param.err) {
                CERROR("LSD downcall indicate error %d\n", param.err);
                goto do_downcall;
        }

        if (param.ngroups > NGROUPS_MAX) {
                CERROR("%d groups too big\n", param.ngroups);
                goto do_err_downcall;
        }

        if (param.ngroups <= NGROUPS_SMALL)
                gids = gids_local;
        else {
                OBD_ALLOC(gids, param.ngroups * sizeof(gid_t));
                if (!gids) {
                        CERROR("fail to alloc memory for %d gids\n",
                                param.ngroups);
                        goto do_err_downcall;
                }
        }
        if (copy_from_user(gids, param.groups,
                           param.ngroups * sizeof(gid_t))) {
                CERROR("broken downcall\n");
                goto do_err_downcall;
        }

        param.groups = gids;

do_downcall:
        upcall_cache_downcall(cache, (__u64) param.uid, &param);

        if (gids && gids != gids_local)
                OBD_FREE(gids, param.ngroups * sizeof(gid_t));
        return count;

do_err_downcall:
        param.err = -EINVAL;
        goto do_downcall;
}

static int lprocfs_rd_lsd_expire(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();

        *eof = 1;
        return snprintf(page, count, "%lu\n", cache->uc_entry_expire);
}
static int lprocfs_wr_lsd_expire(struct file *file, const char *buffer,
                                 unsigned long count, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();
        char buf[32];

        if (copy_from_user(buf, buffer, min(count, 32UL)))
                return count;
        buf[31] = 0;
        sscanf(buf, "%lu", &cache->uc_entry_expire);
        return count;
}

static int lprocfs_rd_lsd_ac_expire(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();

        *eof = 1;
        return snprintf(page, count, "%lu\n", cache->uc_acquire_expire);
}
static int lprocfs_wr_lsd_ac_expire(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();
        char buf[32];

        if (copy_from_user(buf, buffer, min(count, 32UL)))
                return count;
        buf[31] = 0;
        sscanf(buf, "%lu", &cache->uc_acquire_expire);
        return count;
}

static int lprocfs_rd_lsd_upcall(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();

        *eof = 1;
        return snprintf(page, count, "%s\n", cache->uc_upcall);
}
static int lprocfs_wr_lsd_upcall(struct file *file, const char *buffer,
                                 unsigned long count, void *data)
{
        struct upcall_cache *cache= __mds_get_global_lsd_cache();

        if (count < UC_CACHE_UPCALL_MAXPATH) {
                sscanf(buffer, "%1024s", cache->uc_upcall);
                cache->uc_upcall[UC_CACHE_UPCALL_MAXPATH - 1] = 0;
        }
        return count;
}

extern void lgss_svc_cache_flush(__u32 uid);
static int lprocfs_wr_lsd_flush(struct file *file, const char *buffer,
                                unsigned long count, void *data)
{
        char buf[32];
        __u32 uid;

        if (copy_from_user(buf, buffer, min(count, 32UL)))
                return count;
        buf[31] = 0;
        sscanf(buf, "%d", &uid);

        mds_flush_lsd(uid);
#ifdef ENABLE_GSS
        lgss_svc_cache_flush(uid);
#endif
        return count;
}

/*
 * remote acl proc handling
 */
static int lprocfs_rd_lacl_upcall(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
        struct upcall_cache *cache = __mds_get_global_rmtacl_upcall_cache();

        *eof = 1;
        return snprintf(page, count, "%s\n", cache->uc_upcall);
}

static int lprocfs_wr_lacl_upcall(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct upcall_cache *cache = __mds_get_global_rmtacl_upcall_cache();

        if (count < UC_CACHE_UPCALL_MAXPATH) {
                sscanf(buffer, "%1024s", cache->uc_upcall);
                cache->uc_upcall[UC_CACHE_UPCALL_MAXPATH - 1] = 0;
        }
        return count;
}

static int lprocfs_wr_lacl_downcall(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct upcall_cache *cache = __mds_get_global_rmtacl_upcall_cache();
        struct rmtacl_downcall_args param;

        if (count != sizeof(param)) {
                CERROR("invalid data size %lu\n", count);
                goto do_err_downcall;
        }
        if (copy_from_user(&param, buffer, count)) {
                CERROR("broken downcall\n");
                goto do_err_downcall;
        }

do_downcall:
        upcall_cache_downcall(cache, param.key, &param);
        return count;

do_err_downcall:
        memset(&param, 0, sizeof(param));
        param.status = -EINVAL;
        goto do_downcall;
}

struct lprocfs_vars lprocfs_mds_module_vars[] = {
        { "num_refs",                   lprocfs_rd_numrefs, 0, 0 },
        /* LSD stuff */
        { "lsd_expire_interval",        lprocfs_rd_lsd_expire,
                                        lprocfs_wr_lsd_expire, 0},
        { "lsd_acquire_expire",         lprocfs_rd_lsd_ac_expire,
                                        lprocfs_wr_lsd_ac_expire, 0},
        { "lsd_upcall",                 lprocfs_rd_lsd_upcall,
                                        lprocfs_wr_lsd_upcall, 0},
        { "lsd_flush",                  0, lprocfs_wr_lsd_flush, 0},
        { "lsd_downcall",               0, lprocfs_wr_lsd_downcall, 0},
        /* remote acl */
        { "lacl_upcall",                lprocfs_rd_lacl_upcall,
                                        lprocfs_wr_lacl_upcall, 0},
        { "lacl_downcall",              0, lprocfs_wr_lacl_downcall, 0},
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};
#endif /* LPROCFS */

struct lprocfs_static_vars lprocfs_array_vars[] = { {lprocfs_mds_module_vars,
                                                     lprocfs_mds_obd_vars},
                                                    {lprocfs_mdt_module_vars,
                                                     lprocfs_mdt_obd_vars}};

LPROCFS_INIT_MULTI_VARS(lprocfs_array_vars,
                        (sizeof(lprocfs_array_vars) /
                         sizeof(struct lprocfs_static_vars)))
