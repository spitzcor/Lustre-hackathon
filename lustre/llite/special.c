/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Special file handling for Lustre.
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Wang Di <wangdi@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
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
 */

#define DEBUG_SUBSYSTEM S_LLITE
#include <linux/lustre_dlm.h>
#include <linux/lustre_lite.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
#include <linux/lustre_compat25.h>
#endif
#include <asm/poll.h>
#include "llite_internal.h"

#define INODE_OPS 1
#define FILE_OPS 2

static struct file_operations **get_save_fops(struct file* filp, int mode)
{
        struct inode *inode = filp->f_dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);

        if (mode == INODE_OPS) {
                return &(lli->ll_save_ifop);
        } else if (mode == FILE_OPS) {
                if (S_ISFIFO(inode->i_mode)) {
                        switch (filp->f_mode) {
                        case 1: /*O_RDONLY*/
                                return &(lli->ll_save_ffop);
                        case 2: /*O_WRONLY*/
                                return &(lli->ll_save_wfop);
                        case 3: /* O_RDWR */
                                return &(lli->ll_save_wrfop);
                        default:
                                return NULL;
                        }
                }
                return &(lli->ll_save_ffop);
        } else {
                CERROR("invalid special file ops %d\n", mode);
                LBUG();
                return NULL;
        }
}

static void save_fops(struct file *filp, struct inode *inode,
                      struct file_operations *sfops)
{
        if (sfops != filp->f_op) {
                struct file_operations **pfop = get_save_fops(filp, FILE_OPS);

                *pfop = filp->f_op;
                if (S_ISCHR(inode->i_mode))
                        filp->f_op = &ll_special_chr_file_fops;
                else if (S_ISFIFO(inode->i_mode))
                        filp->f_op = &ll_special_fifo_file_fops;

                CDEBUG(D_INFO,"saved %p, replaced with %p\n", *pfop,filp->f_op);
                if ((*pfop)->owner)
                        CDEBUG(D_INFO,"%p has owner %p\n",*pfop,(*pfop)->owner);
        }
}

static ssize_t ll_special_file_read(struct file *filp, char *buf,
                                    size_t count, loff_t *ppos)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->read)
                rc = (*pfop)->read(filp, buf, count, ppos);

        RETURN(rc);
}

static ssize_t ll_special_file_write(struct file *filp, const char *buf,
                                     size_t count, loff_t *ppos)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->write)
                rc = (*pfop)->write(filp, buf, count, ppos);

        RETURN(rc);
}

static int ll_special_file_ioctl(struct inode *inode, struct file *filp,
                                 unsigned int cmd, unsigned long arg)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = -ENOTTY;

        if (pfop && *pfop && (*pfop)->ioctl) {
                struct file_operations *sfops = filp->f_op;

                rc = (*pfop)->ioctl(inode, filp, cmd, arg);
                save_fops(filp, inode, sfops);
        }
        RETURN(rc);
}

static loff_t ll_special_file_seek(struct file *filp, loff_t offset, int origin)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = 0;

        if (pfop && *pfop && (*pfop)->llseek)
                rc = (*pfop)->llseek(filp, offset, origin);
        else
                rc = default_llseek(filp, offset, origin);

        RETURN(rc);
}


#define DEFAULT_POLLMASK (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM)

static unsigned int ll_special_file_poll(struct file *filp,
                                         struct poll_table_struct *poll_table)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = DEFAULT_POLLMASK;

        if (pfop && *pfop && (*pfop)->poll)
                rc = (*pfop)->poll(filp, poll_table);

        RETURN(rc);
}

static int ll_special_file_open(struct inode *inode, struct file *filp)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop) {
                fops_get(*pfop);

                if ((*pfop)->open)
                        rc = (*pfop)->open(inode, filp);

                if (rc)
                        fops_put(*pfop);
        }

        RETURN(rc);
}

static ssize_t ll_special_read(struct file *filp, char *buf, size_t count,
                               loff_t *ppos)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->read)
                rc = (*pfop)->read(filp, buf, count, ppos);

        RETURN(rc);
}

static ssize_t ll_special_write(struct file *filp, const char *buf,
                                size_t count, loff_t *ppos)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->write)
                rc = (*pfop)->write(filp, buf, count, ppos);

        RETURN(rc);
}

static int ll_special_ioctl(struct inode *inode, struct file *filp,
                            unsigned int cmd, unsigned long arg)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        int rc = -ENOTTY;

        if (pfop && *pfop && (*pfop)->ioctl) {
                struct file_operations *sfops = filp->f_op;
			
                rc = (*pfop)->ioctl(inode, filp, cmd, arg);

                /* sometimes, file_operations will be changed in ioctl */
                save_fops(filp, inode, sfops);
        }

        RETURN(rc);
}

static int ll_special_mmap(struct file * filp, struct vm_area_struct * vma)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        int rc = -ENODEV;

        if (pfop && *pfop && (*pfop)->mmap)
                rc = (*pfop)->mmap(filp, vma);

        RETURN(rc);
}

static loff_t ll_special_seek(struct file *filp, loff_t offset, int origin)
{
        struct file_operations** pfop = get_save_fops (filp, INODE_OPS);
        int    rc = 0;

        if (pfop && *pfop && (*pfop)->llseek)
                rc = (*pfop)->llseek(filp, offset, origin);
        else
                rc = default_llseek(filp, offset, origin);

        RETURN(rc);
}

static int ll_special_fsync(struct file *filp, struct dentry *dentry, int data)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->fsync)
                rc = (*pfop)->fsync(filp, dentry, data);

        RETURN(rc);
}

static int ll_special_file_fasync(int fd, struct file *filp, int on)
{
        struct file_operations **pfop = get_save_fops(filp, FILE_OPS);
        int rc = -EINVAL;

        if (pfop && *pfop && (*pfop)->fasync)
                rc = (*pfop)->fasync(fd, filp, on);

        RETURN(rc);
}

static int ll_special_release_internal(struct inode *inode, struct file *filp,
                                       int mode)
{
       struct file_operations **pfop = get_save_fops(filp, mode);
       struct ll_sb_info *sbi = ll_i2sbi(inode);
       int rc = 0, err;
       ENTRY;

        if (pfop && *pfop) {
                if ((*pfop)->release)
                        rc = (*pfop)->release(inode, filp);
                fops_put(*pfop);
        }

        lprocfs_counter_incr(sbi->ll_stats, LPROC_LL_RELEASE);
        err = ll_md_close(sbi->ll_md_exp, inode, filp);

        if (err && rc == 0)
                rc = err;

        RETURN(rc);
}

static int ll_special_open(struct inode *inode, struct file *filp)
{
        struct file_operations **pfop = get_save_fops(filp, INODE_OPS);
        struct file_operations *sfops = filp->f_op;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ptlrpc_request *req;
        struct lookup_intent *it;
        int rc = -EINVAL, err;
        struct obd_client_handle **och_p;
        __u64 *och_usecount;
        ENTRY;

        it = filp->f_it;

        if (LUSTRE_IT(it)->it_disposition) {
                err = it_open_error(DISP_OPEN_OPEN, it);
                if (err)
                        RETURN(err);
        }

        if (pfop && *pfop) {
                /* mostly we will have @def_blk_fops here and it is not in a
                 * module but we do this just to be sure. */
                fops_get(*pfop);

                if ((*pfop)->open) {
                        rc = (*pfop)->open(inode, filp);

                        if (rc)
                                fops_put(*pfop);
                        else    /* sometimes ops will be changed in open */
                                save_fops(filp, inode, sfops);
                }
        }

        /* Let's see if we have file open on MDS already. */
        if (it->it_flags & FMODE_WRITE) {
                och_p = &lli->lli_mds_write_och;
                och_usecount = &lli->lli_open_fd_write_count;
        } else if (it->it_flags & FMODE_EXEC) {
                och_p = &lli->lli_mds_exec_och;
                och_usecount = &lli->lli_open_fd_exec_count;
         } else {
                och_p = &lli->lli_mds_read_och;
                och_usecount = &lli->lli_open_fd_read_count;
        }

        lprocfs_counter_incr(ll_i2sbi(inode)->ll_stats, LPROC_LL_OPEN);

        down(&lli->lli_och_sem);
        if (*och_p) { /* Open handle is present */
                if (LUSTRE_IT(it)->it_disposition) {
                        struct obd_client_handle *och;
                        /* Well, there's extra open request that we do not need,
                           let's close it somehow*/
                        OBD_ALLOC(och, sizeof (struct obd_client_handle));
                        if (!och) {
                                /* XXX We leak open fd and open OPEN connectioni
                                   to server here */
                                up(&lli->lli_och_sem);
                                RETURN(-ENOMEM);
                        }
                        ll_och_fill(inode, it, och);
                        /* ll_md_och_close() will free och */
                        ll_md_och_close(ll_i2mdexp(inode), inode, och);
                }       
                (*och_usecount)++;        

                err = ll_local_open(filp, it, NULL);
        } else {
                LASSERT(*och_usecount == 0);
                OBD_ALLOC(*och_p, sizeof (struct obd_client_handle));
                if (!*och_p) {
                        // XXX Same as above
                        up(&lli->lli_och_sem);
                        RETURN(-ENOMEM);
                }
                (*och_usecount)++;

                err = ll_local_open(filp, it, *och_p);
        }
        up(&lli->lli_och_sem);

        if (rc != 0) {
                CERROR("error opening special file: rc %d\n", rc);
                ll_md_close(ll_i2sbi(inode)->ll_md_exp, inode, filp);
        } else if (err) {
                if (pfop && *pfop) {
                        if ((*pfop)->release)
                                (*pfop)->release(inode, filp);
                        fops_put(*pfop);
                }
                rc = err;
        }

        req = LUSTRE_IT(it)->it_data;
        if (req)
                ptlrpc_req_finished(req);

        RETURN(rc);
}

static int ll_special_release(struct inode *inode, struct file *filp)
{
        return ll_special_release_internal(inode, filp, INODE_OPS);
}

static int ll_special_file_release(struct inode *inode, struct file *filp)
{
        return ll_special_release_internal(inode, filp, FILE_OPS);
}

struct inode_operations ll_special_inode_operations = {
        .setattr        = ll_setattr,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        .getattr        = ll_getattr,
#else
        .revalidate_it  = ll_inode_revalidate_it,
#endif
        .setxattr       = ll_setxattr,
        .getxattr       = ll_getxattr,
        .listxattr      = ll_listxattr,
        .removexattr    = ll_removexattr,
        .permission     = ll_inode_permission,
        
};

struct file_operations ll_special_chr_inode_fops = {
        //FIXME .owner          = THIS_MODULE,
        .open           = ll_special_open,
};

struct file_operations ll_special_blk_inode_fops = {
        //FIXME .owner          = THIS_MODULE,
        .read           = ll_special_read,
        .write          = ll_special_write,
        .ioctl          = ll_special_ioctl,
        .open           = ll_special_open,
        .release        = ll_special_release,
        .mmap           = ll_special_mmap,
        .llseek         = ll_special_seek,
        .fsync          = ll_special_fsync,
};

struct file_operations ll_special_fifo_inode_fops = {
        //FIXME .owner          = THIS_MODULE,
        .open           = ll_special_open,
};

struct file_operations ll_special_sock_inode_fops = {
        //FIXME .owner          = THIS_MODULE,
        .open           = ll_special_open
};

struct file_operations ll_special_chr_file_fops = {
        //FIXME .owner          = THIS_MODULE,
        .llseek         = ll_special_file_seek,
        .read           = ll_special_file_read,
        .write          = ll_special_file_write,
        .poll           = ll_special_file_poll,
        .ioctl          = ll_special_file_ioctl,
        .open           = ll_special_file_open,
        .release        = ll_special_file_release,
        .fasync         = ll_special_file_fasync,
};

struct file_operations ll_special_fifo_file_fops = {
        //FIXME .owner          = THIS_MODULE,
        .llseek         = ll_special_file_seek,
        .read           = ll_special_file_read,
        .write          = ll_special_file_write,
        .poll           = ll_special_file_poll,
        .ioctl          = ll_special_file_ioctl,
        .open           = ll_special_file_open,
        .release        = ll_special_file_release,
};

