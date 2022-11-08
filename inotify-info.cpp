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

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#include "inotify-info.h"
#include "lfqueue/lfqueue.h"

/*
 * TODO
 *  - Comments
 *  - Disable color
 */

static int g_verbose = 0;
static size_t g_numthreads = 32;

/*
 * filename info
 */
struct filename_info_t
{
    ino64_t inode;  // Inode number
    dev_t dev;      // Device ID containing file
    std::string filename;
};

/*
 * inotify process info
 */
struct procinfo_t
{
    pid_t pid = 0;

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
    std::vector< std::string > fdset_filenames;

    // Device id map -> set of inodes for that device id
    std::unordered_map< dev_t, std::unordered_set< ino64_t > > dev_map;
};

class lfqueue_wrapper_t
{
public:
    lfqueue_wrapper_t()                { lfqueue_init( &queue ); }
    ~lfqueue_wrapper_t()               { lfqueue_destroy( &queue ); }

    void queue_directory( char *path ) { lfqueue_enq( &queue, path ); }
    char *dequeue_directory()          { return ( char * )lfqueue_deq( &queue ); }

public:
    typedef long long my_m256i __attribute__((__vector_size__(32), __aligned__(32)));

    union
    {
        lfqueue_t queue;
        my_m256i align_buf[4]; // Align to 128 bytes
    };
};

/*
 * shared thread data
 */
class thread_shared_data_t
{
public:
    bool init( uint32_t numthreads, const std::vector< procinfo_t > &inotify_proclist );

public:
    // Array of queues - one per thread
    std::vector< lfqueue_wrapper_t > dirqueues;
    // Map of all inotify inodes watched to the set of devices they are on
    std::unordered_map< ino64_t, std::unordered_set< dev_t > > inode_set;
};

/*
 * thread info
 */
class thread_info_t
{
public:
    thread_info_t( thread_shared_data_t &tdata_in ) : tdata( tdata_in ) {}
    ~thread_info_t() {}

    void queue_directory( char *path );
    char *dequeue_directory();

    // Returns -1: queue empty, 0: open error, > 0 success
    int parse_dirqueue_entry();

    void add_filename( ino64_t inode, const char *path, const char *d_name, bool is_dir );

public:
    uint32_t idx = 0;
    pthread_t pthread_id = 0;

    thread_shared_data_t &tdata;

    // Total dirs scanned by this thread
    uint32_t scanned_dirs = 0;
    // Files found by this thread
    std::vector< filename_info_t > found_files;
};

/*
 * getdents64 syscall
 */
GCC_DIAG_PUSH_OFF( pedantic )
struct linux_dirent64
{
    uint64_t d_ino;          // Inode number
    int64_t d_off;           // Offset to next linux_dirent
    unsigned short d_reclen; // Length of this linux_dirent
    unsigned char d_type;    // File type
    char d_name[];           // Filename (null-terminated)
};
GCC_DIAG_POP()

int sys_getdents64( int fd, char *dirp, int count )
{
    return syscall( SYS_getdents64, fd, dirp, count );
}

static double gettime()
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ( double )ts.tv_sec + ( double )ts.tv_nsec / 1e9;
}

std::string string_formatv( const char *fmt, va_list ap )
{
    std::string str;
    int size = 512;

    for ( ;; )
    {
        str.resize( size );
        int n = vsnprintf( ( char * )str.c_str(), size, fmt, ap );

        if ( ( n > -1 ) && ( n < size ) )
        {
            str.resize( n );
            return str;
        }

        size = ( n > -1 ) ? ( n + 1 ) : ( size * 2 );
    }
}

std::string string_format( const char *fmt, ... )
{
    va_list ap;
    std::string str;

    va_start( ap, fmt );
    str = string_formatv( fmt, ap );
    va_end( ap );

    return str;
}

static std::string get_link_name( const char *pathname )
{
    std::string Result;
    char filename[ PATH_MAX + 1 ];

    ssize_t ret = readlink( pathname, filename, sizeof( filename ) );
    if ( ( ret > 0 ) && ( ret < ( ssize_t )sizeof( filename ) ) )
    {
        filename[ ret ] = 0;
        Result = filename;
    }
    return Result;
}

static uint64_t get_token_val( const char *line, const char *token )
{
    const char *str = strstr( line, token );

    return str ? strtoull( str + strlen( token ), nullptr, 16 ) : 0;
}

static uint32_t inotify_parse_fdinfo_file( procinfo_t &procinfo, const char *fdset_name )
{
    uint32_t watch_count = 0;

    FILE *fp = fopen( fdset_name, "r" );
    if ( fp )
    {
        char line_buf[ 256 ];

        procinfo.fdset_filenames.push_back( fdset_name );

        for ( ;; )
        {
            if ( !fgets( line_buf, sizeof( line_buf ), fp ) )
                break;

            if ( !strncmp( line_buf, "inotify ", 8 ) )
            {
                watch_count++;

                uint64_t inode_val = get_token_val( line_buf, "ino:" );
                uint64_t sdev_val = get_token_val( line_buf, "sdev:" );

                if ( inode_val )
                {
                    // https://unix.stackexchange.com/questions/645937/listing-the-files-that-are-being-watched-by-inotify-instances
                    //   Assuming that the sdev field is encoded according to Linux's so-called "huge
                    //   encoding", which uses 20 bits (instead of 8) for minor numbers, in bitwise
                    //   parlance the major number is sdev >> 20 while the minor is sdev & 0xfffff.
                    unsigned int major = sdev_val >> 20;
                    unsigned int minor = sdev_val & 0xfffff;

                    // Add inode to this device map
                    procinfo.dev_map[ makedev( major, minor ) ].insert( inode_val );
                }
            }
        }

        fclose( fp );
    }

    return watch_count;
}

static void inotify_parse_fddir( procinfo_t &procinfo )
{
    std::string filename = string_format( "/proc/%d/fd", procinfo.pid );

    DIR *dir_fd = opendir( filename.c_str() );
    if ( !dir_fd )
        return;

    for ( ;; )
    {
        struct dirent *dp_fd = readdir( dir_fd );
        if ( !dp_fd )
            break;

        if ( ( dp_fd->d_type == DT_LNK ) && isdigit( dp_fd->d_name[ 0 ] ) )
        {
            filename = string_format( "/proc/%d/fd/%s", procinfo.pid, dp_fd->d_name );
            filename = get_link_name( filename.c_str() );

            if ( filename == "anon_inode:inotify" )
            {
                filename = string_format( "/proc/%d/fdinfo/%s", procinfo.pid, dp_fd->d_name );

                uint32_t count = inotify_parse_fdinfo_file( procinfo, filename.c_str() );
                if ( count )
                {
                    procinfo.instances++;
                    procinfo.watches += count;
                }
            }
        }
    }

    closedir( dir_fd );
}

void thread_info_t::queue_directory( char *path )
{
    tdata.dirqueues[ idx ].queue_directory( path );
}

char *thread_info_t::dequeue_directory()
{
    char *path = tdata.dirqueues[ idx ].dequeue_directory();

    if ( !path )
    {
        // Nothing on our queue, check queues on other threads
        for ( lfqueue_wrapper_t &dirq : tdata.dirqueues )
        {
            path = dirq.dequeue_directory();
            if ( path )
                break;
        }
    }

    return path;
}

struct statx mystatx( const char *filename, unsigned int mask = 0 )
{
    struct statx statxbuf;
    int flags = AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC;

    if ( statx( 0, filename, flags, mask, &statxbuf ) == -1 )
    {
        printf( "ERROR: statx-ino( %s ) failed. Errno: %d\n", filename, errno );
        memset( &statxbuf, 0, sizeof( statxbuf ) );
    }

    return statxbuf;
}

void thread_info_t::add_filename( ino64_t inode, const char *path, const char *d_name, bool is_dir )
{
    auto it = tdata.inode_set.find( inode );

    if ( it != tdata.inode_set.end() )
    {
        const std::unordered_set< dev_t > &dev_set = it->second;

        std::string filename = std::string( path ) + d_name;
        struct statx statxbuf = mystatx( filename.c_str() );
        dev_t dev = makedev( statxbuf.stx_dev_major, statxbuf.stx_dev_minor );

        // Make sure the inode AND device ID match before adding.
        if ( dev_set.find( dev ) != dev_set.end() )
        {
            filename_info_t fname;

            fname.filename = is_dir ? filename + "/" : filename;
            fname.inode = inode;
            fname.dev = dev;

            found_files.push_back( fname );
        }
    }
}

static bool is_dot_dir( const char *dname )
{
    if ( dname[ 0 ] == '.' )
    {
        if ( !dname[ 1 ] )
            return true;

        if ( ( dname[ 1 ] == '.' ) && !dname[ 2 ] )
            return true;
    }

    return false;
}

// Returns -1: queue empty, 0: open error, > 0 success
int thread_info_t::parse_dirqueue_entry()
{
    char __attribute__( ( aligned( 16 ) ) ) buf[ 1024 ];

    char *path = dequeue_directory();
    if ( !path )
    {
        return -1;
    }

    int fd = open( path, O_RDONLY | O_DIRECTORY, 0 );
    if ( fd < 0 )
    {
        free( path );
        return 0;
    }

    scanned_dirs++;

    size_t pathlen = strlen( path );

    for ( ;; )
    {
        int ret = sys_getdents64( fd, buf, sizeof( buf ) );
        if ( ret < 0 )
        {
            printf( "ERROR: sys_getdents64 failed on '%s': %d errno:%d\n", path, ret, errno );
            break;
        }
        if ( ret == 0 )
            break;

        for ( int bpos = 0; bpos < ret; )
        {
            struct linux_dirent64 *dirp = ( struct linux_dirent64 * )( buf + bpos );
            const char *d_name = dirp->d_name;

            // DT_BLK      This is a block device.
            // DT_CHR      This is a character device.
            // DT_FIFO     This is a named pipe (FIFO).
            // DT_SOCK     This is a UNIX domain socket.
            // DT_UNKNOWN  The file type could not be determined.

            // DT_REG      This is a regular file.
            // DT_LNK      This is a symbolic link.
            if ( dirp->d_type == DT_REG || dirp->d_type == DT_LNK )
            {
                add_filename( dirp->d_ino, path, d_name, false );
            }
            // DT_DIR      This is a directory.
            else if ( dirp->d_type == DT_DIR )
            {
                if ( !is_dot_dir( d_name ) )
                {
                    add_filename( dirp->d_ino, path, d_name, true );

                    size_t len = strlen( d_name );
                    char *newpath = ( char * )malloc( pathlen + len + 2 );

                    if ( newpath )
                    {
                        strcpy( newpath, path );
                        strcpy( newpath + pathlen, d_name );
                        newpath[ pathlen + len ] = '/';
                        newpath[ pathlen + len + 1 ] = 0;

                        queue_directory( newpath );
                    }
                }
            }

            bpos += dirp->d_reclen;
        }
    }

    close( fd );
    free( path );
    return 1;
}

static void *parse_dirqueue_threadproc( void *arg )
{
    thread_info_t *pthread_info = ( thread_info_t * )arg;

    for ( ;; )
    {
        // Loop until all the dequeue(s) fail
        if ( pthread_info->parse_dirqueue_entry() == -1 )
            break;
    }

    return nullptr;
}

static bool is_proc_in_cmdline_applist( const procinfo_t &procinfo, std::vector< std::string > &cmdline_applist )
{
    for ( const std::string &str : cmdline_applist )
    {
        // Check if our command line string is a subset of this appname
        if ( strstr( procinfo.appname.c_str(), str.c_str() ) )
            return true;

        // Check if the PIDs match
        if ( atoll( str.c_str() ) == procinfo.pid )
            return true;
    }

    return false;
}

static bool watch_count_is_greater ( procinfo_t elem1, procinfo_t elem2 )
{
   return elem1.watches > elem2.watches;
}


static bool init_inotify_proclist( std::vector< procinfo_t > &inotify_proclist )
{
    DIR *dir_proc = opendir( "/proc" );

    if ( !dir_proc )
    {
        printf( "ERROR: opendir /proc failed: %d\n", errno );
        return false;
    }

    for ( ;; )
    {
        struct dirent *dp_proc = readdir( dir_proc );
        if ( !dp_proc )
            break;

        if ( ( dp_proc->d_type == DT_DIR ) && isdigit( dp_proc->d_name[ 0 ] ) )
        {
            procinfo_t procinfo;

            procinfo.pid = atoll( dp_proc->d_name );

            std::string executable = string_format( "/proc/%d/exe", procinfo.pid );

            procinfo.executable = get_link_name( executable.c_str() );
            if ( !procinfo.executable.empty() )
            {
                procinfo.appname = basename( procinfo.executable.c_str() );

                inotify_parse_fddir( procinfo );

                if ( procinfo.watches )
                {
                    inotify_proclist.push_back( procinfo );
                }
            }
        }
    }
    std::sort(inotify_proclist.begin(), inotify_proclist.end(), watch_count_is_greater);

    closedir( dir_proc );
    return true;
}

static void print_inotify_proclist( std::vector< procinfo_t > &inotify_proclist )
{
    printf( "%s        Pid  App                        Watches   Instances%s\n", BCYAN, RESET );

    for ( procinfo_t &procinfo : inotify_proclist )
    {
        printf( "  % 10d %s%-30s%s %3u %3u\n", procinfo.pid,
            BYELLOW, procinfo.appname.c_str(), RESET,
            procinfo.watches, procinfo.instances );

        if ( g_verbose > 1 )
        {
            for ( std::string &fname : procinfo.fdset_filenames )
            {
                printf( "    %s%s%s\n", CYAN, fname.c_str(), RESET );
            }
        }

        if ( procinfo.in_cmd_line  )
        {
            for ( const auto &it1 : procinfo.dev_map )
            {
                dev_t dev = it1.first;

                printf( "%s[%u.%u]:%s", BGRAY, major( dev ), minor( dev ), RESET );
                for ( const auto &it2 : it1.second )
                {
                    std::string inode_device_str = string_format( "%lu", it2 );

                    printf( " %s%s%s", BGRAY, inode_device_str.c_str(), RESET );
                }
                printf( "\n" );
            }
        }
    }
}

bool thread_shared_data_t::init( uint32_t numthreads, const std::vector< procinfo_t > &inotify_proclist )
{
    for ( const procinfo_t &procinfo : inotify_proclist )
    {
        if ( !procinfo.in_cmd_line  )
            continue;

        for ( const auto &it1 : procinfo.dev_map )
        {
            dev_t dev = it1.first;

            for ( const auto &inode : it1.second )
            {
                inode_set[ inode ].insert( dev );
            }
        }
    }

    if ( !inode_set.empty() )
    {
        dirqueues.resize( numthreads );
    }

    return !inode_set.empty();
}

static uint32_t find_files_in_inode_set( const std::vector< procinfo_t > &inotify_proclist,
    std::vector< filename_info_t > &all_found_files )
{
    thread_shared_data_t tdata;

    g_numthreads = std::max< size_t >( 1, g_numthreads );

    if ( !tdata.init( g_numthreads, inotify_proclist ) )
        return 0;

    printf( "\n%sSearching '/' for listed inodes...%s (%lu threads)\n", BCYAN, RESET, g_numthreads );

    // Initialize thread_info_t array
    std::vector< class thread_info_t > thread_array( g_numthreads, thread_info_t( tdata ) );

    for ( uint32_t idx = 0; idx < thread_array.size(); idx++ )
    {
        thread_info_t &thread_info = thread_array[ idx ];

        thread_info.idx = idx;

        if ( idx == 0 )
        {
            // Add root dir in case someone is watching it
            thread_info.add_filename( mystatx( "/", STATX_INO ).stx_ino, "/", "", false );
            // Add and parse root
            thread_info.queue_directory( strdup( "/" ) );
            thread_info.parse_dirqueue_entry();
        }
        else if ( pthread_create( &thread_info.pthread_id, NULL, &parse_dirqueue_threadproc, &thread_info ) )
        {
            printf( "Warning: pthread_create failed. errno: %d\n", errno );
            thread_info.pthread_id = 0;
        }
    }

    // Put main thread to work
    parse_dirqueue_threadproc( &thread_array[ 0 ] );

    uint32_t total_scanned_dirs = 0;
    for ( const thread_info_t &thread_info : thread_array )
    {
        if ( thread_info.pthread_id )
        {
            if ( g_verbose > 1 )
            {
                printf( "Waiting for thread #%zu\n", thread_info.pthread_id );
            }

            void *status = NULL;
            int rc = pthread_join( thread_info.pthread_id, &status );

            if ( g_verbose > 1 )
            {
                printf( "Thread #%zu rc=%d status=%d\n", thread_info.pthread_id, rc, ( int )( intptr_t )status );
            }
        }

        // Snag data from this thread
        total_scanned_dirs += thread_info.scanned_dirs;

        all_found_files.insert( all_found_files.end(),
                thread_info.found_files.begin(), thread_info.found_files.end() );

        if ( g_verbose > 1 )
        {
            printf( "Thread #%zu: %u dirs, %zu files found\n",
                    thread_info.pthread_id, thread_info.scanned_dirs, thread_info.found_files.size() );
        }
    }

    struct
    {
        bool operator()( const filename_info_t &a, const filename_info_t &b ) const
        {
            if ( a.dev == b.dev )
                return a.inode < b.inode;
            return a.dev < b.dev;
        }
    } filename_info_less_func;

    std::sort( all_found_files.begin(), all_found_files.end(), filename_info_less_func );

    return total_scanned_dirs;
}

static uint32_t get_inotify_procfs_value( const std::string &fname )
{
    char buf[ 64 ];
    uint32_t val = 0;
    std::string filename = "/proc/sys/fs/inotify/" + fname;

    int fd = open( filename.c_str(), O_RDONLY );
    if ( fd >= 0 )
    {
        if ( read( fd, buf, sizeof( buf ) ) > 0 )
        {
            val = strtoul( buf, nullptr, 10 );
        }

        close( fd );
    }

    return val;
}

static void print_inotify_limits()
{
    const std::vector< std::string > filenames =
    {
        "max_queued_events",
        "max_user_instances",
        "max_user_watches"
    };

    printf( "%sINotify Limits:%s\n", BCYAN, RESET );
    for ( const std::string &fname : filenames )
    {
        uint32_t val = get_inotify_procfs_value( fname );

        printf( "  %-20s %s%u%s\n", fname.c_str(), BGREEN, val, RESET );
    }
}

static void print_usage( const char *appname )
{
    printf( "Usage: %s [--threads=##] [appname | pid...]\n", appname );
    printf( "    [-vv]\n" );
    printf( "    [-?|-h|--help]\n" );

    exit( -1 );
}

static void parse_cmdline( int argc, char **argv, std::vector< std::string > &cmdline_applist )
{
    static struct option long_opts[] =
    {
        { "verbose", no_argument, 0, 0 },
        { "threads", required_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    // Let's pick the number of processors online (with a max of 32) for a default.
    g_numthreads = std::min< uint32_t >( g_numthreads, sysconf( _SC_NPROCESSORS_ONLN ) );

    int c;
    int opt_ind = 0;
    while ( ( c = getopt_long( argc, argv, "m:s:?hv", long_opts, &opt_ind ) ) != -1 )
    {
        switch ( c )
        {
        case 0:
            if ( !strcasecmp( "verbose", long_opts[ opt_ind ].name ) )
                g_verbose++;
            else if ( !strcasecmp( "threads", long_opts[ opt_ind ].name ) )
                g_numthreads = atoi( optarg );
            break;
        case 'v':
            g_verbose++;
            break;
        case 'h':
        case '?':
        default:
            print_usage( argv[ 0 ] );
            break;
        }
    }

    for ( ; optind < argc; optind++ )
    {
        cmdline_applist.push_back( argv[ optind ] );
    }
}

static void print_separator()
{
    printf( "%s%s%s\n", YELLOW, std::string( 78, '-' ).c_str(), RESET );
}

int main( int argc, char *argv[] )
{
    std::vector< std::string > cmdline_applist;
    std::vector< procinfo_t > inotify_proclist;

    parse_cmdline( argc, argv, cmdline_applist );
    print_separator();

    print_inotify_limits();
    print_separator();

    if ( init_inotify_proclist( inotify_proclist ) )
    {
        uint32_t total_watches = 0;
        uint32_t total_instances = 0;
        std::vector< filename_info_t > all_found_files;

        for ( procinfo_t &procinfo : inotify_proclist )
        {
            procinfo.in_cmd_line = is_proc_in_cmdline_applist( procinfo, cmdline_applist );

            total_watches += procinfo.watches;
            total_instances += procinfo.instances;
        }

        print_inotify_proclist( inotify_proclist );
        print_separator();

        printf( "Total inotify Watches:   %s%u%s\n", BGREEN, total_watches, RESET );
        printf( "Total inotify Instances: %s%u%s\n", BGREEN, total_instances, RESET );
        print_separator();

        double search_time = gettime();
        uint32_t total_scanned_dirs = find_files_in_inode_set( inotify_proclist, all_found_files );
        if ( total_scanned_dirs )
        {
            search_time = gettime() - search_time;

            for ( const filename_info_t &fname_info : all_found_files )
            {
                printf( "%s%9lu%s [%u:%u] %s\n", BGREEN, fname_info.inode, RESET,
                        major( fname_info.dev ), minor( fname_info.dev ),
                        fname_info.filename.c_str() );
            }

            setlocale( LC_NUMERIC, "" );
GCC_DIAG_PUSH_OFF( format )
            printf( "\n%'u dirs scanned (%.2f seconds)\n", total_scanned_dirs, search_time );
GCC_DIAG_POP()
        }
    }

    return 0;
}
