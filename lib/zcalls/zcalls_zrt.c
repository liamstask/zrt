/*
 * syscallbacks.c
 * Syscallbacks implementation used by zrt;
 *
 *  Created on: 6.07.2012
 *      Author: YaroslavLitvinov
 */

#define _GNU_SOURCE

#include <time.h>
#include <sys/time.h>
#include <sys/types.h> //off_t
#include <sys/stat.h>
#include <string.h> //memcpy
#include <stdio.h>
#include <stdlib.h> //atoi
#include <stddef.h> //size_t
#include <stdarg.h>
#include <unistd.h> //STDIN_FILENO
#include <fcntl.h> //file flags, O_ACCMODE
#include <errno.h>
#include <dirent.h>     /* Defines DT_* constants */
#include <assert.h>

#include "zvm.h"
#include "zrt.h"
#include "zrt_defines.h"
#include "zcalls.h"
#include "zcalls_zrt.h"
#include "memory_syscall_handlers.h"
#include "zrtlog.h"
#include "zrt_helper_macros.h"
#include "transparent_mount.h"
#include "stream_reader.h"
#include "settime_observer.h"
#include "path_utils.h"             /*alloc_absolute_path_from_relative*/
#include "environment_observer.h"
#include "fstab_observer.h"
#include "mapping_observer.h"
#include "nvram_loader.h"
#include "mounts_manager.h"
#include "mem_mount_wraper.h"
#include "nacl_struct.h"
#include "image_engine.h"
#include "enum_strings.h"
#include "channels_reserved.h"
#include "channels_mount.h"

extern char **environ;

/****************** static data*/
static struct NvramLoader      s_nvram;
struct timeval                 s_cached_timeval;
struct MountsInterface*        s_channels_mount=NULL;
struct MountsInterface*        s_mem_mount=NULL;
static struct MountsManager*   s_mounts_manager = NULL;
static struct MountsInterface* s_transparent_mount = NULL;
static struct MemoryInterface* s_memory_interface = NULL;
static int                     s_zrt_ready=0;
/****************** */

struct NvramLoader*     static_nvram()      { return &s_nvram; }
struct MountsInterface* transparent_mount() { return s_transparent_mount; }


/***********************************************************
 *ZRT initializators
 ***********************************************************/

/*first step zrt initializer*/
static void zrt_init( const struct UserManifest const* manifest ){
    /*manage mounted filesystems*/
    s_mounts_manager = get_mounts_manager();

    /*alloc filesystem based on channels*/
    s_channels_mount = alloc_channels_mount( s_mounts_manager->handle_allocator,
					     manifest->channels, manifest->channels_count );
    /*alloc main filesystem that combines all filesystems mounts*/
    s_transparent_mount = alloc_transparent_mount( s_mounts_manager );

    s_zrt_ready = 1;

    /*open standard files to conform C*/
    s_channels_mount->open( DEV_STDIN, O_RDONLY, 0 );
    s_channels_mount->open( DEV_STDOUT, O_WRONLY, 0 );
    s_channels_mount->open( DEV_STDERR, O_WRONLY, 0 );

    /*create mem mount*/
    s_mem_mount = alloc_mem_mount( s_mounts_manager->handle_allocator );

    /*Mount filesystems*/
    s_mounts_manager->mount_add( "/dev", s_channels_mount );
    s_mounts_manager->mount_add( "/", s_mem_mount );

    /*explicitly create /dev directory in memmount, it's required for consistent
      FS structure, readdir from now can list /dev dir recursively from root */
    s_mem_mount->mkdir( "/dev", 0777 );

#define HANDLE_ONLY_TIME_SECTION get_settime_observer()
    /*nvram must be already parsed*/
    if ( NULL != s_nvram.section_by_name( &s_nvram, TIME_SECTION_NAME ) ){
	s_nvram.handle(&s_nvram, HANDLE_ONLY_TIME_SECTION, &s_cached_timeval, NULL, NULL);
    }
    /*user main execution just after zrt initialization*/
}

/*second step zrt initializer*/
static void zrt_setup_finally(){
#define HANDLE_ONLY_MAPPING_SECTION get_mapping_observer()
    /*nvram must be already parsed*/
    if ( NULL != s_nvram.section_by_name( &s_nvram, MAPPING_SECTION_NAME ) ){
	s_nvram.handle(&s_nvram, HANDLE_ONLY_MAPPING_SECTION, NULL, NULL, NULL);
    }

#define HANDLE_ONLY_FSTAB_SECTION get_fstab_observer()
    /*nvram must be already parsed*/
    if ( NULL != s_nvram.section_by_name( &s_nvram, FSTAB_SECTION_NAME ) ){
	s_nvram.handle(&s_nvram, HANDLE_ONLY_FSTAB_SECTION, 
		       s_channels_mount, s_transparent_mount, NULL);
    }

    ZRT_LOG(L_INFO, P_TEXT, "zrt startup finished");
    ZRT_LOG_DELIMETER;
}

static inline void update_cached_time()
{
    /* update time value
     * update seconds because updating miliseconds has no effect*/
    ++s_cached_timeval.tv_sec;
}


/********************************************************************************
 * ZRT IMPLEMENTATION OF NACL SYSCALLS
 * each nacl syscall must be implemented or, at least, mocked. no exclusions!
 *********************************************************************************/

/* irt basic *************************/

/*
 * exit. without it the user program cannot terminate correctly.
 */
void zrt_zcall_enhanced_exit(int status){
    ZRT_LOG(L_SHORT, "status %d exiting...", status);
    handle_tar_export();
    zvm_exit(status); /*get controls into zerovm*/
    /* unreachable code*/
    return; 
}

int  zrt_zcall_enhanced_gettod(struct timeval *tvl){
    int ret=-1;
    errno=0;

    if(tvl == NULL) {
        errno = EFAULT;
    }
    else{
        /*retrieve and get cached time value*/
        tvl->tv_usec = s_cached_timeval.tv_usec;
        tvl->tv_sec  = s_cached_timeval.tv_sec;
        ZRT_LOG(L_INFO, "tv_sec=%lld, tv_usec=%d", tvl->tv_sec, tvl->tv_usec );

        /* update time value*/
        update_cached_time();
	ret=0;
    }

    return ret;
}
/* irt fdio *************************/
int  zrt_zcall_enhanced_close(int handle){
    LOG_SYSCALL_START("handle=%d", handle);
    errno = 0;

    int ret = s_transparent_mount->close(handle);
    LOG_SHORT_SYSCALL_FINISH( ret, "handle=%d", handle);
    return ret;
}
int  zrt_zcall_enhanced_dup(int fd, int *newfd){
    SET_ERRNO(ENOSYS);
    return -1;
}
int  zrt_zcall_enhanced_dup2(int fd, int newfd){
    SET_ERRNO(ENOSYS);
    return -1;
}

int  zrt_zcall_enhanced_read(int handle, void *buf, size_t count, size_t *nread){
    int ret=-1;
    LOG_SYSCALL_START("handle=%d buf=%p count=%u", handle, buf, count);
    errno = 0;
    VALIDATE_SYSCALL_PTR(buf);

    int32_t bytes_read = s_transparent_mount->read(handle, buf, count);
    if ( bytes_read >= 0 ){
	/*get read bytes by pointer*/
	*nread = bytes_read;
	ret = 0;
    }
    LOG_INFO_SYSCALL_FINISH( ret, "bytes_read=%d, handle=%d", bytes_read, handle);
    return ret;
}

int  zrt_zcall_enhanced_write(int handle, const void *buf, size_t count, size_t *nwrote){
    int ret=-1;
    int log_state = __zrt_log_is_enabled();
    /*disable logging while writing to stdout and stderr channel */
    if ( handle <= 2 && log_state ){
	__zrt_log_enable(0);
    }

    LOG_SYSCALL_START("handle=%d buf=%p count=%u", handle, buf, count);
    VALIDATE_SYSCALL_PTR(buf);

    int32_t bytes_wrote = s_transparent_mount->write(handle, buf, count);
    if ( bytes_wrote >= 0 ){
	/*get wrote bytes by pointer*/
	*nwrote = bytes_wrote;
	ret=0;
    }
    LOG_INFO_SYSCALL_FINISH( ret, "bytes_wrote=%d, handle=%d count=%u", 
			      bytes_wrote, handle, count);
    if ( log_state )
	__zrt_log_enable(log_state); /*restore log state*/
    return ret;
}

int  zrt_zcall_enhanced_seek(int handle, off_t offset, int whence, off_t *new_offset){
    int ret=-1;
    LOG_SYSCALL_START("handle=%d offset=%lld whence=%d", handle, offset, whence);
    errno = 0;

    if ( whence == SEEK_SET && offset < 0 ){
	SET_ERRNO(EINVAL);
    }
    else{
	offset = s_transparent_mount->lseek(handle, offset, whence);
	if ( offset != -1 ){
	    /*get new offset by pointer*/
	    *new_offset = offset;
	    ret=0;
	}
    }

    LOG_SHORT_SYSCALL_FINISH( ret, "newoffset=%lld, handle=%d whence=%s", 
			      offset, handle, STR_SEEK_WHENCE(whence));
    return ret;
}

int  zrt_zcall_enhanced_fstat(int handle, struct stat *st){
    LOG_SYSCALL_START("handle=%d stat=%p", handle, st);
    errno = 0;
    VALIDATE_SYSCALL_PTR(stat);

    int ret = s_transparent_mount->fstat( handle, st);
    if ( ret == 0 ){
	ZRT_LOG_STAT(L_INFO, st);
    }
    LOG_SHORT_SYSCALL_FINISH( ret, "handle=%d", handle);
    return ret;
}

int  zrt_zcall_enhanced_getdents(int fd, struct dirent *dirent_buf, size_t count, size_t *nread){
    int ret = -1;
    LOG_SYSCALL_START("fd=%d dirent_buf=%p count=%u", fd, dirent_buf, count);
    errno=0;
    VALIDATE_SYSCALL_PTR(dirent_buf);

    int32_t bytes_readed = s_transparent_mount->getdents(fd, (char*)dirent_buf, count);
    if ( bytes_readed >= 0 ){
	*nread = bytes_readed;
	ret=0;
    }
    LOG_SHORT_SYSCALL_FINISH( ret, "fd=%d count=%u", fd, count);
    return ret;
}

int  zrt_zcall_enhanced_open(const char *name, int flags, mode_t mode, int *newfd){
    int ret=-1;
    LOG_SYSCALL_START("name=%s flags=%d mode=%o(octal)", name, flags, mode );
    errno=0;
    VALIDATE_SYSCALL_PTR(name);
    
    /*reset mode bits, that is not actual for permissions*/
    mode&=(S_IRWXU|S_IRWXG|S_IRWXO);

    char* absolute_path = alloc_absolute_path_from_relative( name );
    APPLY_UMASK(&mode);
    int fd = s_transparent_mount->open( absolute_path, flags, mode );
    free(absolute_path);
    /*get fd by pointer*/
    if ( fd >= 0 ){
	*newfd  = fd;
	ret =0;
    }
    LOG_SHORT_SYSCALL_FINISH( ret, 
			      "newfd=%d, name=%s, flags=%s, mode=%s", 
			      fd, name, 
			      STR_ALLOCA_COPY(STR_FILE_OPEN_FLAGS(flags)),
			      STR_ALLOCA_COPY(STR_STAT_ST_MODE(mode)));
    return ret;
}

int  zrt_zcall_enhanced_stat(const char *pathname, struct stat * stat){
    LOG_SYSCALL_START("pathname=%s stat=%p", pathname, stat);
    errno = 0;
    VALIDATE_SYSCALL_PTR(pathname);
    VALIDATE_SYSCALL_PTR(stat);

    char* absolute_path = alloc_absolute_path_from_relative(pathname);
    int ret = s_transparent_mount->stat(absolute_path, stat);
    free(absolute_path);
    if ( ret == 0 ){
	ZRT_LOG_STAT(L_INFO, stat);
    }
    LOG_SHORT_SYSCALL_FINISH( ret, "pathname=%s", pathname);
    return ret;
}

/*
 * following 3 functions (sysbrk, mmap, munmap) is the part of the
 * new memory engine. the new allocate specified amount of ram before
 * user code start and then user can only obtain that allocated memory.
 * zrt lib will help user to do it transparently.
 */

/* irt memory *************************/
int  zrt_zcall_enhanced_sysbrk(void **newbrk){
    int ret=-1;
    LOG_SYSCALL_START("*newbrk=%p", *newbrk);
    int32_t retaddr = s_memory_interface->sysbrk(s_memory_interface, *newbrk );
    if ( retaddr != -1 ){
	/*get new address via pointer*/
	*newbrk = (void*)retaddr;
	ret=0;
    }
    LOG_INFO_SYSCALL_FINISH( retaddr, "*newbrk=%p", *newbrk);
    return ret;
}

int  zrt_zcall_enhanced_mmap(void **addr, size_t length, int prot, int flags, int fd, off_t off){
    int32_t retcode = -1;
    LOG_SYSCALL_START("addr=%p length=%u prot=%u flags=%u fd=%u off=%lld",
    		      *addr, length, prot, flags, fd, off);

    retcode = s_memory_interface->mmap(s_memory_interface, *addr, length, prot,
    		  flags, fd, off);
    if ( retcode > 0 ){
	*addr = (void*)retcode;
	retcode=0;
    }
  
    LOG_INFO_SYSCALL_FINISH( retcode,
    		       "addr=%p length=%u prot=%s flags=%s fd=%u off=%lld",
			     *addr, length, 
			     STR_ALLOCA_COPY(STR_MMAP_PROT_FLAGS(prot)), 
			     STR_ALLOCA_COPY(STR_MMAP_FLAGS(flags)),
    		       fd, off);
    return retcode;
}

int  zrt_zcall_enhanced_munmap(void *addr, size_t len){
    LOG_SYSCALL_START("addr=%p, len=%u", addr, len);
    int32_t retcode = s_memory_interface->munmap(s_memory_interface, addr, len);
    LOG_INFO_SYSCALL_FINISH( retcode, "addr=%p, len=%u", addr, len);
    return retcode;
}

/************************************************************************
* zcalls_zrt_t implementation
* Setup zrt
*************************************************************************/
void zrt_zcall_enhanced_zrt_setup(void){
    int i;
    char **envp = environ;
    const struct UserManifest const *setup = MANIFEST;
    s_memory_interface = get_memory_interface( setup->heap_ptr, setup->heap_size );

    zrt_init( setup );

    /* debug print */
    ZRT_LOG(L_BASE, P_TEXT, "DEBUG INFORMATION");
    time_t t = time(NULL);
    ZRT_LOG(L_BASE, "Time %s", ctime(&t) );
    ZRT_LOG(L_BASE, "user heap pointer address = 0x%x", (intptr_t)setup->heap_ptr);
    ZRT_LOG(L_BASE, "user memory size = %u", setup->heap_size);
    ZRT_LOG_DELIMETER;

    /*print environment variables*/
    i=0;
    while( envp[i] ){
        ZRT_LOG(L_BASE, "envp[%d] = '%s'", i, envp[i]);
	++i;
    }

    ZRT_LOG(L_BASE, "channels count = %d", setup->channels_count);
    ZRT_LOG_DELIMETER;
    /*print channels list*/
    for(i = 0; i < setup->channels_count; ++i)
    {
        ZRT_LOG(L_BASE, "channel[%2d].name = '%s'", i, setup->channels[i].name);
        ZRT_LOG(L_BASE, "channel[%2d].type=%d, size=%lld", i, 
		setup->channels[i].type, setup->channels[i].size);
        ZRT_LOG(L_BASE, "channel[%2d].limits[GetsLimit=%7lld, GetSizeLimit=%7lld]", i, 
		setup->channels[i].limits[GetsLimit], 
		setup->channels[i].limits[GetSizeLimit]);
        ZRT_LOG(L_BASE, "channel[%2d].limits[PutsLimit=%7lld, PutSizeLimit=%7lld]", i, 
		setup->channels[i].limits[PutsLimit],
		setup->channels[i].limits[PutSizeLimit]);
    }
    ZRT_LOG_DELIMETER;
    ZRT_LOG(L_SHORT, "_SC_PAGE_SIZE=%ld", sysconf(_SC_PAGE_SIZE));

    zrt_setup_finally();
}

int is_zrt_ready(){
    return s_zrt_ready;
}


/*************************************************************************/



