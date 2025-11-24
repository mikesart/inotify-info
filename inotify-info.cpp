/*
 * Copyright 2021 Michael Sartain
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE 1

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "inotify-info.h"
#include "lfqueue/lfqueue.h"

#ifndef INOTIFYINFO_VERSION
#error INOTIFYINFO_VERSION must be set
#endif

static int g_verbose = 0;
static size_t g_numthreads = 32;
static std::string g_search_path = "/"; // Default path to root
static int g_sort_by_instances = 0; // Sort list by instances instead of watches

/* true if at least one inotify watch is found in fdinfo files
 * On a system with no active inotify watches, but which otherwise
 * supports inotify watch info, this will prevent the watches column
 * from being displayed.
 * This case is indistinguishable from the case where the kernel does
 * not support inotify watch info.
 */
static int g_kernel_provides_watches_info = 0;

static char thousands_sep = ',';

static std::vector<std::string> ignore_dirs;

const char* RESET = "\x1b[0m";
const char* YELLOW = "\x1b[0;33m";
const char* CYAN = "\x1b[0;36m";
const char* BGRAY = "\x1b[1;30m";
const char* BGREEN = "\x1b[1;32m";
const char* BYELLOW = "\x1b[1;33m";
const char* BCYAN = "\x1b[1;36m";

void set_no_color()
{
    RESET = "";
    YELLOW = "";
    CYAN = "";
    BGRAY = "";
    BGREEN = "";
    BYELLOW = "";
    BCYAN = "";
}

/*
 * filename info
 */
struct filename_info_t {
    ino64_t inode; // Inode number
    dev_t dev; // Device ID containing file
    std::string filename;
};

/*
 * inotify process info
 */
struct procinfo_t {
    pid_t pid = 0;

    // uid
    uid_t uid = 0;

    // Count of inotify watches and instances
    uint32_t watches = 0;
    uint32_t instances = 0;

    // This appname or pid found in command line?
    bool in_cmd_line = false;

    // Full executable path
    std::string executable;

    // Executable basename
    std::string appname;

    // Inotify fdset filenames
    std::vector<std::string> fdset_filenames;

    // Device id map -> set of inodes for that device id
    std::unordered_map<dev_t, std::unordered_set<ino64_t>> dev_map;
};

class lfqueue_wrapper_t {
public:
    lfqueue_wrapper_t() { lfqueue_init(&queue); }
    ~lfqueue_wrapper_t() { lfqueue_destroy(&queue); }

    void queue_directory(char* path) { lfqueue_enq(&queue, path); }
    char* dequeue_directory() { return (char*)lfqueue_deq(&queue); }

public:
    typedef long long my_m256i __attribute__((__vector_size__(32), __aligned__(32)));

    union {
        lfqueue_t queue;
        my_m256i align_buf[4]; // Align to 128 bytes
    };
};

/*
 * shared thread data
 */
class thread_shared_data_t {
public:
    bool init(uint32_t numthreads, const std::vector<procinfo_t>& inotify_proclist);

public:
    // Array of queues - one per thread
    std::vector<lfqueue_wrapper_t> dirqueues;
    // Map of all inotify inodes watched to the set of devices they are on
    std::unordered_map<ino64_t, std::unordered_set<dev_t>> inode_set;
};

/*
 * thread info
 */
class thread_info_t {
public:
    thread_info_t(thread_shared_data_t& tdata_in)
        : tdata(tdata_in)
    {
    }
    ~thread_info_t() { }

    void queue_directory(char* path);
    char* dequeue_directory();

    // Returns -1: queue empty, 0: open error, > 0 success
    int parse_dirqueue_entry();

    void add_filename(ino64_t inode, const char* path, const char* d_name, bool is_dir);

public:
    uint32_t idx = 0;
    pthread_t pthread_id = 0;

    thread_shared_data_t& tdata;

    // Total dirs scanned by this thread
    uint32_t scanned_dirs = 0;
    // Files found by this thread
    std::vector<filename_info_t> found_files;
};

/*
 * getdents64 syscall
 */
GCC_DIAG_PUSH_OFF(pedantic)
struct linux_dirent64 {
    uint64_t d_ino; // Inode number
    int64_t d_off; // Offset to next linux_dirent
    unsigned short d_reclen; // Length of this linux_dirent
    unsigned char d_type; // File type
    char d_name[]; // Filename (null-terminated)
};
GCC_DIAG_POP()

int sys_getdents64(int fd, char* dirp, int count)
{
    return syscall(SYS_getdents64, fd, dirp, count);
}

static double gettime()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

std::string string_formatv(const char* fmt, va_list ap)
{
    std::string str;
    int size = 512;

    for (;;) {
        str.resize(size);
        int n = vsnprintf((char*)str.c_str(), size, fmt, ap);

        if ((n > -1) && (n < size)) {
            str.resize(n);
            return str;
        }

        size = (n > -1) ? (n + 1) : (size * 2);
    }
}

std::string string_format(const char* fmt, ...)
{
    va_list ap;
    std::string str;

    va_start(ap, fmt);
    str = string_formatv(fmt, ap);
    va_end(ap);

    return str;
}

static std::string get_link_name(const char* pathname)
{
    std::string Result;
    char filename[PATH_MAX + 1];

    ssize_t ret = readlink(pathname, filename, sizeof(filename));
    if ((ret > 0) && (ret < (ssize_t)sizeof(filename))) {
        filename[ret] = 0;
        Result = filename;
    }
    return Result;
}

static uid_t get_uid(const char* pathname)
{
    int fd = open(pathname, O_RDONLY, 0);

    if (fd >= 0) {
        char buf[16 * 1024];

        ssize_t len = read(fd, buf, sizeof(buf));

        close(fd);
        fd = -1;

        if (len > 0) {
            buf[len - 1] = 0;

            const char* uidstr = strstr(buf, "\nUid:");
            if (uidstr) {
                return atoll(uidstr + 5);
            }
        }
    }

    return -1;
}

static uint64_t get_token_val(const char* line, const char* token)
{
    const char* str = strstr(line, token);

    return str ? strtoull(str + strlen(token), nullptr, 16) : 0;
}

static uint32_t inotify_parse_fdinfo_file(procinfo_t& procinfo, const char* fdset_name)
{
    uint32_t watch_count = 0;

    FILE* fp = fopen(fdset_name, "r");
    if (fp) {
        char line_buf[256];

        procinfo.fdset_filenames.push_back(fdset_name);

        for (;;) {
            if (!fgets(line_buf, sizeof(line_buf), fp))
                break;

            /* sample fdinfo; inotify line added in linux 3.8, available if
             * kernel compiled with CONFIG_INOTIFY_USER and CONFIG_PROC_FS
             *   pos:    0
             *   flags:  00
             *   mnt_id: 15
             *   ino:    5865
             *   inotify wd:1 ino:80001 sdev:800011 mask:100 ignored_mask:0 fhandle-bytes:8 fhandle-type:1 f_handle:01000800bc1b8c7c
             */
            if (!strncmp(line_buf, "inotify ", 8)) {
                watch_count++;

                uint64_t inode_val = get_token_val(line_buf, "ino:");
                uint64_t sdev_val = get_token_val(line_buf, "sdev:");

                if (inode_val) {
                    // https://unix.stackexchange.com/questions/645937/listing-the-files-that-are-being-watched-by-inotify-instances
                    //   Assuming that the sdev field is encoded according to Linux's so-called "huge
                    //   encoding", which uses 20 bits (instead of 8) for minor numbers, in bitwise
                    //   parlance the major number is sdev >> 20 while the minor is sdev & 0xfffff.
                    unsigned int major = sdev_val >> 20;
                    unsigned int minor = sdev_val & 0xfffff;

                    // Add inode to this device map
                    procinfo.dev_map[makedev(major, minor)].insert(inode_val);
                }
            }
        }

        fclose(fp);
    }

    return watch_count;
}

static void inotify_parse_fddir(procinfo_t& procinfo)
{
    std::string filename = string_format("/proc/%d/fd", procinfo.pid);

    DIR* dir_fd = opendir(filename.c_str());
    if (!dir_fd)
        return;

    for (;;) {
        struct dirent* dp_fd = readdir(dir_fd);
        if (!dp_fd)
            break;

        if ((dp_fd->d_type == DT_LNK) && isdigit(dp_fd->d_name[0])) {
            filename = string_format("/proc/%d/fd/%s", procinfo.pid, dp_fd->d_name);
            filename = get_link_name(filename.c_str());

            if (filename == "anon_inode:inotify" || filename == "inotify") {
                filename = string_format("/proc/%d/fdinfo/%s", procinfo.pid, dp_fd->d_name);

                procinfo.instances++;
                procinfo.watches += inotify_parse_fdinfo_file(procinfo, filename.c_str());

                /* If any watches have been found, enable the stats display */
                g_kernel_provides_watches_info |= !!procinfo.watches;
            }
        }
    }

    closedir(dir_fd);
}

void thread_info_t::queue_directory(char* path)
{
    tdata.dirqueues[idx].queue_directory(path);
}

char* thread_info_t::dequeue_directory()
{
    char* path = tdata.dirqueues[idx].dequeue_directory();

    if (!path) {
        // Nothing on our queue, check queues on other threads
        for (lfqueue_wrapper_t& dirq : tdata.dirqueues) {
            path = dirq.dequeue_directory();
            if (path)
                break;
        }
    }

    return path;
}

// statx() was added to Linux in kernel 4.11; library support was added in glibc 2.28.
#if defined(__linux__) && ((__GLIBC__ >= 2 && __GLIBC_MINOR__ >= 28) || (__GLIBC__ > 2))

struct statx mystatx(const char* filename, unsigned int mask = 0)
{
    struct statx statxbuf;
    int flags = AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC;

    if (statx(0, filename, flags, mask, &statxbuf) == -1) {
        printf("ERROR: statx-ino( %s ) failed. Errno: %d (%s)\n", filename, errno, strerror(errno));
        memset(&statxbuf, 0, sizeof(statxbuf));
    }

    return statxbuf;
}

static dev_t stat_get_dev_t(const char* filename)
{
    struct statx statxbuf = mystatx(filename);

    return makedev(statxbuf.stx_dev_major, statxbuf.stx_dev_minor);
}

static uint64_t stat_get_ino(const char* filename)
{
    return mystatx(filename, STATX_INO).stx_ino;
}

#else

// Fall back to using stat() functions. Should work but be slower than using statx().

static dev_t stat_get_dev_t(const char* filename)
{
    struct stat statbuf;

    int ret = stat(filename, &statbuf);
    if (ret == -1) {
        printf("ERROR: stat-dev_t( %s ) failed. Errno: %d (%s)\n", filename, errno, strerror(errno));
        return 0;
    }
    return statbuf.st_dev;
}

static uint64_t stat_get_ino(const char* filename)
{
    struct stat statbuf;

    int ret = stat(filename, &statbuf);
    if (ret == -1) {
        printf("ERROR: stat-ino( %s ) failed. Errno: %d (%s)\n", filename, errno, strerror(errno));
        return 0;
    }

    return statbuf.st_ino;
}

#endif

void thread_info_t::add_filename(ino64_t inode, const char* path, const char* d_name, bool is_dir)
{
    auto it = tdata.inode_set.find(inode);

    if (it != tdata.inode_set.end()) {
        const std::unordered_set<dev_t>& dev_set = it->second;

        std::string filename = std::string(path) + d_name;
        dev_t dev = stat_get_dev_t(filename.c_str());

        // Make sure the inode AND device ID match before adding.
        if (dev_set.find(dev) != dev_set.end()) {
            filename_info_t fname;

            fname.filename = is_dir ? filename + "/" : filename;
            fname.inode = inode;
            fname.dev = dev;

            found_files.push_back(fname);
        }
    }
}

static bool is_dot_dir(const char* dname)
{
    if (dname[0] == '.') {
        if (!dname[1])
            return true;

        if ((dname[1] == '.') && !dname[2])
            return true;
    }

    return false;
}

// From "linux/magic.h"
#define PROC_SUPER_MAGIC 0x9fa0
#define SMB_SUPER_MAGIC 0x517B
#define CIFS_SUPER_MAGIC 0xFF534D42 /* the first four bytes of SMB PDUs */
#define SMB2_SUPER_MAGIC 0xFE534D42
#define FUSE_SUPER_MAGIC 0x65735546

// Detect proc and fuse directories and skip them.
//   https://github.com/mikesart/inotify-info/issues/6
// Could use setmntent("/proc/mounts", "r") + getmntent if speed is an issue?
static bool is_proc_dir(const char* path, const char* d_name)
{
    struct statfs s;
    std::string filename = std::string(path) + d_name;

    if (statfs(filename.c_str(), &s) == 0) {
        switch (s.f_type) {
        case PROC_SUPER_MAGIC:
        case FUSE_SUPER_MAGIC:
            return true;
        }
    }

    return false;
}

// Returns -1: queue empty, 0: open error, > 0 success
int thread_info_t::parse_dirqueue_entry()
{
    char __attribute__((aligned(16))) buf[1024];

    char* path = dequeue_directory();
    if (!path) {
        return -1;
    }

    for (std::string& dname : ignore_dirs) {
        if (dname == path) {
            if (g_verbose > 1) {
                printf("Ignoring '%s'\n", path);
            }
            return 0;
        }
    }

    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        free(path);
        return 0;
    }

    scanned_dirs++;

    size_t pathlen = strlen(path);

    for (;;) {
        int ret = sys_getdents64(fd, buf, sizeof(buf));

        if (ret < 0) {
            bool spew_error = true;

            if ((errno == 5) && !strncmp(path, "/sys/kernel/", 12)) {
                // In docker container we can get permission denied errors in /sys/kernel. Ignore them.
                // https://github.com/mikesart/inotify-info/issues/16
                spew_error = false;
            }

            if (spew_error) {
                printf("ERROR: sys_getdents64 failed on '%s': %d errno: %d (%s)\n", path, ret, errno, strerror(errno));
            }
            break;
        }
        if (ret == 0)
            break;

        for (int bpos = 0; bpos < ret;) {
            struct linux_dirent64* dirp = (struct linux_dirent64*)(buf + bpos);
            const char* d_name = dirp->d_name;

            // DT_BLK      This is a block device.
            // DT_CHR      This is a character device.
            // DT_FIFO     This is a named pipe (FIFO).
            // DT_SOCK     This is a UNIX domain socket.
            // DT_UNKNOWN  The file type could not be determined.

            // DT_REG      This is a regular file.
            // DT_LNK      This is a symbolic link.
            if (dirp->d_type == DT_REG || dirp->d_type == DT_LNK) {
                add_filename(dirp->d_ino, path, d_name, false);
            }
            // DT_DIR      This is a directory.
            else if (dirp->d_type == DT_DIR) {
                if (!is_dot_dir(d_name) && !is_proc_dir(path, d_name)) {
                    add_filename(dirp->d_ino, path, d_name, true);

                    size_t len = strlen(d_name);
                    char* newpath = (char*)malloc(pathlen + len + 2);

                    if (newpath) {
                        strcpy(newpath, path);
                        strcpy(newpath + pathlen, d_name);
                        newpath[pathlen + len] = '/';
                        newpath[pathlen + len + 1] = 0;

                        queue_directory(newpath);
                    }
                }
            }

            bpos += dirp->d_reclen;
        }
    }

    close(fd);
    free(path);
    return 1;
}

static void* parse_dirqueue_threadproc(void* arg)
{
    thread_info_t* pthread_info = (thread_info_t*)arg;

    for (;;) {
        // Loop until all the dequeue(s) fail
        if (pthread_info->parse_dirqueue_entry() == -1)
            break;
    }

    return nullptr;
}

static bool is_proc_in_cmdline_applist(const procinfo_t& procinfo, std::vector<std::string>& cmdline_applist)
{
    for (const std::string& str : cmdline_applist) {
        // Check if our command line string is a subset of this appname
        if (strstr(procinfo.appname.c_str(), str.c_str()))
            return true;

        // Check if the PIDs match
        if (atoll(str.c_str()) == procinfo.pid)
            return true;
    }

    return false;
}

static bool watch_count_is_greater(procinfo_t elem1, procinfo_t elem2)
{
    return elem1.watches > elem2.watches;
}

static bool instance_count_is_greater(procinfo_t elem1, procinfo_t elem2)
{
    return elem1.instances > elem2.instances;
}

static bool init_inotify_proclist(std::vector<procinfo_t>& inotify_proclist)
{
    DIR* dir_proc = opendir("/proc");

    if (!dir_proc) {
        printf("ERROR: opendir /proc failed: %d (%s)\n", errno, strerror(errno));
        return false;
    }

    for (;;) {
        struct dirent* dp_proc = readdir(dir_proc);
        if (!dp_proc)
            break;

        if ((dp_proc->d_type == DT_DIR) && isdigit(dp_proc->d_name[0])) {
            procinfo_t procinfo;

            procinfo.pid = atoll(dp_proc->d_name);

            std::string executable = string_format("/proc/%d/exe", procinfo.pid);
            std::string status = string_format("/proc/%d/status", procinfo.pid);
            procinfo.uid = get_uid(status.c_str());
            procinfo.executable = get_link_name(executable.c_str());
            if (!procinfo.executable.empty()) {
                procinfo.appname = basename((char*)procinfo.executable.c_str());

                inotify_parse_fddir(procinfo);

                if (procinfo.instances) {
                    inotify_proclist.push_back(procinfo);
                }
            }
        }
    }
    if (g_sort_by_instances)
        std::sort(inotify_proclist.begin(), inotify_proclist.end(), instance_count_is_greater);
    else
        std::sort(inotify_proclist.begin(), inotify_proclist.end(), watch_count_is_greater);

    closedir(dir_proc);
    return true;
}

// From:
//  https://stackoverflow.com/questions/1449805/how-to-format-a-number-using-comma-as-thousands-separator-in-c
size_t str_format_uint32(char dst[16], uint32_t num)
{
    if (thousands_sep) {
        char src[16];
        char* p_src = src;
        char* p_dst = dst;
        int num_len, commas;

        num_len = sprintf(src, "%u", num);

        for (commas = 2 - num_len % 3; *p_src; commas = (commas + 1) % 3) {
            *p_dst++ = *p_src++;
            if (commas == 1) {
                *p_dst++ = thousands_sep;
            }
        }
        *--p_dst = '\0';

        return (size_t)(p_dst - dst);
    }

    return sprintf(dst, "%u", num);
}

static void print_inotify_proclist(std::vector<procinfo_t>& inotify_proclist)
{
#if 0
    // test data
    procinfo_t proc_info = {};
    proc_info.pid = 100;
    proc_info.appname = "fsnotifier";
    proc_info.watches = 2;
    proc_info.instances = 1;
    inotify_proclist.push_back(proc_info);

    proc_info.pid = 1000;
    proc_info.appname = "evolution-addressbook-factor";
    proc_info.watches = 116;
    proc_info.instances = 10;
    inotify_proclist.push_back(proc_info);

    proc_info.pid = 22154;
    proc_info.appname = "evolution-addressbook-factor blah blah";
    proc_info.watches = 28200;
    proc_info.instances = 100;
    inotify_proclist.push_back(proc_info);

    proc_info.pid = 0x7fffffff;
    proc_info.appname = "evolution-addressbook-factor blah blah2";
    proc_info.watches = 999999;
    proc_info.instances = 999999999;
    inotify_proclist.push_back(proc_info);
#endif

    int lenPid = 10;
    int lenUid = 10;
    int lenApp = 10;
    int lenWatches = 8;
    int lenInstances = 10;

    for (procinfo_t& procinfo : inotify_proclist)
        lenApp = std::max<int>(procinfo.appname.length(), lenApp);

    /* If the number of watches is negative, the kernel doesn't support this info. omit the header*/
    if (g_kernel_provides_watches_info)
        printf("%s%*s %-*s %-*s %*s %*s%s\n",
            BCYAN, lenPid, "Pid", lenUid, "Uid", lenApp, "App", lenWatches, "Watches", lenInstances, "Instances", RESET);
    else
        printf("%s%*s %-*s %*s %*s%s\n",
            BCYAN, lenPid, "Pid", lenUid, "Uid", lenApp, "App", lenInstances, "Instances", RESET);

    for (procinfo_t& procinfo : inotify_proclist) {
        char watches_str[16];

        str_format_uint32(watches_str, procinfo.watches);

        if (g_kernel_provides_watches_info)
            printf("%*d %-*d %s%-*s%s %*s %*u\n",
                lenPid, procinfo.pid,
                lenUid, procinfo.uid,
                BYELLOW, lenApp, procinfo.appname.c_str(), RESET,
                lenWatches, watches_str,
                lenInstances, procinfo.instances);
        else
            printf("%*d %-*d %s%-*s%s %*u\n",
                lenPid, procinfo.pid,
                lenUid, procinfo.uid,
                BYELLOW, lenApp, procinfo.appname.c_str(), RESET,
                lenInstances, procinfo.instances);

        if (g_verbose > 1) {
            for (std::string& fname : procinfo.fdset_filenames) {
                printf("    %s%s%s\n", CYAN, fname.c_str(), RESET);
            }
        }

        if (procinfo.in_cmd_line) {
            for (const auto& it1 : procinfo.dev_map) {
                dev_t dev = it1.first;

                printf("%s[%u.%u]:%s", BGRAY, major(dev), minor(dev), RESET);
                for (const auto& it2 : it1.second) {
                    std::string inode_device_str = string_format("%lu", it2);

                    printf(" %s%s%s", BGRAY, inode_device_str.c_str(), RESET);
                }
                printf("\n");
            }
        }
    }
}

bool thread_shared_data_t::init(uint32_t numthreads, const std::vector<procinfo_t>& inotify_proclist)
{
    for (const procinfo_t& procinfo : inotify_proclist) {
        if (!procinfo.in_cmd_line)
            continue;

        for (const auto& it1 : procinfo.dev_map) {
            dev_t dev = it1.first;

            for (const auto& inode : it1.second) {
                inode_set[inode].insert(dev);
            }
        }
    }

    if (!inode_set.empty()) {
        dirqueues.resize(numthreads);
    }

    return !inode_set.empty();
}

static uint32_t find_files_in_inode_set(const std::vector<procinfo_t>& inotify_proclist,
    std::vector<filename_info_t>& all_found_files)
{
    thread_shared_data_t tdata;

    g_numthreads = std::max<size_t>(1, g_numthreads);

    if (!tdata.init(g_numthreads, inotify_proclist))
        return 0;
    printf("\n%sSearching '%s' for listed inodes...%s (%lu threads)\n",
        BCYAN, g_search_path.c_str(), RESET, g_numthreads);

    // Initialize thread_info_t array
    std::vector<class thread_info_t> thread_array(g_numthreads, thread_info_t(tdata));

    for (uint32_t idx = 0; idx < thread_array.size(); idx++) {
        thread_info_t& thread_info = thread_array[idx];

        thread_info.idx = idx;

        if (idx == 0) {
            // Add search dir in case someone is watching it
            thread_info.add_filename(stat_get_ino(g_search_path.c_str()), g_search_path.c_str(), "", false);
            // Add and parse root
            thread_info.queue_directory(strdup(g_search_path.c_str()));
            thread_info.parse_dirqueue_entry();
        } else if (pthread_create(&thread_info.pthread_id, NULL, &parse_dirqueue_threadproc, &thread_info)) {
            printf("Warning: pthread_create failed. errno: %d\n", errno);
            thread_info.pthread_id = 0;
        }
    }

    // Put main thread to work
    parse_dirqueue_threadproc(&thread_array[0]);

    uint32_t total_scanned_dirs = 0;
    for (const thread_info_t& thread_info : thread_array) {
        if (thread_info.pthread_id) {
            if (g_verbose > 1) {
                printf("Waiting for thread #%zu\n", thread_info.pthread_id);
            }

            void* status = NULL;
            int rc = pthread_join(thread_info.pthread_id, &status);

            if (g_verbose > 1) {
                printf("Thread #%zu rc=%d status=%d\n", thread_info.pthread_id, rc, (int)(intptr_t)status);
            }
        }

        // Snag data from this thread
        total_scanned_dirs += thread_info.scanned_dirs;

        all_found_files.insert(all_found_files.end(),
            thread_info.found_files.begin(), thread_info.found_files.end());

        if (g_verbose > 1) {
            printf("Thread #%zu: %u dirs, %zu files found\n",
                thread_info.pthread_id, thread_info.scanned_dirs, thread_info.found_files.size());
        }
    }

    struct
    {
        bool operator()(const filename_info_t& a, const filename_info_t& b) const
        {
            if (a.dev == b.dev)
                return a.inode < b.inode;
            return a.dev < b.dev;
        }
    } filename_info_less_func;

    std::sort(all_found_files.begin(), all_found_files.end(), filename_info_less_func);

    return total_scanned_dirs;
}

static uint32_t get_inotify_procfs_value(const std::string& fname)
{
    char buf[64];
    uint32_t val = 0;
    std::string filename = "/proc/sys/fs/inotify/" + fname;

    int fd = open(filename.c_str(), O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf)) > 0) {
            val = strtoul(buf, nullptr, 10);
        }

        close(fd);
    }

    return val;
}

static void print_inotify_limits()
{
    const std::vector<std::string> filenames = {
        "max_queued_events",
        "max_user_instances",
        "max_user_watches"
    };

    printf("%sINotify Limits:%s\n", BCYAN, RESET);
    for (const std::string& fname : filenames) {
        char str[16];
        uint32_t val = get_inotify_procfs_value(fname);

        str_format_uint32(str, val);

        printf("  %-20s %s%s%s\n", fname.c_str(), BGREEN, str, RESET);
    }
}

static uint32_t parse_config_file(const char* config_file)
{
    uint32_t dir_count = 0;

    FILE* fp = fopen(config_file, "r");
    if (fp) {
        char line_buf[8192];
        bool in_ignore_dirs_section = false;

        for (;;) {
            if (!fgets(line_buf, sizeof(line_buf) - 1, fp))
                break;

            if (line_buf[0] == '#') {
                // comment
            } else if (!in_ignore_dirs_section) {
                size_t len = strcspn(line_buf, "\r\n");

                if ((len == 12) && !strncmp("[ignoredirs]", line_buf, 12)) {
                    in_ignore_dirs_section = true;
                }
            } else if (line_buf[0] == '[') {
                in_ignore_dirs_section = false;
            } else if (in_ignore_dirs_section && (line_buf[0] == '/')) {
                size_t len = strcspn(line_buf, "\r\n");

                if (len > 1) {
                    line_buf[len] = 0;
                    if (line_buf[len - 1] != '/') {
                        line_buf[len] = '/';
                        line_buf[len + 1] = '\0';
                    }

                    ignore_dirs.push_back(line_buf);
                    dir_count++;
                }
            }
        }

        fclose(fp);
    }

    return dir_count;
}

static bool parse_ignore_dirs_file()
{
    const std::string filename = "inotify-info.config";

    const char* xdg_config_dir = getenv("XDG_CONFIG_HOME");
    if (xdg_config_dir) {
        std::string config_file = std::string(xdg_config_dir) + "/" + filename;
        if (parse_config_file(config_file.c_str()))
            return true;

        config_file = std::string(xdg_config_dir) + "/.config/" + filename;
        if (parse_config_file(config_file.c_str()))
            return true;
    }

    const char* home_dir = getenv("HOME");
    if (home_dir) {
        std::string config_file = std::string(home_dir) + "/" + filename;
        if (parse_config_file(config_file.c_str()))
            return true;
    }

    std::string config_file = "/etc/" + filename;
    if (parse_config_file(config_file.c_str()))
        return true;

    return false;
}

void parse_search_dir()
{
    // Check if the path is a valid directory
    struct stat path_stat;
    if (stat(g_search_path.c_str(), &path_stat) == 0) {
        if (S_ISDIR(path_stat.st_mode)) {
            // Ensure the g_search_path ends with "/"
            if (g_search_path.back() != '/') {
                g_search_path += '/';
            }
        } else {
            // g_search_path exists but is not a directory
            printf("ERROR: path (%s) is not a directory. Errno: %d (%s)\n", g_search_path.c_str(), errno, strerror(errno));
            exit(1);
        }
    } else {
        // g_search_path does not exist
        printf("ERROR: path (%s) does not exist. Errno: %d (%s)\n", g_search_path.c_str(), errno, strerror(errno));
        exit(1);
    }
}

static void print_version()
{
    printf("%s\n", INOTIFYINFO_VERSION);
}

static void print_usage(const char* appname)
{
    printf("Usage: %s [options] [appname | pid...]\n", appname);
    printf("Where options are:\n");
    printf("    [--threads=##]        Number of threads\n");
    printf("    [-p=PATH]\n");
    printf("    [--path=PATH]         Path to search (default '/')\n");
    printf("    [--ignoredir=NAME]    Directories to ignore in searched path\n");
    printf("    [--sort-by-instances] Sort list by instances instead of watches\n");
    printf("    [-v|--verbose]        More option increase verbosity level\n");
    printf("    [--no-color]          Do not colorize output\n");
    printf("    [--version]           Show version and stop\n");
    printf("    [-?|-h|--help]        Show this help and stop\n");
}

static void parse_cmdline(int argc, char** argv, std::vector<std::string>& cmdline_applist)
{
    static struct option long_opts[] = {
        { "verbose", no_argument, 0, 0 },
        { "no-color", no_argument, 0, 0 },
        { "threads", required_argument, 0, 0 },
        { "ignoredir", required_argument, 0, 0 },
        { "path", required_argument, 0, 0 },
        { "sort-by-instances", no_argument, 0, 0 },
        { "version", no_argument, 0, 0 },
        { "help", no_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    // Let's pick the number of processors online (with a max of 32) for a default.
    g_numthreads = std::min<uint32_t>(g_numthreads, sysconf(_SC_NPROCESSORS_ONLN));

    int c;
    int opt_ind = 0;
    while ((c = getopt_long(argc, argv, "p:?hv", long_opts, &opt_ind)) != -1) {
        switch (c) {
        case 0:
            if (!strcasecmp("help", long_opts[opt_ind].name)) {
                print_usage(argv[0]);
                exit(0);
            };
            if (!strcasecmp("version", long_opts[opt_ind].name)) {
                print_version();
                exit(0);
            }
            if (!strcasecmp("verbose", long_opts[opt_ind].name))
                g_verbose++;
            else if (!strcasecmp("no-color", long_opts[opt_ind].name))
                set_no_color();
            else if (!strcasecmp("threads", long_opts[opt_ind].name))
                g_numthreads = atoi(optarg);
            else if (!strcasecmp("ignoredir", long_opts[opt_ind].name)) {
                std::string dirname = optarg;
                if (dirname.size() > 1) {
                    if (optarg[dirname.size() - 1] != '/')
                        dirname += "/";
                    ignore_dirs.push_back(dirname);
                }
            } else if (!strcasecmp("path", long_opts[opt_ind].name)) {
                g_search_path = optarg;
            } else if (!strcasecmp("sort-by-instances", long_opts[opt_ind].name)) {
                g_sort_by_instances = 1;
            }
            break;
        case 'p':
            g_search_path = optarg;
            break;
        case 'v':
            g_verbose++;
            break;
        case 'h':
        case '?':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(-1);
            break;
        }
    }

    for (; optind < argc; optind++) {
        cmdline_applist.push_back(argv[optind]);
    }

    parse_ignore_dirs_file();
    parse_search_dir();

    if (g_verbose > 1) {
        printf("%lu ignore_dirs:\n", ignore_dirs.size());

        for (std::string& dname : ignore_dirs) {
            printf("  '%s'\n", dname.c_str());
        }
    }
}

static void print_separator()
{
    printf("%s%s%s\n", YELLOW, std::string(78, '-').c_str(), RESET);
}

int main(int argc, char* argv[])
{
    std::vector<std::string> cmdline_applist;
    std::vector<procinfo_t> inotify_proclist;

    struct lconv* env = localeconv();
    if (env && env->thousands_sep && env->thousands_sep[0]) {
        thousands_sep = env->thousands_sep[0];
    }

    parse_cmdline(argc, argv, cmdline_applist);
    print_separator();

    print_inotify_limits();
    print_separator();

    if (init_inotify_proclist(inotify_proclist)) {
        uint32_t total_watches = 0;
        uint32_t total_instances = 0;
        std::vector<filename_info_t> all_found_files;

        for (procinfo_t& procinfo : inotify_proclist) {
            procinfo.in_cmd_line = is_proc_in_cmdline_applist(procinfo, cmdline_applist);

            total_watches += procinfo.watches;
            total_instances += procinfo.instances;
        }

        if (inotify_proclist.size()) {
            print_inotify_proclist(inotify_proclist);
            print_separator();
        }

        if (g_kernel_provides_watches_info)
            printf("Total inotify Watches:   %s%u%s\n", BGREEN, total_watches, RESET);
        printf("Total inotify Instances: %s%u%s\n", BGREEN, total_instances, RESET);
        print_separator();

        double search_time = gettime();
        uint32_t total_scanned_dirs = find_files_in_inode_set(inotify_proclist, all_found_files);
        if (total_scanned_dirs) {
            search_time = gettime() - search_time;

            for (const filename_info_t& fname_info : all_found_files) {
                printf("%s%9lu%s [%u:%u] %s\n", BGREEN, fname_info.inode, RESET,
                    major(fname_info.dev), minor(fname_info.dev),
                    fname_info.filename.c_str());
            }

            setlocale(LC_NUMERIC, "");
            GCC_DIAG_PUSH_OFF(format)
            printf("\n%'u dirs scanned (%.2f seconds)\n", total_scanned_dirs, search_time);
            GCC_DIAG_POP()
        }
    }

    return 0;
}
