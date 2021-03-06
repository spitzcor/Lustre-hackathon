/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * cl code shared between vvp and liblustre (and other Lustre clients in the
 * future).
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <libcfs/libcfs.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/quotaops.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>

#include <obd.h>
#include <obd_support.h>
#include <lustre_fid.h>
#include <lustre_dlm.h>
#include <lustre_ver.h>
#include <lustre_mdc.h>
#include <cl_object.h>

#include "llite_internal.h"

static const struct cl_req_operations ccc_req_ops;

/*
 * ccc_ prefix stands for "Common Client Code".
 */

static struct kmem_cache *ccc_thread_kmem;
static struct kmem_cache *ccc_req_kmem;

static struct lu_kmem_descr ccc_caches[] = {
        {
                .ckd_cache = &ccc_thread_kmem,
                .ckd_name  = "ccc_thread_kmem",
                .ckd_size  = sizeof (struct ccc_thread_info),
        },
        {
                .ckd_cache = &ccc_req_kmem,
                .ckd_name  = "ccc_req_kmem",
                .ckd_size  = sizeof (struct ccc_req)
        },
        {
                .ckd_cache = NULL
        }
};

/*****************************************************************************
 *
 * Vvp device and device type functions.
 *
 */

void *ccc_key_init(const struct lu_context *ctx, struct lu_context_key *key)
{
	struct ccc_thread_info *info;

	OBD_SLAB_ALLOC_PTR_GFP(info, ccc_thread_kmem, GFP_NOFS);
	if (info == NULL)
		info = ERR_PTR(-ENOMEM);
	return info;
}

void ccc_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct ccc_thread_info *info = data;
        OBD_SLAB_FREE_PTR(info, ccc_thread_kmem);
}

struct lu_context_key ccc_key = {
        .lct_tags = LCT_CL_THREAD,
        .lct_init = ccc_key_init,
        .lct_fini = ccc_key_fini
};

int ccc_req_init(const struct lu_env *env, struct cl_device *dev,
		 struct cl_req *req)
{
	struct ccc_req *vrq;
	int result;

	OBD_SLAB_ALLOC_PTR_GFP(vrq, ccc_req_kmem, GFP_NOFS);
	if (vrq != NULL) {
		cl_req_slice_add(req, &vrq->crq_cl, dev, &ccc_req_ops);
		result = 0;
	} else
		result = -ENOMEM;
	return result;
}

/**
 * An `emergency' environment used by ccc_inode_fini() when cl_env_get()
 * fails. Access to this environment is serialized by ccc_inode_fini_guard
 * mutex.
 */
static struct lu_env *ccc_inode_fini_env = NULL;

/**
 * A mutex serializing calls to slp_inode_fini() under extreme memory
 * pressure, when environments cannot be allocated.
 */
static DEFINE_MUTEX(ccc_inode_fini_guard);
static int dummy_refcheck;

int ccc_global_init(struct lu_device_type *device_type)
{
        int result;

        result = lu_kmem_init(ccc_caches);
        if (result)
                return result;

        result = lu_device_type_init(device_type);
        if (result)
                goto out_kmem;

        ccc_inode_fini_env = cl_env_alloc(&dummy_refcheck,
                                          LCT_REMEMBER|LCT_NOREF);
        if (IS_ERR(ccc_inode_fini_env)) {
                result = PTR_ERR(ccc_inode_fini_env);
                goto out_device;
        }

        ccc_inode_fini_env->le_ctx.lc_cookie = 0x4;
        return 0;
out_device:
        lu_device_type_fini(device_type);
out_kmem:
        lu_kmem_fini(ccc_caches);
        return result;
}

void ccc_global_fini(struct lu_device_type *device_type)
{
        if (ccc_inode_fini_env != NULL) {
                cl_env_put(ccc_inode_fini_env, &dummy_refcheck);
                ccc_inode_fini_env = NULL;
        }
        lu_device_type_fini(device_type);
        lu_kmem_fini(ccc_caches);
}

static void vvp_object_size_lock(struct cl_object *obj)
{
	struct inode *inode = vvp_object_inode(obj);

	ll_inode_size_lock(inode);
	cl_object_attr_lock(obj);
}

static void vvp_object_size_unlock(struct cl_object *obj)
{
	struct inode *inode = vvp_object_inode(obj);

	cl_object_attr_unlock(obj);
	ll_inode_size_unlock(inode);
}

/*****************************************************************************
 *
 * io operations.
 *
 */

int vvp_io_one_lock_index(const struct lu_env *env, struct cl_io *io,
			  __u32 enqflags, enum cl_lock_mode mode,
			  pgoff_t start, pgoff_t end)
{
	struct vvp_io          *vio   = vvp_env_io(env);
	struct cl_lock_descr   *descr = &vio->vui_link.cill_descr;
	struct cl_object       *obj   = io->ci_obj;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));
	ENTRY;

	CDEBUG(D_VFSTRACE, "lock: %d [%lu, %lu]\n", mode, start, end);

	memset(&vio->vui_link, 0, sizeof vio->vui_link);

	if (vio->vui_fd && (vio->vui_fd->fd_flags & LL_FILE_GROUP_LOCKED)) {
		descr->cld_mode = CLM_GROUP;
		descr->cld_gid  = vio->vui_fd->fd_grouplock.cg_gid;
	} else {
		descr->cld_mode  = mode;
	}

	descr->cld_obj   = obj;
	descr->cld_start = start;
	descr->cld_end   = end;
	descr->cld_enq_flags = enqflags;

	cl_io_lock_add(env, io, &vio->vui_link);

	RETURN(0);
}

void vvp_io_update_iov(const struct lu_env *env,
		       struct vvp_io *vio, struct cl_io *io)
{
	int i;
	size_t size = io->u.ci_rw.crw_count;

	vio->vui_iov_olen = 0;
	if (!cl_is_normalio(env, io) || vio->vui_tot_nrsegs == 0)
		return;

	for (i = 0; i < vio->vui_tot_nrsegs; i++) {
		struct iovec *iv = &vio->vui_iov[i];

		if (iv->iov_len < size) {
			size -= iv->iov_len;
		} else {
			if (iv->iov_len > size) {
				vio->vui_iov_olen = iv->iov_len;
				iv->iov_len = size;
			}
			break;
		}
	}

	vio->vui_nrsegs = i + 1;
	LASSERTF(vio->vui_tot_nrsegs >= vio->vui_nrsegs,
		 "tot_nrsegs: %lu, nrsegs: %lu\n",
		 vio->vui_tot_nrsegs, vio->vui_nrsegs);
}

int vvp_io_one_lock(const struct lu_env *env, struct cl_io *io,
		    __u32 enqflags, enum cl_lock_mode mode,
		    loff_t start, loff_t end)
{
	struct cl_object *obj = io->ci_obj;

	return vvp_io_one_lock_index(env, io, enqflags, mode,
				     cl_index(obj, start), cl_index(obj, end));
}

void vvp_io_end(const struct lu_env *env, const struct cl_io_slice *ios)
{
	CLOBINVRNT(env, ios->cis_io->ci_obj,
		   vvp_object_invariant(ios->cis_io->ci_obj));
}

void vvp_io_advance(const struct lu_env *env,
		    const struct cl_io_slice *ios,
		    size_t nob)
{
	struct vvp_io    *vio = cl2vvp_io(env, ios);
	struct cl_io     *io  = ios->cis_io;
	struct cl_object *obj = ios->cis_io->ci_obj;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));

	if (!cl_is_normalio(env, io))
		return;

	LASSERT(vio->vui_tot_nrsegs >= vio->vui_nrsegs);
	LASSERT(vio->vui_tot_count  >= nob);

	vio->vui_iov        += vio->vui_nrsegs;
	vio->vui_tot_nrsegs -= vio->vui_nrsegs;
	vio->vui_tot_count  -= nob;

	/* update the iov */
	if (vio->vui_iov_olen > 0) {
		struct iovec *iv;

		vio->vui_iov--;
		vio->vui_tot_nrsegs++;
		iv = &vio->vui_iov[0];
		if (io->ci_continue) {
			iv->iov_base += iv->iov_len;
			LASSERT(vio->vui_iov_olen > iv->iov_len);
			iv->iov_len = vio->vui_iov_olen - iv->iov_len;
		} else {
			/* restore the iov_len, in case of restart io. */
			iv->iov_len = vio->vui_iov_olen;
		}
		vio->vui_iov_olen = 0;
	}
}

/**
 * Helper function that if necessary adjusts file size (inode->i_size), when
 * position at the offset \a pos is accessed. File size can be arbitrary stale
 * on a Lustre client, but client at least knows KMS. If accessed area is
 * inside [0, KMS], set file size to KMS, otherwise glimpse file size.
 *
 * Locking: cl_isize_lock is used to serialize changes to inode size and to
 * protect consistency between inode size and cl_object
 * attributes. cl_object_size_lock() protects consistency between cl_attr's of
 * top-object and sub-objects.
 */
int ccc_prep_size(const struct lu_env *env, struct cl_object *obj,
                  struct cl_io *io, loff_t start, size_t count, int *exceed)
{
        struct cl_attr *attr  = ccc_env_thread_attr(env);
	struct inode   *inode = vvp_object_inode(obj);
        loff_t          pos   = start + count - 1;
        loff_t kms;
        int result;

        /*
         * Consistency guarantees: following possibilities exist for the
         * relation between region being accessed and real file size at this
         * moment:
         *
         *  (A): the region is completely inside of the file;
         *
         *  (B-x): x bytes of region are inside of the file, the rest is
         *  outside;
         *
         *  (C): the region is completely outside of the file.
         *
         * This classification is stable under DLM lock already acquired by
         * the caller, because to change the class, other client has to take
         * DLM lock conflicting with our lock. Also, any updates to ->i_size
         * by other threads on this client are serialized by
         * ll_inode_size_lock(). This guarantees that short reads are handled
         * correctly in the face of concurrent writes and truncates.
         */
	vvp_object_size_lock(obj);
        result = cl_object_attr_get(env, obj, attr);
        if (result == 0) {
                kms = attr->cat_kms;
                if (pos > kms) {
                        /*
                         * A glimpse is necessary to determine whether we
                         * return a short read (B) or some zeroes at the end
                         * of the buffer (C)
                         */
			vvp_object_size_unlock(obj);
                        result = cl_glimpse_lock(env, io, inode, obj, 0);
                        if (result == 0 && exceed != NULL) {
                                /* If objective page index exceed end-of-file
                                 * page index, return directly. Do not expect
                                 * kernel will check such case correctly.
                                 * linux-2.6.18-128.1.1 miss to do that.
                                 * --bug 17336 */
				loff_t size = i_size_read(inode);
				unsigned long cur_index = start >>
							  PAGE_CACHE_SHIFT;

				if ((size == 0 && cur_index != 0) ||
				    (((size - 1) >> PAGE_CACHE_SHIFT) <
				     cur_index))
				*exceed = 1;
                        }
                        return result;
                } else {
                        /*
                         * region is within kms and, hence, within real file
                         * size (A). We need to increase i_size to cover the
                         * read region so that generic_file_read() will do its
                         * job, but that doesn't mean the kms size is
                         * _correct_, it is only the _minimum_ size. If
                         * someone does a stat they will get the correct size
                         * which will always be >= the kms value here.
                         * b=11081
                         */
			if (i_size_read(inode) < kms) {
				i_size_write(inode, kms);
                                CDEBUG(D_VFSTRACE,
                                       DFID" updating i_size "LPU64"\n",
                                       PFID(lu_object_fid(&obj->co_lu)),
				       (__u64)i_size_read(inode));

                        }
                }
        }

	vvp_object_size_unlock(obj);

	return result;
}

/*****************************************************************************
 *
 * Transfer operations.
 *
 */

void ccc_req_completion(const struct lu_env *env,
                        const struct cl_req_slice *slice, int ioret)
{
        struct ccc_req *vrq;

        if (ioret > 0)
                cl_stats_tally(slice->crs_dev, slice->crs_req->crq_type, ioret);

        vrq = cl2ccc_req(slice);
        OBD_SLAB_FREE_PTR(vrq, ccc_req_kmem);
}

/**
 * Implementation of struct cl_req_operations::cro_attr_set() for ccc
 * layer. ccc is responsible for
 *
 *    - o_[mac]time
 *
 *    - o_mode
 *
 *    - o_parent_seq
 *
 *    - o_[ug]id
 *
 *    - o_parent_oid
 *
 *    - o_parent_ver
 *
 *    - o_ioepoch,
 *
 *  and capability.
 */
void ccc_req_attr_set(const struct lu_env *env,
		      const struct cl_req_slice *slice,
		      const struct cl_object *obj,
		      struct cl_req_attr *attr, u64 flags)
{
	struct inode	*inode;
	struct obdo	*oa;
	u32		 valid_flags;

	oa = attr->cra_oa;
	inode = vvp_object_inode(obj);
	valid_flags = OBD_MD_FLTYPE;

	if ((flags & OBD_MD_FLOSSCAPA) != 0) {
		LASSERT(attr->cra_capa == NULL);
		attr->cra_capa = cl_capa_lookup(inode,
						slice->crs_req->crq_type);
	}

	if (slice->crs_req->crq_type == CRT_WRITE) {
		if (flags & OBD_MD_FLEPOCH) {
			oa->o_valid |= OBD_MD_FLEPOCH;
			oa->o_ioepoch = ll_i2info(inode)->lli_ioepoch;
			valid_flags |= OBD_MD_FLMTIME | OBD_MD_FLCTIME |
				       OBD_MD_FLUID | OBD_MD_FLGID;
		}
	}
	obdo_from_inode(oa, inode, valid_flags & flags);
	obdo_set_parent_fid(oa, &ll_i2info(inode)->lli_fid);
	if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_INVALID_PFID))
		oa->o_parent_oid++;
	memcpy(attr->cra_jobid, ll_i2info(inode)->lli_jobid,
	       LUSTRE_JOBID_SIZE);
}

static const struct cl_req_operations ccc_req_ops = {
        .cro_attr_set   = ccc_req_attr_set,
        .cro_completion = ccc_req_completion
};

int cl_setattr_ost(struct inode *inode, const struct iattr *attr,
                   struct obd_capa *capa)
{
        struct lu_env *env;
        struct cl_io  *io;
        int            result;
        int            refcheck;

        ENTRY;

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        io = ccc_env_thread_io(env);
	io->ci_obj = ll_i2info(inode)->lli_clob;

	io->u.ci_setattr.sa_attr.lvb_atime = LTIME_S(attr->ia_atime);
	io->u.ci_setattr.sa_attr.lvb_mtime = LTIME_S(attr->ia_mtime);
	io->u.ci_setattr.sa_attr.lvb_ctime = LTIME_S(attr->ia_ctime);
	io->u.ci_setattr.sa_attr.lvb_size = attr->ia_size;
	io->u.ci_setattr.sa_valid = attr->ia_valid;
	io->u.ci_setattr.sa_parent_fid = ll_inode2fid(inode);
	io->u.ci_setattr.sa_capa = capa;

again:
        if (cl_io_init(env, io, CIT_SETATTR, io->ci_obj) == 0) {
		struct vvp_io *vio = vvp_env_io(env);

		if (attr->ia_valid & ATTR_FILE)
			/* populate the file descriptor for ftruncate to honor
			 * group lock - see LU-787 */
			vio->vui_fd = LUSTRE_FPRIVATE(attr->ia_file);

                result = cl_io_loop(env, io);
        } else {
                result = io->ci_result;
        }
        cl_io_fini(env, io);
	if (unlikely(io->ci_need_restart))
		goto again;
	/* HSM import case: file is released, cannot be restored
	 * no need to fail except if restore registration failed
	 * with -ENODATA */
	if (result == -ENODATA && io->ci_restore_needed &&
	    io->ci_result != -ENODATA)
		result = 0;
	cl_env_put(env, &refcheck);
	RETURN(result);
}

/*****************************************************************************
 *
 * Type conversions.
 *
 */

struct vvp_io *cl2vvp_io(const struct lu_env *env,
			 const struct cl_io_slice *slice)
{
	struct vvp_io *vio;

	vio = container_of(slice, struct vvp_io, vui_cl);
	LASSERT(vio == vvp_env_io(env));

	return vio;
}

struct ccc_req *cl2ccc_req(const struct cl_req_slice *slice)
{
        return container_of0(slice, struct ccc_req, crq_cl);
}

/**
 * Initialize or update CLIO structures for regular files when new
 * meta-data arrives from the server.
 *
 * \param inode regular file inode
 * \param md    new file metadata from MDS
 * - allocates cl_object if necessary,
 * - updated layout, if object was already here.
 */
int cl_file_inode_init(struct inode *inode, struct lustre_md *md)
{
	struct lu_env        *env;
	struct ll_inode_info *lli;
        struct cl_object     *clob;
        struct lu_site       *site;
        struct lu_fid        *fid;
        struct cl_object_conf conf = {
                .coc_inode = inode,
                .u = {
                        .coc_md    = md
                }
        };
        int result = 0;
        int refcheck;

	LASSERT(md->body->mbo_valid & OBD_MD_FLID);
	LASSERT(S_ISREG(inode->i_mode));

	env = cl_env_get(&refcheck);
	if (IS_ERR(env))
		return PTR_ERR(env);

	site = ll_i2sbi(inode)->ll_site;
	lli  = ll_i2info(inode);
        fid  = &lli->lli_fid;
        LASSERT(fid_is_sane(fid));

        if (lli->lli_clob == NULL) {
                /* clob is slave of inode, empty lli_clob means for new inode,
                 * there is no clob in cache with the given fid, so it is
                 * unnecessary to perform lookup-alloc-lookup-insert, just
                 * alloc and insert directly. */
                LASSERT(inode->i_state & I_NEW);
                conf.coc_lu.loc_flags = LOC_F_NEW;
                clob = cl_object_find(env, lu2cl_dev(site->ls_top_dev),
                                      fid, &conf);
                if (!IS_ERR(clob)) {
                        /*
                         * No locking is necessary, as new inode is
                         * locked by I_NEW bit.
                         */
                        lli->lli_clob = clob;
			lli->lli_has_smd = lsm_has_objects(md->lsm);
                        lu_object_ref_add(&clob->co_lu, "inode", inode);
                } else
                        result = PTR_ERR(clob);
	} else {
		result = cl_conf_set(env, lli->lli_clob, &conf);
	}

        cl_env_put(env, &refcheck);

        if (result != 0)
                CERROR("Failure to initialize cl object "DFID": %d\n",
                       PFID(fid), result);
        return result;
}

/**
 * Wait for others drop their references of the object at first, then we drop
 * the last one, which will lead to the object be destroyed immediately.
 * Must be called after cl_object_kill() against this object.
 *
 * The reason we want to do this is: destroying top object will wait for sub
 * objects being destroyed first, so we can't let bottom layer (e.g. from ASTs)
 * to initiate top object destroying which may deadlock. See bz22520.
 */
static void cl_object_put_last(struct lu_env *env, struct cl_object *obj)
{
	struct lu_object_header *header = obj->co_lu.lo_header;
	wait_queue_t           waiter;

	if (unlikely(atomic_read(&header->loh_ref) != 1)) {
		struct lu_site *site = obj->co_lu.lo_dev->ld_site;
		struct lu_site_bkt_data *bkt;

		bkt = lu_site_bkt_from_fid(site, &header->loh_fid);

		init_waitqueue_entry_current(&waiter);
		add_wait_queue(&bkt->lsb_marche_funebre, &waiter);

		while (1) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			if (atomic_read(&header->loh_ref) == 1)
				break;
			waitq_wait(&waiter, TASK_UNINTERRUPTIBLE);
		}

		set_current_state(TASK_RUNNING);
		remove_wait_queue(&bkt->lsb_marche_funebre, &waiter);
	}

	cl_object_put(env, obj);
}

void cl_inode_fini(struct inode *inode)
{
	struct lu_env           *env;
	struct ll_inode_info    *lli  = ll_i2info(inode);
        struct cl_object        *clob = lli->lli_clob;
        int refcheck;
        int emergency;

        if (clob != NULL) {
                void                    *cookie;

                cookie = cl_env_reenter();
                env = cl_env_get(&refcheck);
                emergency = IS_ERR(env);
                if (emergency) {
			mutex_lock(&ccc_inode_fini_guard);
                        LASSERT(ccc_inode_fini_env != NULL);
                        cl_env_implant(ccc_inode_fini_env, &refcheck);
                        env = ccc_inode_fini_env;
                }
                /*
                 * cl_object cache is a slave to inode cache (which, in turn
                 * is a slave to dentry cache), don't keep cl_object in memory
                 * when its master is evicted.
                 */
                cl_object_kill(env, clob);
                lu_object_ref_del(&clob->co_lu, "inode", inode);
                cl_object_put_last(env, clob);
                lli->lli_clob = NULL;
                if (emergency) {
                        cl_env_unplant(ccc_inode_fini_env, &refcheck);
			mutex_unlock(&ccc_inode_fini_guard);
                } else
                        cl_env_put(env, &refcheck);
                cl_env_reexit(cookie);
        }
}

/**
 * return IF_* type for given lu_dirent entry.
 * IF_* flag shld be converted to particular OS file type in
 * platform llite module.
 */
__u16 ll_dirent_type_get(struct lu_dirent *ent)
{
        __u16 type = 0;
        struct luda_type *lt;
        int len = 0;

        if (le32_to_cpu(ent->lde_attrs) & LUDA_TYPE) {
                const unsigned align = sizeof(struct luda_type) - 1;

                len = le16_to_cpu(ent->lde_namelen);
                len = (len + align) & ~align;
		lt = (void *)ent->lde_name + len;
		type = IFTODT(le16_to_cpu(lt->lt_type));
	}
	return type;
}

/**
 * build inode number from passed @fid */
__u64 cl_fid_build_ino(const struct lu_fid *fid, int api32)
{
        if (BITS_PER_LONG == 32 || api32)
                RETURN(fid_flatten32(fid));
        else
                RETURN(fid_flatten(fid));
}

/**
 * build inode generation from passed @fid.  If our FID overflows the 32-bit
 * inode number then return a non-zero generation to distinguish them. */
__u32 cl_fid_build_gen(const struct lu_fid *fid)
{
        __u32 gen;
        ENTRY;

        if (fid_is_igif(fid)) {
                gen = lu_igif_gen(fid);
                RETURN(gen);
        }

        gen = (fid_flatten(fid) >> 32);
        RETURN(gen);
}

/* lsm is unreliable after hsm implementation as layout can be changed at
 * any time. This is only to support old, non-clio-ized interfaces. It will
 * cause deadlock if clio operations are called with this extra layout refcount
 * because in case the layout changed during the IO, ll_layout_refresh() will
 * have to wait for the refcount to become zero to destroy the older layout.
 *
 * Notice that the lsm returned by this function may not be valid unless called
 * inside layout lock - MDS_INODELOCK_LAYOUT. */
struct lov_stripe_md *ccc_inode_lsm_get(struct inode *inode)
{
	return lov_lsm_get(ll_i2info(inode)->lli_clob);
}

void inline ccc_inode_lsm_put(struct inode *inode, struct lov_stripe_md *lsm)
{
	lov_lsm_put(ll_i2info(inode)->lli_clob, lsm);
}
