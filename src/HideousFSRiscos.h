#ifndef HIDEOUSFS_RISCOS_H
#define HIDEOUSFS_RISCOS_H

#include "HideousFS.h"

enum {
    HideousFS_FileType = 0x001,

    OS_File = 0x08,
    OS_Args = 0x09,
    OS_BPut = 0x0b,
    OS_GBPB = 0x0c,
    OS_Find = 0x0d,
    OS_FSControl = 0x29,

    OSFind_Close = 0,
    OSFind_ReadFileNoPath = 0x4f,
    OSFind_WriteFileNoPath = 0x87,
    OSFind_UpdateFileNoPath = 0xcf,

    OSGBPB_WriteAt = 1,
    OSGBPB_ReadAt = 3,
    OSGBPB_DirEntries = 9,
    OSGBPB_DirEntriesInfo = 10,

    OSArgs_SetExt = 3,
    OSArgs_SetAllocation = 6,
    OSArgs_ReadExt = 2,
    OSArgs_ReadPath = 7,

    FSControl_RegisterImageFS = 0x23,
    FSControl_DeregisterImageFS = 0x24,
    FSControl_Rename = 0x19,

    fsfile_Save = 0,
    fsfile_WriteInfo = 1,
    fsfile_WriteLoad = 2,
    fsfile_WriteExec = 3,
    fsfile_WriteAttr = 4,
    fsfile_ReadInfo = 5,
    fsfile_Delete = 6,
    fsfile_Create = 7,
    fsfile_CreateDir = 8,
    fsfile_ReadBlockSize = 10,
    fsfile_WriteFileType = 18,

    fsopen_ReadOnly = 0,
    fsopen_CreateUpdate = 1,
    fsopen_Update = 2,

    fsargs_ReadPTR = 0,
    fsargs_SetPTR = 1,
    fsargs_SetEXT = 3,
    fsargs_ReadEXT = 2,
    fsargs_ReadAllocation = 4,
    fsargs_ReadEOFStatus = 5,
    fsargs_Flush = 6,
    fsargs_EnsureSize = 7,
    fsargs_ReadLoadExec = 9,

    fsfunc_ReadDirEntries = 14,
    fsfunc_ReadDirEntriesInfo = 15,
    fsfunc_Rename = 8,
    fsfunc_NewImage = 21,
    fsfunc_ImageClosing = 22,
    fsfunc_ReadBootOption = 27,
    fsfunc_WriteBootOption = 28,

    object_file = 1,
    object_directory = 2,

    fsopen_ReadPermission = 1u << 30,
    fsopen_IsDirectory = 1u << 29,

    file_attr_owner_read = 1u << 0,
    file_attr_owner_write = 1u << 1,
    file_attr_public_read = 1u << 4,
    file_attr_public_write = 1u << 5
};

#define fsopen_WritePermission ((word)1u << 31)

typedef struct FS_cat_entry {
    word type;
    word loadaddr;
    word execaddr;
    word filelen;
    word fileattr;
} FS_cat_entry;

typedef struct FS_open_block {
    word information;
    void *inhand;
    word buffsize;
    word fileext;
    word falloc;
} FS_open_block;

typedef struct FS_datestamp {
    word loadaddr;
    word execaddr;
} FS_datestamp;

typedef struct FS_dir_block {
    word objects_read;
    int next_offset;
} FS_dir_block;

typedef struct FS_entry_info {
    word loadaddr;
    word execaddr;
    word flength;
    word attributes;
    word type;
    char fname[1];
} FS_entry_info;

typedef struct HideousFS_File {
    int in_use;
    int is_directory;
    int writable;
    word os_handle;
    word ptr;
    word loadaddr;
    word execaddr;
    word extent;
    word attr;
    char backing_path[MaxPath];
} HideousFS_File;

#endif
