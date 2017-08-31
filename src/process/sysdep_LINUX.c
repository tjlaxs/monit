/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */


#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_ASM_PARAM_H
#include <asm/param.h>
#endif

#ifdef HAVE_GLOB_H
#include <glob.h>
#endif

#ifdef HAVE_SYS_SYSINFO_H
#include <sys/sysinfo.h>
#endif

#include "monit.h"
#include "ProcessTree.h"
#include "process_sysdep.h"

// libmonit
#include "system/Time.h"

/**
 *  System dependent resource data collection code for Linux.
 *
 *  @file
 */


/* ------------------------------------------------------------- Definitions */


static struct {
        int hasIOStatistics; // True if /proc/<PID>/io is present
} _statistics = {};


/* --------------------------------------- Static constructor and destructor */


static void __attribute__ ((constructor)) _constructor() {
        struct stat sb;
        _statistics.hasIOStatistics = stat("/proc/self/io", &sb) == 0 ? true : false;
}


/* ----------------------------------------------------------------- Private */


#define NSEC_PER_SEC    1000000000L

static unsigned long long old_cpu_user     = 0;
static unsigned long long old_cpu_syst     = 0;
static unsigned long long old_cpu_wait     = 0;
static unsigned long long old_cpu_total    = 0;

static long page_size = 0;

static double hz = 0.;

/**
 * Get system start time
 * @return seconds since unix epoch
 */
static time_t get_starttime() {
        struct sysinfo info;
        if (sysinfo(&info) < 0) {
                LogError("system statistic error -- cannot get system uptime: %s\n", STRERROR);
                return 0;
        }
        return Time_now() - info.uptime;
}


/* ------------------------------------------------------------------ Public */


boolean_t init_process_info_sysdep(void) {
        if ((hz = sysconf(_SC_CLK_TCK)) <= 0.) {
                DEBUG("system statistic error -- cannot get hz: %s\n", STRERROR);
                return false;
        }

        if ((page_size = sysconf(_SC_PAGESIZE)) <= 0) {
                DEBUG("system statistic error -- cannot get page size: %s\n", STRERROR);
                return false;
        }

        if ((systeminfo.cpu.count = sysconf(_SC_NPROCESSORS_CONF)) < 0) {
                DEBUG("system statistic error -- cannot get cpu count: %s\n", STRERROR);
                return false;
        } else if (systeminfo.cpu.count == 0) {
                DEBUG("system reports cpu count 0, setting dummy cpu count 1\n");
                systeminfo.cpu.count = 1;
        }

        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
                char line[STRLEN];
                systeminfo.memory.size = 0L;
                while (fgets(line, sizeof(line), f)) {
                        if (sscanf(line, "MemTotal: %"PRIu64, &systeminfo.memory.size) == 1) {
                                systeminfo.memory.size *= 1024;
                                break;
                        }
                }
                fclose(f);
                if (! systeminfo.memory.size)
                        DEBUG("system statistic error -- cannot get real memory amount\n");
        } else {
                DEBUG("system statistic error -- cannot open /proc/meminfo\n");
        }

        f = fopen("/proc/stat", "r");
        if (f) {
                char line[STRLEN];
                systeminfo.booted = 0;
                while (fgets(line, sizeof(line), f)) {
                        if (sscanf(line, "btime %"PRIu64, &systeminfo.booted) == 1) {
                                break;
                        }
                }
                fclose(f);
                if (! systeminfo.booted)
                        DEBUG("system statistic error -- cannot get system boot time\n");
        } else {
                DEBUG("system statistic error -- cannot open /proc/stat\n");
        }

        return true;
}


/**
 * Read all processes of the proc files system to initialize the process tree
 * @param reference reference of ProcessTree
 * @param pflags Process engine flags
 * @return treesize > 0 if succeeded otherwise 0
 */
int initprocesstree_sysdep(ProcessTree_T **reference, ProcessEngine_Flags pflags) {
        int                 rv, bytes = 0;
        int                 treesize = 0;
        int                 stat_pid = 0;
        int                 stat_ppid = 0;
        int                 stat_uid = 0;
        int                 stat_euid = 0;
        int                 stat_gid = 0;
        char               *tmp = NULL;
        char                procname[STRLEN];
        char                buf[4096];
        char                stat_item_state;
        long                stat_item_cutime = 0;
        long                stat_item_cstime = 0;
        long                stat_item_rss = 0;
        int                 stat_item_threads = 0;
        glob_t              globbuf;
        unsigned long       stat_item_utime = 0;
        unsigned long       stat_item_stime = 0;
        unsigned long long  stat_item_starttime = 0ULL;
        uint64_t            stat_read_bytes = 0ULL;
        uint64_t            stat_write_bytes = 0ULL;

        ASSERT(reference);

        /* Find all processes in the /proc directory */
        if ((rv = glob("/proc/[0-9]*", 0, NULL, &globbuf))) {
                LogError("system statistic error -- glob failed: %d (%s)\n", rv, STRERROR);
                return 0;
        }

        treesize = globbuf.gl_pathc;

        ProcessTree_T *pt = CALLOC(sizeof(ProcessTree_T), treesize);

        /* Insert data from /proc directory */
        time_t starttime = get_starttime();
        for (int i = 0; i < treesize; i++) {
                stat_pid = atoi(globbuf.gl_pathv[i] + 6); // skip "/proc/"

                /********** /proc/PID/stat **********/
                if (! file_readProc(buf, sizeof(buf), "stat", stat_pid, NULL)) {
                        DEBUG("system statistic error -- cannot read /proc/%d/stat\n", stat_pid);
                        continue;
                }
                if (! (tmp = strrchr(buf, ')'))) {
                        DEBUG("system statistic error -- file /proc/%d/stat parse error\n", stat_pid);
                        continue;
                }
                *tmp = 0;
                if (sscanf(buf, "%*d (%255s", procname) != 1) {
                        DEBUG("system statistic error -- file /proc/%d/stat process name parse error\n", stat_pid);
                        continue;
                }
                tmp += 2;
                if (sscanf(tmp,
                           "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %ld %ld %*d %*d %d %*u %llu %*u %ld %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d %*d\n",
                           &stat_item_state,
                           &stat_ppid,
                           &stat_item_utime,
                           &stat_item_stime,
                           &stat_item_cutime,
                           &stat_item_cstime,
                           &stat_item_threads,
                           &stat_item_starttime,
                           &stat_item_rss) != 9) {
                        DEBUG("system statistic error -- file /proc/%d/stat parse error\n", stat_pid);
                        continue;
                }

                /********** /proc/PID/status **********/
                if (! file_readProc(buf, sizeof(buf), "status", stat_pid, NULL)) {
                        DEBUG("system statistic error -- cannot read /proc/%d/status\n", stat_pid);
                        continue;
                }
                if (! (tmp = strstr(buf, "Uid:"))) {
                        DEBUG("system statistic error -- cannot find process uid\n");
                        continue;
                }
                if (sscanf(tmp + 4, "\t%d\t%d", &stat_uid, &stat_euid) != 2) {
                        DEBUG("system statistic error -- cannot read process uid\n");
                        continue;
                }
                if (! (tmp = strstr(buf, "Gid:"))) {
                        DEBUG("system statistic error -- cannot find process gid\n");
                        continue;
                }
                if (sscanf(tmp + 4, "\t%d", &stat_gid) != 1) {
                        DEBUG("system statistic error -- cannot read process gid\n");
                        continue;
                }

                /********** /proc/PID/io **********/
                if (_statistics.hasIOStatistics) {
                        if (file_readProc(buf, sizeof(buf), "io", stat_pid, NULL)) {
                                if (! (tmp = strstr(buf, "read_bytes:"))) {
                                        DEBUG("system statistic error -- cannot find process read_bytes\n");
                                        continue;
                                }
                                if (sscanf(tmp + 11, "\t%"PRIu64, &stat_read_bytes) != 1) {
                                        DEBUG("system statistic error -- cannot get process read bytes\n");
                                        continue;
                                }
                                if (! (tmp = strstr(buf, "write_bytes:"))) {
                                        DEBUG("system statistic error -- cannot find process write_bytes\n");
                                        continue;
                                }
                                if (sscanf(tmp + 12, "\t%"PRIu64, &stat_write_bytes) != 1) {
                                        DEBUG("system statistic error -- cannot get process write bytes\n");
                                        continue;
                                }
                        }
                }

                /********** /proc/PID/cmdline **********/
                if (pflags & ProcessEngine_CollectCommandLine) {
                        if (! file_readProc(buf, sizeof(buf), "cmdline", stat_pid, &bytes)) {
                                DEBUG("system statistic error -- cannot read /proc/%d/cmdline\n", stat_pid);
                                continue;
                        }
                        for (int j = 0; j < (bytes - 1); j++) // The cmdline file contains argv elements/strings terminated separated by '\0' => join the string
                                if (buf[j] == 0)
                                        buf[j] = ' ';
                        pt[i].cmdline = Str_dup(*buf ? buf : procname);
                }

                /* Set the data in ptree only if all process related reads succeeded (prevent partial data in the case that continue was called during data collecting) */
                pt[i].pid = stat_pid;
                pt[i].ppid = stat_ppid;
                pt[i].cred.uid = stat_uid;
                pt[i].cred.euid = stat_euid;
                pt[i].cred.gid = stat_gid;
                pt[i].threads = stat_item_threads;
                pt[i].uptime = starttime > 0 ? (systeminfo.time / 10. - (starttime + (time_t)(stat_item_starttime / hz))) : 0;
                pt[i].cpu.time = (double)(stat_item_utime + stat_item_stime) / hz * 10.; // jiffies -> seconds = 1/hz
                pt[i].memory.usage = (uint64_t)stat_item_rss * (uint64_t)page_size;
                pt[i].read.bytes = stat_read_bytes;
                pt[i].write.bytes = stat_write_bytes;
                pt[i].zombie = stat_item_state == 'Z' ? true : false;
        }

        *reference = pt;
        globfree(&globbuf);

        return treesize;
}


/**
 * This routine returns 'nelem' double precision floats containing
 * the load averages in 'loadv'; at most 3 values will be returned.
 * @param loadv destination of the load averages
 * @param nelem number of averages
 * @return: 0 if successful, -1 if failed (and all load averages are 0).
 */
int getloadavg_sysdep(double *loadv, int nelem) {
#ifdef HAVE_GETLOADAVG
        return getloadavg(loadv, nelem);
#else
        char buf[STRLEN];
        double load[3];
        if (! file_readProc(buf, sizeof(buf), "loadavg", -1, NULL))
                return -1;
        if (sscanf(buf, "%lf %lf %lf", &load[0], &load[1], &load[2]) != 3) {
                DEBUG("system statistic error -- cannot get load average\n");
                return -1;
        }
        for (int i = 0; i < nelem; i++)
                loadv[i] = load[i];
        return 0;
#endif
}


/**
 * This routine returns real memory in use.
 * @return: true if successful, false if failed
 */
boolean_t used_system_memory_sysdep(SystemInfo_T *si) {
        char          *ptr;
        char           buf[2048];
        unsigned long  mem_free = 0UL;
        unsigned long  buffers = 0UL;
        unsigned long  cached = 0UL;
        unsigned long  slabreclaimable = 0UL;
        unsigned long  swap_total = 0UL;
        unsigned long  swap_free = 0UL;
        uint64_t       zfsarcsize = 0ULL;

        if (! file_readProc(buf, sizeof(buf), "meminfo", -1, NULL)) {
                LogError("system statistic error -- cannot get real memory free amount\n");
                goto error;
        }

        /* Memory */
        if (! (ptr = strstr(buf, "MemFree:")) || sscanf(ptr + 8, "%ld", &mem_free) != 1) {
                LogError("system statistic error -- cannot get real memory free amount\n");
                goto error;
        }
        if (! (ptr = strstr(buf, "Buffers:")) || sscanf(ptr + 8, "%ld", &buffers) != 1)
                DEBUG("system statistic error -- cannot get real memory buffers amount\n");
        if (! (ptr = strstr(buf, "Cached:")) || sscanf(ptr + 7, "%ld", &cached) != 1)
                DEBUG("system statistic error -- cannot get real memory cache amount\n");
        if (! (ptr = strstr(buf, "SReclaimable:")) || sscanf(ptr + 13, "%ld", &slabreclaimable) != 1)
                DEBUG("system statistic error -- cannot get slab reclaimable memory amount\n");
        FILE *f = fopen("/proc/spl/kstat/zfs/arcstats", "r");
        if (f) {
                char line[STRLEN];
                while (fgets(line, sizeof(line), f)) {
                        if (sscanf(line, "size %*d %"PRIu64, &zfsarcsize) == 1) {
                                break;
                        }
                }
                fclose(f);
        }
        si->memory.usage.bytes = systeminfo.memory.size - zfsarcsize - (uint64_t)(mem_free + buffers + cached + slabreclaimable) * 1024;

        /* Swap */
        if (! (ptr = strstr(buf, "SwapTotal:")) || sscanf(ptr + 10, "%ld", &swap_total) != 1) {
                LogError("system statistic error -- cannot get swap total amount\n");
                goto error;
        }
        if (! (ptr = strstr(buf, "SwapFree:")) || sscanf(ptr + 9, "%ld", &swap_free) != 1) {
                LogError("system statistic error -- cannot get swap free amount\n");
                goto error;
        }
        si->swap.size = (uint64_t)swap_total * 1024;
        si->swap.usage.bytes = (uint64_t)(swap_total - swap_free) * 1024;

        return true;

error:
        si->memory.usage.bytes = 0ULL;
        si->swap.size = 0ULL;
        return false;
}


/**
 * This routine returns system/user CPU time in use.
 * @return: true if successful, false if failed (or not available)
 */
boolean_t used_system_cpu_sysdep(SystemInfo_T *si) {
        boolean_t rv;
        unsigned long long cpu_total;
        unsigned long long cpu_user;
        unsigned long long cpu_nice;
        unsigned long long cpu_syst;
        unsigned long long cpu_idle;
        unsigned long long cpu_wait;
        unsigned long long cpu_irq;
        unsigned long long cpu_softirq;
        char buf[STRLEN];

        if (! file_readProc(buf, sizeof(buf), "stat", -1, NULL)) {
                LogError("system statistic error -- cannot read /proc/stat\n");
                goto error;
        }

        rv = sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu",
                    &cpu_user,
                    &cpu_nice,
                    &cpu_syst,
                    &cpu_idle,
                    &cpu_wait,
                    &cpu_irq,
                    &cpu_softirq);
        if (rv < 4) {
                LogError("system statistic error -- cannot read cpu usage\n");
                goto error;
        } else if (rv == 4) {
                /* linux 2.4.x doesn't support these values */
                cpu_wait    = 0;
                cpu_irq     = 0;
                cpu_softirq = 0;
        }

        cpu_total = cpu_user + cpu_nice + cpu_syst + cpu_idle + cpu_wait + cpu_irq + cpu_softirq;
        cpu_user  = cpu_user + cpu_nice;

        if (old_cpu_total == 0) {
                si->cpu.usage.user = -1.;
                si->cpu.usage.system = -1.;
                si->cpu.usage.wait = -1.;
        } else {
                unsigned long long delta = cpu_total - old_cpu_total;

                si->cpu.usage.user = 100. * (double)(cpu_user - old_cpu_user) / delta;
                si->cpu.usage.system = 100. * (double)(cpu_syst - old_cpu_syst) / delta;
                si->cpu.usage.wait = 100. * (double)(cpu_wait - old_cpu_wait) / delta;
        }

        old_cpu_user  = cpu_user;
        old_cpu_syst  = cpu_syst;
        old_cpu_wait  = cpu_wait;
        old_cpu_total = cpu_total;
        return true;

error:
        si->cpu.usage.user = 0.;
        si->cpu.usage.system = 0.;
        si->cpu.usage.wait = 0.;
        return false;
}


