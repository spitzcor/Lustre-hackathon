/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Robert Read <rread@clusterfs.com>
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


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <mntent.h>
#define _GNU_SOURCE
#include <getopt.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <grp.h>

#include "obdctl.h"
#include <portals/ptlctl.h>

int debug;
int verbose;
int nomtab;
int force;
static char *progname = NULL;

typedef struct {
        ptl_nid_t gw;
        ptl_nid_t lo;
        ptl_nid_t hi;
} llmount_route_t;

#define MAX_ROUTES  1024
int route_index;
ptl_nid_t lmd_cluster_id = 0;
llmount_route_t routes[MAX_ROUTES];

static int check_mtab_entry(char *spec, char *mtpt, char *type)
{
        FILE *fp;
        struct mntent *mnt;

        if (!force) {
                fp = setmntent(MOUNTED, "r");
                if (fp == NULL)
                        return(0);

                while ((mnt = getmntent(fp)) != NULL) {
                        if (strcmp(mnt->mnt_fsname, spec) == 0 &&
                            strcmp(mnt->mnt_dir, mtpt) == 0 &&
                            strcmp(mnt->mnt_type, type) == 0) {
                                fprintf(stderr, "%s: according to %s %s is "
                                        "already mounted on %s\n",
                                        progname, MOUNTED, spec, mtpt);
                                return(1); /* or should we return an error? */
                        }
                }
                endmntent(fp);
        }
        return(0);
}

static void
update_mtab_entry(char *spec, char *mtpt, char *type, char *opts,
                  int flags, int freq, int pass)
{
        FILE *fp;
        struct mntent mnt;

        mnt.mnt_fsname = spec;
        mnt.mnt_dir = mtpt;
        mnt.mnt_type = type;
        mnt.mnt_opts = opts ? opts : "";
        mnt.mnt_freq = freq;
        mnt.mnt_passno = pass;

        if (!nomtab) {
                fp = setmntent(MOUNTED, "a+");
                if (fp == NULL) {
                        fprintf(stderr, "%s: setmntent(%s): %s:",
                                progname, MOUNTED, strerror (errno));
                } else {
                        if ((addmntent (fp, &mnt)) == 1) {
                                fprintf(stderr, "%s: addmntent: %s:",
                                        progname, strerror (errno));
                        }
                        endmntent(fp);
                }
        }
}

int
init_options(struct lustre_mount_data *lmd)
{
        memset(lmd, 0, sizeof(*lmd));
        lmd->lmd_magic = LMD_MAGIC;
        lmd->lmd_server_nid = PTL_NID_ANY;
        lmd->lmd_local_nid = PTL_NID_ANY;
        lmd->lmd_port = 988;    /* XXX define LUSTRE_DEFAULT_PORT */
        lmd->lmd_nal = SOCKNAL;
        lmd->lmd_async = 0;
        lmd->lmd_nllu = 99;
        lmd->lmd_nllg = 99;
        strncpy(lmd->lmd_security, "null", sizeof(lmd->lmd_security));
        return 0;
}

int
print_options(struct lustre_mount_data *lmd)
{
        int i;

        printf("mds:             %s\n", lmd->lmd_mds);
        printf("profile:         %s\n", lmd->lmd_profile);
        printf("sec_flavor:      %s\n", lmd->lmd_security);
        printf("server_nid:      "LPX64"\n", lmd->lmd_server_nid);
#ifdef CRAY_PORTALS
        if (lmd->lmd_nal != CRAY_KB_SSNAL) {
#endif
                printf("local_nid:       "LPX64"\n", lmd->lmd_local_nid);
#ifdef CRAY_PORTALS
        }
#endif
        printf("nal:             %x\n", lmd->lmd_nal);
#ifdef CRAY_PORTALS
        if (lmd->lmd_nal != CRAY_KB_SSNAL) {
#endif
                printf("server_ipaddr:   0x%x\n", lmd->lmd_server_ipaddr);
                printf("port:            %d\n", lmd->lmd_port);
#ifdef CRAY_PORTALS
        }
#endif

        for (i = 0; i < route_index; i++)
                printf("route:           "LPX64" : "LPX64" - "LPX64"\n",
                       routes[i].gw, routes[i].lo, routes[i].hi);

        return 0;
}

static int parse_route(char *opteq, char *opttgts)
{
        char *gw_lo_ptr, *gw_hi_ptr, *tgt_lo_ptr, *tgt_hi_ptr;
        ptl_nid_t gw_lo, gw_hi, tgt_lo, tgt_hi;

        opttgts[0] = '\0';
        gw_lo_ptr = opteq + 1;
        if (!(gw_hi_ptr = strchr(gw_lo_ptr, '-'))) {
                gw_hi_ptr = gw_lo_ptr;
        } else {
                gw_hi_ptr[0] = '\0';
                gw_hi_ptr++;
        }

        if (ptl_parse_nid(&gw_lo, gw_lo_ptr) != 0) {
                fprintf(stderr, "%s: can't parse NID %s\n", progname,gw_lo_ptr);
                return(-1);
        }

        if (ptl_parse_nid(&gw_hi, gw_hi_ptr) != 0) {
                fprintf(stderr, "%s: can't parse NID %s\n", progname,gw_hi_ptr);
                return(-1);
        }

        tgt_lo_ptr = opttgts + 1;
        if (!(tgt_hi_ptr = strchr(tgt_lo_ptr, '-'))) {
                tgt_hi_ptr = tgt_lo_ptr;
        } else {
                tgt_hi_ptr[0] = '\0';
                tgt_hi_ptr++;
        }

        if (ptl_parse_nid(&tgt_lo, tgt_lo_ptr) != 0) {
                fprintf(stderr, "%s: can't parse NID %s\n",progname,tgt_lo_ptr);
                return(-1);
        }

        if (ptl_parse_nid(&tgt_hi, tgt_hi_ptr) != 0) {
                fprintf(stderr, "%s: can't parse NID %s\n",progname,tgt_hi_ptr);
                return(-1);
        }

        while (gw_lo <= gw_hi) {
                if (route_index >= MAX_ROUTES) {
                        fprintf(stderr, "%s: to many routes %d\n",
                                progname, MAX_ROUTES);
                        return(-1);
                }

                routes[route_index].gw = gw_lo;
                routes[route_index].lo = tgt_lo;
                routes[route_index].hi = tgt_hi;
                route_index++;
                gw_lo++;
        }

        return(0);
}

/*
 * here all what we do is gurantee the result is exactly
 * what user intend to get, no ambiguous. maybe there have
 * simpler library call could do the same job for us?
 */
static int parse_u32(char *str, uint32_t *res)
{
        unsigned long id;
        char *endptr = NULL;

        id = strtol(str, &endptr, 0);
        if (endptr && *endptr != 0)
                return -1;

        if (id == LONG_MAX || id == LONG_MIN)
                return -1;

        if ((uint32_t)id != id)
                return -1;

        *res = (uint32_t) id;
        return 0;
}

static int parse_nllu(struct lustre_mount_data *lmd, char *str_nllu)
{
        struct passwd *pass;

        if (parse_u32(str_nllu, &lmd->lmd_nllu) == 0)
                return 0;

        pass = getpwnam(str_nllu);
        if (pass == NULL)
                return -1;

        lmd->lmd_nllu = pass->pw_uid;
        return 0;
}

static int parse_nllg(struct lustre_mount_data *lmd, char *str_nllg)
{
        struct group *grp;

        if (parse_u32(str_nllg, &lmd->lmd_nllg) == 0)
                return 0;

        grp = getgrnam(str_nllg);
        if (grp == NULL)
                return -1;

        lmd->lmd_nllg = grp->gr_gid;
        return 0;
}

int parse_options(char * options, struct lustre_mount_data *lmd)
{
        ptl_nid_t nid = 0, cluster_id = 0;
        int val;
        char *opt, *opteq, *opttgts;

        /* parsing ideas here taken from util-linux/mount/nfsmount.c */
        for (opt = strtok(options, ","); opt; opt = strtok(NULL, ",")) {
                if ((opteq = strchr(opt, '='))) {
                        val = atoi(opteq + 1);
                        *opteq = '\0';
                        if (!strcmp(opt, "nettype")) {
                                lmd->lmd_nal = ptl_name2nal(opteq + 1);
                        } else if(!strcmp(opt, "cluster_id")) {
                                if (ptl_parse_nid(&cluster_id, opteq+1) != 0) {
                                        fprintf (stderr, "%s: can't parse NID "
                                                 "%s\n", progname, opteq+1);
                                        return (-1);
                                }
                                lmd_cluster_id = cluster_id;
                        } else if(!strcmp(opt, "route")) {
                                if (!(opttgts = strchr(opteq + 1, ':'))) {
                                        fprintf(stderr, "%s: Route must be "
                                                "of the form: route="
                                                "<gw>[-<gw>]:<low>[-<high>]\n",
                                                progname);
                                        return(-1);
                                }
                                parse_route(opteq, opttgts);
                        } else if (!strcmp(opt, "local_nid")) {
                                if (ptl_parse_nid(&nid, opteq + 1) != 0) {
                                        fprintf (stderr, "%s: "
                                                 "can't parse NID %s\n",
                                                 progname,
                                                 opteq+1);
                                        return (-1);
                                }
                                lmd->lmd_local_nid = nid;
                        } else if (!strcmp(opt, "server_nid")) {
                                if (ptl_parse_nid(&nid, opteq + 1) != 0) {
                                        fprintf (stderr, "%s: "
                                                 "can't parse NID %s\n",
                                                 progname, opteq + 1);
                                        return (-1);
                                }
                                lmd->lmd_server_nid = nid;
                        } else if (!strcmp(opt, "port")) {
                                lmd->lmd_port = val;
                        } else if (!strcmp(opt, "sec")) {
                                strncpy(lmd->lmd_security, opteq + 1,
                                        sizeof(lmd->lmd_security));
                        } else if (!strcmp(opt, "nllu")) {
                                if (parse_nllu(lmd, opteq + 1)) {
                                        fprintf(stderr, "%s: "
                                                "can't parse user: %s\n",
                                                progname, opteq + 1);
                                        return (-1);
                                }
                        } else if (!strcmp(opt, "nllg")) {
                                if (parse_nllg(lmd, opteq + 1)) {
                                        fprintf(stderr, "%s: "
                                                "can't parse group: %s\n",
                                                progname, opteq + 1);
                                        return (-1);
                                }
                        }
                } else {
                        val = 1;
                        if (!strncmp(opt, "no", 2)) {
                                val = 0;
                                opt += 2;
                        }
                        if (!strcmp(opt, "debug")) {
                                debug = val;
                        } else if (!strcmp(opt, "async")) {
                                lmd->lmd_async = 1;
                        }
                }
        }
        return 0;
}

int
get_local_elan_id(char *fname, char *buf)
{
        FILE *fp = fopen(fname, "r");
        int   rc;

        if (fp == NULL)
                return -1;

        rc = fscanf(fp, "NodeId %255s", buf);

        fclose(fp);

        return (rc == 1) ? 0 : -1;
}

int
set_local(struct lustre_mount_data *lmd)
{
        /* XXX ClusterID?
         * XXX PtlGetId() will be safer if portals is loaded and
         * initialised correctly at this time... */
        char buf[256];
        ptl_nid_t nid;
        int rc;

        if (lmd->lmd_local_nid != PTL_NID_ANY)
                return 0;

        memset(buf, 0, sizeof(buf));

#ifdef CRAY_PORTALS
        if (lmd->lmd_nal == CRAY_KB_ERNAL) {
#else
        if (lmd->lmd_nal == SOCKNAL || lmd->lmd_nal == TCPNAL ||
            lmd->lmd_nal == OPENIBNAL || lmd->lmd_nal == IIBNAL) {
#endif
                struct utsname uts;

                rc = gethostname(buf, sizeof(buf) - 1);
                if (rc) {
                        fprintf(stderr, "%s: can't get hostname: %s\n",
                                progname, strerror(rc));
                        return rc;
                }

                rc = uname(&uts);
                /* for 2.6 kernels, reserve at least 8MB free, or we will
                 * go OOM during heavy read load */
                if (rc == 0 && strncmp(uts.release, "2.6", 3) == 0) {
                        int f, minfree = 32768;
                        char name[40], val[40];
                        FILE *meminfo;

                        meminfo = fopen("/proc/meminfo", "r");
                        if (meminfo != NULL) {
                                while (fscanf(meminfo, "%s %s %*s\n", name, val) != EOF) {
                                        if (strcmp(name, "MemTotal:") == 0) {
                                                f = strtol(val, NULL, 0);
                                                if (f > 0 && f < 8 * minfree)
                                                        minfree = f / 16;
                                                break;
                                        }
                                }
                                fclose(meminfo);
                        }
                        f = open("/proc/sys/vm/min_free_kbytes", O_WRONLY);
                        if (f >= 0) {
                                sprintf(val, "%d", minfree);
                                write(f, val, strlen(val));
                                close(f);
                        }
                }
#ifndef CRAY_PORTALS
        } else if (lmd->lmd_nal == QSWNAL) {
                char *pfiles[] = {"/proc/qsnet/elan3/device0/position",
                                  "/proc/qsnet/elan4/device0/position",
                                  "/proc/elan/device0/position",
                                  NULL};
                int   i = 0;

                do {
                        rc = get_local_elan_id(pfiles[i], buf);
                } while (rc != 0 && pfiles[++i] != NULL);

                if (rc != 0) {
                        fprintf(stderr, "%s: can't read Elan ID from /proc\n",
                                progname);

                        return -1;
                }
#else
	} else if (lmd->lmd_nal == CRAY_KB_SSNAL) {
		return 0;
#endif
        }

        if (ptl_parse_nid (&nid, buf) != 0) {
                fprintf (stderr, "%s: can't parse NID %s\n", progname, buf);
                return (-1);
        }

        lmd->lmd_local_nid = nid + lmd_cluster_id;
        return 0;
}

int
set_peer(char *hostname, struct lustre_mount_data *lmd)
{
        ptl_nid_t nid = 0;
        int rc;

#ifdef CRAY_PORTALS
        if (lmd->lmd_nal == CRAY_KB_ERNAL) {
#else
        if (lmd->lmd_nal == SOCKNAL || lmd->lmd_nal == TCPNAL ||
            lmd->lmd_nal == OPENIBNAL || lmd->lmd_nal == IIBNAL) {
#endif
                if (lmd->lmd_server_nid == PTL_NID_ANY) {
                        if (ptl_parse_nid (&nid, hostname) != 0) {
                                fprintf (stderr, "%s: can't parse NID %s\n",
                                         progname, hostname);
                                return (-1);
                        }
                        lmd->lmd_server_nid = nid;
                }

                if (ptl_parse_ipaddr(&lmd->lmd_server_ipaddr, hostname) != 0) {
                        fprintf (stderr, "%s: can't parse host %s\n",
                                 progname, hostname);
                        return (-1);
                }
#ifndef CRAY_PORTALS
        } else if (lmd->lmd_nal == QSWNAL &&lmd->lmd_server_nid == PTL_NID_ANY){
                char buf[64];
                rc = sscanf(hostname, "%*[^0-9]%63[0-9]", buf);
                if (rc != 1) {
                        fprintf (stderr, "%s: can't get elan id from host %s\n",
                                 progname, hostname);
                        return -1;
                }
                if (ptl_parse_nid (&nid, buf) != 0) {
                        fprintf (stderr, "%s: can't parse NID %s\n",
                                 progname, hostname);
                        return (-1);
                }
                lmd->lmd_server_nid = nid;
#else
	} else if (lmd->lmd_nal == CRAY_KB_SSNAL) {
		lmd->lmd_server_nid = strtoll(hostname,0,0);
#endif
        }


        return 0;
}

int
build_data(char *source, char *options, struct lustre_mount_data *lmd)
{
        char buf[1024];
        char *hostname = NULL, *mds = NULL, *profile = NULL, *s;
        int rc;

        if (lmd_bad_magic(lmd))
                return 4;

        if (strlen(source) > sizeof(buf) + 1) {
                fprintf(stderr, "%s: host:/mds/profile argument too long\n",
                        progname);
                return 1;
        }
        strcpy(buf, source);
        if ((s = strchr(buf, ':'))) {
                hostname = buf;
                *s = '\0';

                while (*++s == '/')
                        ;
                mds = s;
                if ((s = strchr(mds, '/'))) {
                        *s = '\0';
                        profile = s + 1;
                } else {
                        fprintf(stderr, "%s: directory to mount not in "
                                "host:/mds/profile format\n",
                                progname);
                        return(1);
                }
        } else {
                fprintf(stderr, "%s: "
                        "directory to mount not in host:/mds/profile format\n",
                        progname);
                return(1);
        }

        rc = parse_options(options, lmd);
        if (rc)
                return rc;

        rc = set_local(lmd);
        if (rc)
                return rc;

        rc = set_peer(hostname, lmd);
        if (rc)
                return rc;
        if (strlen(mds) > sizeof(lmd->lmd_mds) + 1) {
                fprintf(stderr, "%s: mds name too long\n", progname);
                return(1);
        }
        strcpy(lmd->lmd_mds, mds);

        if (strlen(profile) > sizeof(lmd->lmd_profile) + 1) {
                fprintf(stderr, "%s: profile name too long\n", progname);
                return(1);
        }
        strcpy(lmd->lmd_profile, profile);

        if (verbose)
                print_options(lmd);
        return 0;
}

static int set_routes(struct lustre_mount_data *lmd) {
       struct portals_cfg pcfg;
       struct portal_ioctl_data data;
       int i, j, route_exists, rc, err = 0;

       register_ioc_dev(PORTALS_DEV_ID, PORTALS_DEV_PATH);

       for (i = 0; i < route_index; i++) {

               /* Check for existing routes so as not to add duplicates */
              for (j = 0; ; j++) {
                      PCFG_INIT(pcfg, NAL_CMD_GET_ROUTE);
                      pcfg.pcfg_nal = ROUTER;
                      pcfg.pcfg_count = j;

                      PORTAL_IOC_INIT(data);
                      data.ioc_pbuf1 = (char*)&pcfg;
                      data.ioc_plen1 = sizeof(pcfg);
                      data.ioc_nid = pcfg.pcfg_nid;

                      rc = l_ioctl(PORTALS_DEV_ID, IOC_PORTAL_NAL_CMD, &data);
                      if (rc != 0) {
                              route_exists = 0;
                              break;
                      }

                      if ((pcfg.pcfg_gw_nal == lmd->lmd_nal) &&
                          (pcfg.pcfg_nid    == routes[i].gw) &&
                          (pcfg.pcfg_nid2   == routes[i].lo) &&
                          (pcfg.pcfg_nid3   == routes[i].hi)) {
                              route_exists = 1;
                              break;
                      }
              }

              if (route_exists)
                      continue;

              PCFG_INIT(pcfg, NAL_CMD_ADD_ROUTE);
              pcfg.pcfg_nid = routes[i].gw;
              pcfg.pcfg_nal = ROUTER;
              pcfg.pcfg_gw_nal = lmd->lmd_nal;
              pcfg.pcfg_nid2 = MIN(routes[i].lo, routes[i].hi);
              pcfg.pcfg_nid3 = MAX(routes[i].lo, routes[i].hi);

              PORTAL_IOC_INIT(data);
              data.ioc_pbuf1 = (char*)&pcfg;
              data.ioc_plen1 = sizeof(pcfg);
              data.ioc_nid = pcfg.pcfg_nid;

              rc = l_ioctl(PORTALS_DEV_ID, IOC_PORTAL_NAL_CMD, &data);
              if (rc != 0) {
                      fprintf(stderr, "%s: Unable to add route "
                              LPX64" : "LPX64" - "LPX64"\n[%d] %s\n",
                              progname, routes[i].gw, routes[i].lo,
                              routes[i].hi, errno, strerror(errno));
                      err = -1;
                      break;
              }
       }

       unregister_ioc_dev(PORTALS_DEV_ID);
       return err;
}

void usage(FILE *out)
{
        fprintf(out, "usage: %s <source> <target> [-f] [-v] [-n] [-o mntopt]\n",
                progname);
        exit(out != stdout);
}

int main(int argc, char *const argv[])
{
        char *source, *target, *options = "";
        int i, nargs = 3, opt, rc;
        struct lustre_mount_data lmd;
        static struct option long_opt[] = {
                {"force", 0, 0, 'f'},
                {"help", 0, 0, 'h'},
                {"nomtab", 0, 0, 'n'},
                {"options", 1, 0, 'o'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };

        progname = strrchr(argv[0], '/');
        progname = progname ? progname + 1 : argv[0];

        while ((opt = getopt_long(argc, argv, "fno:v", long_opt, NULL)) != EOF){
                switch (opt) {
                case 'f':
                        ++force;
                        printf("force: %d\n", force);
                        nargs++;
                        break;
                case 'h':
                        usage(stdout);
                        break;
                case 'n':
                        ++nomtab;
                        printf("nomtab: %d\n", nomtab);
                        nargs++;
                        break;
                case 'o':
                        options = optarg;
                        nargs++;
                        break;
                case 'v':
                        ++verbose;
                        printf("verbose: %d\n", verbose);
                        nargs++;
                        break;
                default:
                        fprintf(stderr, "%s: unknown option '%c'\n",
                                progname, opt);
                        usage(stderr);
                        break;
                }
        }

        if (optind + 2 > argc) {
                fprintf(stderr, "%s: too few arguments\n", progname);
                usage(stderr);
        }

        source = argv[optind];
        target = argv[optind + 1];

        if (verbose) {
                for (i = 0; i < argc; i++)
                        printf("arg[%d] = %s\n", i, argv[i]);
                printf("source = %s, target = %s\n", source, target);
        }

        if (check_mtab_entry(source, target, "lustre"))
                exit(32);

        init_options(&lmd);
        rc = build_data(source, options, &lmd);
        if (rc) {
                exit(rc);
        }

        rc = set_routes(&lmd);
        if (rc) {
                exit(rc);
        }

        if (debug) {
                printf("%s: debug mode, not mounting\n", progname);
                exit(0);
        }

        rc = access(target, F_OK);
        if (rc) {
                rc = errno;
                fprintf(stderr, "%s: %s inaccessible: %s\n", progname, target,
                        strerror(errno));
                return rc;
        }

        rc = mount(source, target, "lustre", 0, (void *)&lmd);
        if (rc) {
                rc = errno;
                perror(argv[0]);
                fprintf(stderr, "%s: mount(%s, %s) failed: %s\n", source,
                        target, progname, strerror(errno));
                if (rc == ENODEV)
                        fprintf(stderr, "Are the lustre modules loaded?\n"
                             "Check /etc/modules.conf and /proc/filesystems\n");
                return 2;
        }
        update_mtab_entry(source, target, "lustre", options, 0, 0, 0);
        return 0;
}
