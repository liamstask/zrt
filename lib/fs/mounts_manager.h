/*
 * mounts_manager.h
 *
 *  Created on: 19.09.2012
 *      Author: yaroslav
 */

#ifndef MOUNTS_MANAGER_H_
#define MOUNTS_MANAGER_H_

#include <linux/limits.h>

struct MountsInterface;

struct MountInfo{
    char mount_path[PATH_MAX]; /*for example "/", "/dev" */
    struct MountsInterface* mount;
};


struct MountsManager{
    int (*mount_add)( const char* path, struct MountsInterface* filesystem_mount );
    int (*mount_remove)( const char* path );

    struct MountInfo* (*mountinfo_bypath)(const char* path);
    struct MountsInterface* (*mount_bypath)( const char* path );
    struct MountsInterface* (*mount_byhandle)( int handle );

    const char* (*convert_path_to_mount)(const char* full_path);

    struct HandleAllocator* handle_allocator;
};


struct MountsManager* get_mounts_manager();
struct MountsManager* mounts_manager(); /*accessor*/


#endif /* MOUNTS_MANAGER_H_ */
