/*
 * handle_allocator.c
 *
 *  Created on: 20.09.2012
 *      Author: yaroslav
 */

#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "zrtlog.h"
#include "handle_allocator.h"

#define CHECK_HANDLE(handle){						\
	if ( handle < 0 ){						\
	    /*bad handle*/						\
	    return -1;							\
	}								\
	else if ( handle >= MAX_HANDLES_COUNT ){			\
	    ZRT_LOG(L_ERROR, "MAX_HANDLES_COUNT exceed %d", handle);	\
	    return -1;							\
	}								\
    }


enum { EHandleAvailable=0, EHandleUsed=1, EHandleReserved=2  };
struct HandleItem{
    int used; /*if EHandleReserved is set, it can't be reallocated to another mount*/
    ino_t inode;
    off_t offset; /*used by read, write functions*/
    struct MountsInterface* mount_fs;
};


static int s_first_unused_slot = 0;
static struct HandleItem s_handle_slots[MAX_HANDLES_COUNT];

int seek_unused_slot( int starting_from ){
    int i;
    starting_from = starting_from<0 ?0 :starting_from;
    for ( i=starting_from; i < MAX_HANDLES_COUNT; i++ ){
        if ( s_handle_slots[i].used == EHandleAvailable ){
	    /*found closest unusable slot*/
            ZRT_LOG( L_INFO, "slot index=%d, EHandleAvailable", i );
            s_first_unused_slot = i;
            return s_first_unused_slot;
        }
    }
    return -1;
}

static struct MountsInterface* mount_interface(int handle){
    /*if handle invalid or can't be a valid*/
    if ( handle < 0 || handle >= MAX_HANDLES_COUNT ) return NULL;
    return s_handle_slots[handle].mount_fs;
}

static int get_inode(int handle, ino_t* inode ){
    CHECK_HANDLE(handle);
    ZRT_LOG( L_EXTRA, "handle=%d, inode=%d, inode pointer=%p",
	     handle, (int)s_handle_slots[handle].inode, &(s_handle_slots[handle]).inode );
    *inode = s_handle_slots[handle].inode;
    return 0;
}

static int set_inode(int handle, ino_t inode ){
    CHECK_HANDLE(handle);
    ZRT_LOG( L_EXTRA, "inode=%d", (int)inode );
    s_handle_slots[handle].inode = inode;
    return 0;
}

static int get_offset(int handle, off_t* offset ){
    CHECK_HANDLE(handle);
    *offset = s_handle_slots[handle].offset;
    return 0;
}

static int set_offset(int handle, off_t newoffset ){
    CHECK_HANDLE(handle);
    off_t oldoffset = s_handle_slots[handle].offset;
    ZRT_LOG(L_INFO, P_INT, handle);
    ZRT_LOG(L_INFO, P_LONGINT, oldoffset);
    ZRT_LOG(L_INFO, P_LONGINT, newoffset);
    s_handle_slots[handle].offset = newoffset;
    return 0;
}

static int allocate_handle(struct MountsInterface* mount_fs){
    s_first_unused_slot = seek_unused_slot( s_first_unused_slot );
    s_handle_slots[s_first_unused_slot].used = EHandleUsed;
    s_handle_slots[s_first_unused_slot].mount_fs = mount_fs;
    return s_first_unused_slot;
}

static int allocate_reserved_handle( struct MountsInterface* mount_fs, int handle ){
    CHECK_HANDLE(handle);
    s_handle_slots[handle].used = EHandleReserved;
    s_handle_slots[handle].mount_fs = mount_fs;
    return handle;
}

static int free_handle(int handle){
    CHECK_HANDLE(handle);
    if ( s_handle_slots[handle].used == EHandleReserved ){
        return 0; //ok
    }
    else{
        s_handle_slots[handle].offset = 0;
        s_handle_slots[handle].inode = 0;
        s_handle_slots[handle].used = EHandleAvailable;
        s_handle_slots[handle].mount_fs = NULL;
        /*set lowest available slot*/
        if ( handle < s_first_unused_slot ) s_first_unused_slot = handle;
        return 0; //ok
    }
}


static struct HandleAllocator s_handle_allocator = {
    allocate_handle,
    allocate_reserved_handle,
    free_handle,
    mount_interface,
    get_inode,
    set_inode,
    get_offset,
    set_offset
};


struct HandleAllocator* get_handle_allocator(){
    return &s_handle_allocator;
}

