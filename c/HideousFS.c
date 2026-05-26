#include <stddef.h>
#include <string.h>

#include "kernel.h"

#include "cmhg.h"

typedef unsigned int word;

enum {
    HideousFS_FileType = 0x001,

    OS_File = 0x08,
    OS_Args = 0x09,
    OS_GBPB = 0x0c,
    OS_Find = 0x0d,
    OS_BPut = 0x0b,
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

    fsopen_ReadOnly = 0,
    fsopen_CreateUpdate = 1,
    fsopen_Update = 2,

    fsargs_SetEXT = 3,
    fsargs_ReadEXT = 2,
    fsargs_ReadSize = 4,
    fsargs_Flush = 6,
    fsargs_EnsureSize = 7,
    fsargs_ReadLoadExec = 9,

    fsfunc_ReadDirEntries = 14,
    fsfunc_ReadDirEntriesInfo = 15,
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
    file_attr_public_write = 1u << 5,

    MaxPath = 768,
    TempBufferSize = 1024,
    MaxImages = 8,
    MaxOpenFiles = 8
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

typedef struct HideousFS_Image {
    int in_use;
    word fileswitch_handle;
    word buffer_size;
    char image_path[MaxPath];
    char backing_dir[MaxPath];
    char image_leaf[MaxPath];
} HideousFS_Image;

typedef struct HideousFS_File {
    int in_use;
    int is_directory;
    int writable;
    word os_handle;
    word loadaddr;
    word execaddr;
    word extent;
    word attr;
    char backing_path[MaxPath];
} HideousFS_File;

extern char __module_header[];

static FS_cat_entry cat_entry;
static FS_datestamp datestamp;
static FS_open_block open_block;
static FS_dir_block dir_block;
static HideousFS_Image images[MaxImages];
static HideousFS_File open_files[MaxOpenFiles];
static char temp_buffer[TempBufferSize];
static char path_buffer[MaxPath];

static const _kernel_oserror err_buffer_too_small = {
    0x808002, "HideousFS: buffer too small"
};

static const _kernel_oserror err_not_found = {
    0x808003, "HideousFS: object not found"
};

static const _kernel_oserror err_path_too_long = {
    0x808004, "HideousFS: path too long"
};

static const _kernel_oserror err_no_file_handles = {
    0x808005, "HideousFS: no file handles available"
};

static const _kernel_oserror err_no_image_handles = {
    0x808006, "HideousFS: no image handles available"
};

static _kernel_oserror dynamic_error;

static const char *default_reverse_extensions[] = {
    "c", "h", "s", "o", "a", "cpp", "c++"
};

static _kernel_oserror *operation_not_implemented(const char *entry, int op)
{
    static const char prefix[] = "HideousFS: ";
    static const char middle[] = " op ";
    static const char suffix[] = " not implemented";
    char *out = dynamic_error.errmess;
    unsigned int value;
    char digits[12];
    int digit_count = 0;
    int i;

    dynamic_error.errnum = 0x808001;

    strcpy(out, prefix);
    out += strlen(out);
    strcpy(out, entry);
    out += strlen(out);
    strcpy(out, middle);
    out += strlen(out);

    if (op < 0) {
        *out++ = '-';
        value = (unsigned int)(-op);
    } else {
        value = (unsigned int)op;
    }

    do {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    for (i = digit_count - 1; i >= 0; --i) {
        *out++ = digits[i];
    }

    strcpy(out, suffix);
    return &dynamic_error;
}

static word word_align(word value)
{
    return (value + 3u) & ~3u;
}

static int reverse_extension_count(void)
{
    return (int)(sizeof(default_reverse_extensions) /
                 sizeof(default_reverse_extensions[0]));
}

static int mapped_extension_index(const char *name, size_t len)
{
    int i;

    for (i = 0; i < reverse_extension_count(); ++i) {
        if (strlen(default_reverse_extensions[i]) == len &&
            strncmp(default_reverse_extensions[i], name, len) == 0) {
            return i;
        }
    }

    return -1;
}

static int is_mapped_extension(const char *name)
{
    return mapped_extension_index(name, strlen(name)) >= 0;
}

static int is_root_name(const char *name)
{
    return name == NULL || name[0] == '\0' ||
           (name[0] == '$' && name[1] == '\0');
}

static int copy_string(char *dest, size_t dest_size, const char *src)
{
    size_t len = strlen(src);

    if (len >= dest_size) {
        return 0;
    }

    memcpy(dest, src, len + 1);
    return 1;
}

static const char *skip_root_prefix(const char *name)
{
    if (name != NULL && name[0] == '$' && name[1] == '.') {
        return name + 2;
    }

    return name;
}

static int append_relative_to_backing(const HideousFS_Image *image,
                                      const char *relative,
                                      char *dest, size_t dest_size)
{
    size_t dir_len;
    size_t relative_len;

    dir_len = strlen(image->backing_dir);
    relative_len = strlen(relative);

    if (relative_len == 0) {
        return copy_string(dest, dest_size, image->backing_dir);
    }

    if (dir_len + 1 + relative_len >= dest_size) {
        return 0;
    }

    memcpy(dest, image->backing_dir, dir_len);
    dest[dir_len] = '.';
    memcpy(dest + dir_len + 1, relative, relative_len + 1);
    return 1;
}

static const char *last_component(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot == NULL ? name : dot + 1;
}

static const char *previous_component(const char *name, const char *component)
{
    const char *scan = component;

    if (component == name) {
        return NULL;
    }

    --scan;
    while (scan > name && scan[-1] != '.') {
        --scan;
    }

    return scan;
}

static int build_backing_object_path(const HideousFS_Image *image,
                                     const char *name,
                                     char *dest, size_t dest_size)
{
    const char *relative;
    const char *leaf;
    const char *extension;
    const char *prefix_end;
    size_t prefix_len;
    size_t leaf_len;
    size_t extension_len;
    size_t dir_len;

    if (is_root_name(name)) {
        return copy_string(dest, dest_size, image->backing_dir);
    }

    relative = skip_root_prefix(name);
    leaf = last_component(relative);
    extension = previous_component(relative, leaf);

    if (extension == NULL) {
        return append_relative_to_backing(image, relative, dest, dest_size);
    }

    prefix_end = extension == relative ? extension : extension - 1;
    extension_len = (size_t)((leaf - 1) - extension);
    if (mapped_extension_index(extension, extension_len) < 0) {
        return append_relative_to_backing(image, relative, dest, dest_size);
    }

    dir_len = strlen(image->backing_dir);
    prefix_len = (size_t)(prefix_end - relative);
    leaf_len = strlen(leaf);

    if (dir_len + 1 + prefix_len + (prefix_len == 0 ? 0 : 1) +
        leaf_len + 1 + extension_len >= dest_size) {
        return 0;
    }

    memcpy(dest, image->backing_dir, dir_len);
    dest[dir_len++] = '.';
    if (prefix_len != 0) {
        memcpy(dest + dir_len, relative, prefix_len);
        dir_len += prefix_len;
        dest[dir_len++] = '.';
    }
    memcpy(dest + dir_len, leaf, leaf_len);
    dir_len += leaf_len;
    dest[dir_len++] = '/';
    memcpy(dest + dir_len, extension, extension_len);
    dest[dir_len + extension_len] = '\0';
    return 1;
}

static int resolve_directory_path(const HideousFS_Image *image,
                                  const char *name,
                                  char *dest, size_t dest_size,
                                  char *synthetic_extension,
                                  size_t synthetic_extension_size)
{
    char prefix[MaxPath];
    const char *relative;
    const char *component;
    const char *prefix_end;
    size_t extension_len;

    synthetic_extension[0] = '\0';

    if (is_root_name(name)) {
        return copy_string(dest, dest_size, image->backing_dir);
    }

    relative = skip_root_prefix(name);
    component = last_component(relative);
    extension_len = strlen(component);

    if (mapped_extension_index(component, extension_len) >= 0) {
        if (extension_len >= synthetic_extension_size) {
            return 0;
        }
        memcpy(synthetic_extension, component, extension_len + 1);

        if (component == relative) {
            return copy_string(dest, dest_size, image->backing_dir);
        }

        prefix_end = component - 1;
        if ((size_t)(prefix_end - relative) >= dest_size) {
            return 0;
        }

        memcpy(prefix, relative, (size_t)(prefix_end - relative));
        prefix[prefix_end - relative] = '\0';
        return append_relative_to_backing(image, prefix, dest, dest_size);
    }

    return append_relative_to_backing(image, relative, dest, dest_size);
}

static int is_active_image_entry(const HideousFS_Image *image, const char *dir,
                                 const char *leaf)
{
    return is_root_name(dir) && strcmp(leaf, image->image_leaf) == 0;
}

static int is_active_image_path(const HideousFS_Image *image, const char *name)
{
    return name != NULL && strcmp(skip_root_prefix(name), image->image_leaf) == 0;
}

static HideousFS_Image *claim_image(void)
{
    int i;

    for (i = 0; i < MaxImages; ++i) {
        if (!images[i].in_use) {
            images[i].in_use = 1;
            return &images[i];
        }
    }

    return NULL;
}

static void release_image(HideousFS_Image *image)
{
    image->in_use = 0;
    image->fileswitch_handle = 0;
}

static HideousFS_File *claim_file_handle(void)
{
    int i;

    for (i = 0; i < MaxOpenFiles; ++i) {
        if (!open_files[i].in_use) {
            open_files[i].in_use = 1;
            return &open_files[i];
        }
    }

    return NULL;
}

static void release_file_handle(HideousFS_File *file)
{
    file->in_use = 0;
    file->is_directory = 0;
    file->writable = 0;
    file->os_handle = 0;
    file->backing_path[0] = '\0';
}

static _kernel_oserror *read_file_info(const char *path, FS_cat_entry *entry)
{
    _kernel_swi_regs regs;
    _kernel_oserror *error;

    regs.r[0] = fsfile_ReadInfo;
    regs.r[1] = (int)path;

    error = _kernel_swi(OS_File, &regs, &regs);
    if (error != NULL) {
        return error;
    }

    entry->type = (word)regs.r[0];
    entry->loadaddr = (word)regs.r[2];
    entry->execaddr = (word)regs.r[3];
    entry->filelen = (word)regs.r[4];
    entry->fileattr = (word)regs.r[5];
    return NULL;
}

static int host_leaf_extension_index(const char *leaf, const char **base_end)
{
    const char *slash = strrchr(leaf, '/');

    if (slash == NULL || slash == leaf || slash[1] == '\0') {
        return -1;
    }

    if (base_end != NULL) {
        *base_end = slash;
    }

    return mapped_extension_index(slash + 1, strlen(slash + 1));
}

static _kernel_oserror *directory_contains_extension(const char *backing_dir,
                                                     const char *extension,
                                                     int *found)
{
    _kernel_swi_regs regs;
    _kernel_oserror *error;
    int offset = 0;

    *found = 0;

    while (offset != -1) {
        int extension_index;

        regs.r[0] = OSGBPB_DirEntries;
        regs.r[1] = (int)backing_dir;
        regs.r[2] = (int)temp_buffer;
        regs.r[3] = 1;
        regs.r[4] = offset;
        regs.r[5] = TempBufferSize;
        regs.r[6] = 0;

        error = _kernel_swi(OS_GBPB, &regs, &regs);
        if (error != NULL) {
            return error;
        }

        offset = regs.r[4];
        if (regs.r[3] == 0) {
            break;
        }

        extension_index = host_leaf_extension_index(temp_buffer, NULL);
        if (extension_index >= 0 &&
            strcmp(default_reverse_extensions[extension_index], extension) == 0) {
            *found = 1;
            return NULL;
        }
    }

    return NULL;
}

static void fill_synthetic_dir_info(FS_cat_entry *entry)
{
    entry->type = object_directory;
    entry->loadaddr = 0;
    entry->execaddr = 0;
    entry->filelen = 0;
    entry->fileattr = file_attr_owner_read | file_attr_public_read;
}

static _kernel_oserror *write_file_info(const char *path, word loadaddr,
                                        word execaddr, word attr)
{
    _kernel_swi_regs regs;

    regs.r[0] = fsfile_WriteInfo;
    regs.r[1] = (int)path;
    regs.r[2] = (int)loadaddr;
    regs.r[3] = (int)execaddr;
    regs.r[5] = (int)attr;

    return _kernel_swi(OS_File, &regs, &regs);
}

static int build_writable_object_path(const HideousFS_Image *image,
                                      const char *name,
                                      char *dest, size_t dest_size)
{
    if (is_active_image_path(image, name) ||
        is_mapped_extension(last_component(skip_root_prefix(name)))) {
        return 0;
    }

    return build_backing_object_path(image, name, dest, dest_size);
}

static _kernel_oserror *set_file_extent(HideousFS_File *file, word extent)
{
    _kernel_swi_regs regs;
    _kernel_oserror *error;

    regs.r[0] = OSArgs_SetExt;
    regs.r[1] = (int)file->os_handle;
    regs.r[2] = (int)extent;

    error = _kernel_swi(OS_Args, &regs, &regs);
    if (error == NULL) {
        file->extent = extent;
    }
    return error;
}

static _kernel_oserror *ensure_file_size(HideousFS_File *file, word size)
{
    _kernel_swi_regs regs;
    _kernel_oserror *error;

    regs.r[0] = OSArgs_SetAllocation;
    regs.r[1] = (int)file->os_handle;
    regs.r[2] = (int)size;

    error = _kernel_swi(OS_Args, &regs, &regs);
    if (error == NULL && size > file->extent) {
        file->extent = size;
    }
    return error;
}

static _kernel_oserror *read_image_path(HideousFS_Image *image)
{
    _kernel_swi_regs regs;
    _kernel_oserror *error;
    int needed;
    char *last_dot;

    regs.r[0] = OSArgs_ReadPath;
    regs.r[1] = (int)image->fileswitch_handle;
    regs.r[2] = 0;
    regs.r[5] = 0;

    error = _kernel_swi(OS_Args, &regs, &regs);
    if (error != NULL) {
        return error;
    }

    needed = 1 - regs.r[5];
    if (needed <= 0 || needed > MaxPath) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    regs.r[0] = OSArgs_ReadPath;
    regs.r[1] = (int)image->fileswitch_handle;
    regs.r[2] = (int)image->image_path;
    regs.r[5] = MaxPath;

    error = _kernel_swi(OS_Args, &regs, &regs);
    if (error != NULL) {
        return error;
    }

    last_dot = strrchr(image->image_path, '.');
    if (last_dot == NULL || last_dot[1] == '\0') {
        return (_kernel_oserror *)&err_not_found;
    }

    if (!copy_string(image->image_leaf, sizeof(image->image_leaf), last_dot + 1)) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    if ((size_t)(last_dot - image->image_path) >= sizeof(image->backing_dir)) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    memcpy(image->backing_dir, image->image_path, (size_t)(last_dot - image->image_path));
    image->backing_dir[last_dot - image->image_path] = '\0';
    return NULL;
}

static word module_offset(void (*entry)(void))
{
    union {
        void (*fn)(void);
        char *addr;
    } entry_address;

    entry_address.fn = entry;
    return (word)(entry_address.addr - __module_header);
}

static _kernel_oserror *register_image_fs(void *private_word)
{
    word info_block[9];
    _kernel_swi_regs regs;

    info_block[0] = 0;
    info_block[1] = HideousFS_FileType;
    info_block[2] = module_offset(hideousfs_fsentry_open);
    info_block[3] = module_offset(hideousfs_fsentry_getbytes);
    info_block[4] = module_offset(hideousfs_fsentry_putbytes);
    info_block[5] = module_offset(hideousfs_fsentry_args);
    info_block[6] = module_offset(hideousfs_fsentry_close);
    info_block[7] = module_offset(hideousfs_fsentry_file);
    info_block[8] = module_offset(hideousfs_fsentry_func);

    regs.r[0] = FSControl_RegisterImageFS;
    regs.r[1] = (int)__module_header;
    regs.r[2] = (int)((char *)info_block - __module_header);
    regs.r[3] = (int)private_word;

    return _kernel_swi(OS_FSControl, &regs, &regs);
}

_kernel_oserror *hideousfs_initialise(const char *cmd_tail, int podule_base, void *private_word)
{
    (void)cmd_tail;
    (void)podule_base;

    return register_image_fs(private_word);
}

_kernel_oserror *hideousfs_finalise(int fatal, int podule, void *private_word)
{
    _kernel_swi_regs regs;

    (void)fatal;
    (void)podule;
    (void)private_word;

    regs.r[0] = FSControl_DeregisterImageFS;
    regs.r[1] = HideousFS_FileType;

    (void)_kernel_swi(OS_FSControl, &regs, &regs);
    return NULL;
}

_kernel_oserror *hideousfs_fsentry_open_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_Image *image = (HideousFS_Image *)regs->r[6];
    HideousFS_File *file;
    _kernel_swi_regs os_regs;
    _kernel_oserror *error;
    char synthetic_extension[16];
    int found;
    int open_mode = regs->r[0];
    int writable;

    (void)private_word;

    if (image == NULL || !image->in_use || regs->r[1] == 0 ||
        is_active_image_path(image, (const char *)regs->r[1])) {
        return (_kernel_oserror *)&err_not_found;
    }
    if (open_mode < fsopen_ReadOnly || open_mode > fsopen_Update) {
        return operation_not_implemented("Open", open_mode);
    }

    if (!resolve_directory_path(image, (const char *)regs->r[1],
                                path_buffer, sizeof(path_buffer),
                                synthetic_extension,
                                sizeof(synthetic_extension))) {
        return (_kernel_oserror *)&err_path_too_long;
    }
    if (synthetic_extension[0] != '\0') {
        error = directory_contains_extension(path_buffer, synthetic_extension, &found);
        if (error != NULL) {
            return error;
        }
        if (found) {
            if (open_mode != fsopen_ReadOnly) {
                return (_kernel_oserror *)&err_not_found;
            }

            file = claim_file_handle();
            if (file == NULL) {
                return (_kernel_oserror *)&err_no_file_handles;
            }

            file->is_directory = 1;
            file->writable = 0;
            file->os_handle = 0;
            file->loadaddr = 0;
            file->execaddr = 0;
            file->extent = 0;
            file->attr = file_attr_owner_read | file_attr_public_read;
            file->backing_path[0] = '\0';

            open_block.information = fsopen_ReadPermission | fsopen_IsDirectory;
            open_block.inhand = file;
            open_block.buffsize = 0;
            open_block.fileext = 0;
            open_block.falloc = 0;

            regs->r[0] = (int)open_block.information;
            regs->r[1] = (int)open_block.inhand;
            regs->r[2] = (int)open_block.buffsize;
            regs->r[3] = (int)open_block.fileext;
            regs->r[4] = (int)open_block.falloc;
            return NULL;
        }
    }

    if (!build_backing_object_path(image, (const char *)regs->r[1],
                                   path_buffer, sizeof(path_buffer))) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    if (is_mapped_extension(last_component(skip_root_prefix((const char *)regs->r[1])))) {
        return (_kernel_oserror *)&err_not_found;
    }

    writable = open_mode != fsopen_ReadOnly;
    cat_entry.type = 0;
    cat_entry.loadaddr = 0;
    cat_entry.execaddr = 0;
    cat_entry.filelen = 0;
    cat_entry.fileattr = file_attr_owner_read | file_attr_owner_write |
                         file_attr_public_read | file_attr_public_write;

    if (open_mode != fsopen_CreateUpdate) {
        error = read_file_info(path_buffer, &cat_entry);
        if (error != NULL) {
            return error;
        }
    } else {
        error = read_file_info(path_buffer, &cat_entry);
        if (error != NULL) {
            cat_entry.type = object_file;
            cat_entry.loadaddr = 0;
            cat_entry.execaddr = 0;
            cat_entry.filelen = 0;
            cat_entry.fileattr = file_attr_owner_read | file_attr_owner_write |
                                 file_attr_public_read | file_attr_public_write;
        }
    }

    if (cat_entry.type == object_directory && writable) {
        return (_kernel_oserror *)&err_not_found;
    }
    if (cat_entry.type != object_file && cat_entry.type != object_directory) {
        return (_kernel_oserror *)&err_not_found;
    }

    file = claim_file_handle();
    if (file == NULL) {
        return (_kernel_oserror *)&err_no_file_handles;
    }

    if (cat_entry.type == object_file) {
        if (open_mode == fsopen_CreateUpdate) {
            os_regs.r[0] = OSFind_WriteFileNoPath;
        } else if (open_mode == fsopen_Update) {
            os_regs.r[0] = OSFind_UpdateFileNoPath;
        } else {
            os_regs.r[0] = OSFind_ReadFileNoPath;
        }
        os_regs.r[1] = (int)path_buffer;
        error = _kernel_swi(OS_Find, &os_regs, &os_regs);
        if (error != NULL) {
            release_file_handle(file);
            return error;
        }
        file->os_handle = (word)os_regs.r[0];
    } else {
        file->os_handle = 0;
    }

    file->is_directory = cat_entry.type == object_directory;
    file->writable = writable && !file->is_directory;
    file->loadaddr = cat_entry.loadaddr;
    file->execaddr = cat_entry.execaddr;
    file->extent = open_mode == fsopen_CreateUpdate ? 0 : cat_entry.filelen;
    file->attr = cat_entry.fileattr;
    if (!copy_string(file->backing_path, sizeof(file->backing_path), path_buffer)) {
        if (!file->is_directory && file->os_handle != 0) {
            os_regs.r[0] = OSFind_Close;
            os_regs.r[1] = (int)file->os_handle;
            (void)_kernel_swi(OS_Find, &os_regs, &os_regs);
        }
        release_file_handle(file);
        return (_kernel_oserror *)&err_path_too_long;
    }

    open_block.information = fsopen_ReadPermission |
                             (file->writable ? fsopen_WritePermission : 0) |
                             (file->is_directory ? fsopen_IsDirectory : 0);
    open_block.inhand = file;
    open_block.buffsize = 0;
    open_block.fileext = file->extent;
    open_block.falloc = file->extent;

    regs->r[0] = (int)open_block.information;
    regs->r[1] = (int)open_block.inhand;
    regs->r[2] = (int)open_block.buffsize;
    regs->r[3] = (int)open_block.fileext;
    regs->r[4] = (int)open_block.falloc;
    return NULL;
}

_kernel_oserror *hideousfs_fsentry_getbytes_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_File *file = (HideousFS_File *)regs->r[1];
    _kernel_swi_regs os_regs;

    (void)private_word;

    if (file == NULL || !file->in_use || file->is_directory) {
        return (_kernel_oserror *)&err_not_found;
    }

    os_regs.r[0] = OSGBPB_ReadAt;
    os_regs.r[1] = (int)file->os_handle;
    os_regs.r[2] = regs->r[2];
    os_regs.r[3] = regs->r[3];
    os_regs.r[4] = regs->r[4];

    return _kernel_swi(OS_GBPB, &os_regs, &os_regs);
}

_kernel_oserror *hideousfs_fsentry_putbytes_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_File *file = (HideousFS_File *)regs->r[1];
    _kernel_swi_regs os_regs;
    _kernel_oserror *error;
    word end_offset;

    (void)private_word;

    if (file == NULL || !file->in_use || file->is_directory || !file->writable) {
        return (_kernel_oserror *)&err_not_found;
    }

    if (regs->r[2] == -1) {
        os_regs.r[0] = regs->r[0] & 0xff;
        os_regs.r[1] = (int)file->os_handle;
        error = _kernel_swi(OS_BPut, &os_regs, &os_regs);
        if (error == NULL) {
            file->extent++;
        }
        return error;
    }

    os_regs.r[0] = OSGBPB_WriteAt;
    os_regs.r[1] = (int)file->os_handle;
    os_regs.r[2] = regs->r[2];
    os_regs.r[3] = regs->r[3];
    os_regs.r[4] = regs->r[4];

    error = _kernel_swi(OS_GBPB, &os_regs, &os_regs);
    if (error == NULL) {
        end_offset = (word)regs->r[4] + (word)regs->r[3];
        if (end_offset > file->extent) {
            file->extent = end_offset;
        }
    }
    return error;
}

_kernel_oserror *hideousfs_fsentry_args_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_File *file = (HideousFS_File *)regs->r[1];

    (void)private_word;

    if (file == NULL || !file->in_use) {
        return (_kernel_oserror *)&err_not_found;
    }

    switch (regs->r[0]) {
    case fsargs_SetEXT:
        if (file->is_directory || !file->writable) {
            return (_kernel_oserror *)&err_not_found;
        }
        return set_file_extent(file, (word)regs->r[2]);

    case fsargs_ReadEXT:
    case fsargs_ReadSize:
        regs->r[2] = (int)file->extent;
        return NULL;

    case fsargs_Flush:
    case fsargs_ReadLoadExec:
        datestamp.loadaddr = file->loadaddr;
        datestamp.execaddr = file->execaddr;
        regs->r[2] = (int)datestamp.loadaddr;
        regs->r[3] = (int)datestamp.execaddr;
        return NULL;

    case fsargs_EnsureSize:
        if (file->is_directory || !file->writable) {
            return (_kernel_oserror *)&err_not_found;
        }
        return ensure_file_size(file, (word)regs->r[2]);

    default:
        return operation_not_implemented("Args", regs->r[0]);
    }
}

_kernel_oserror *hideousfs_fsentry_close_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_File *file = (HideousFS_File *)regs->r[1];
    _kernel_swi_regs os_regs;
    _kernel_oserror *error;

    (void)private_word;

    if (file == NULL || !file->in_use) {
        return (_kernel_oserror *)&err_not_found;
    }

    if (file->is_directory) {
        error = NULL;
    } else {
        os_regs.r[0] = OSFind_Close;
        os_regs.r[1] = (int)file->os_handle;
        error = _kernel_swi(OS_Find, &os_regs, &os_regs);
        if (error == NULL && file->writable) {
            file->loadaddr = (word)regs->r[2];
            file->execaddr = (word)regs->r[3];
            error = write_file_info(file->backing_path, file->loadaddr,
                                    file->execaddr, file->attr);
        }
    }

    release_file_handle(file);
    return error;
}

_kernel_oserror *hideousfs_fsentry_file_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_Image *image = (HideousFS_Image *)regs->r[6];
    _kernel_swi_regs os_regs;
    _kernel_oserror *error;
    char synthetic_extension[16];
    int found;

    (void)private_word;

    if (image == NULL || !image->in_use || regs->r[1] == 0) {
        return operation_not_implemented("FSFile", regs->r[0]);
    }

    switch (regs->r[0]) {
    case fsfile_Save:
    case fsfile_Create:
        if (!build_writable_object_path(image, (const char *)regs->r[1],
                                        path_buffer, sizeof(path_buffer))) {
            return (_kernel_oserror *)&err_not_found;
        }

        os_regs.r[0] = regs->r[0];
        os_regs.r[1] = (int)path_buffer;
        os_regs.r[2] = regs->r[2];
        os_regs.r[3] = regs->r[3];
        os_regs.r[4] = regs->r[4];
        os_regs.r[5] = regs->r[5];
        return _kernel_swi(OS_File, &os_regs, &os_regs);

    case fsfile_WriteInfo:
    case fsfile_WriteLoad:
    case fsfile_WriteExec:
    case fsfile_WriteAttr:
    case fsfile_Delete:
        if (!build_writable_object_path(image, (const char *)regs->r[1],
                                        path_buffer, sizeof(path_buffer))) {
            return (_kernel_oserror *)&err_not_found;
        }

        os_regs.r[0] = regs->r[0];
        os_regs.r[1] = (int)path_buffer;
        os_regs.r[2] = regs->r[2];
        os_regs.r[3] = regs->r[3];
        os_regs.r[4] = regs->r[4];
        os_regs.r[5] = regs->r[5];
        error = _kernel_swi(OS_File, &os_regs, &os_regs);
        if (error != NULL) {
            return error;
        }
        regs->r[0] = os_regs.r[0];
        regs->r[2] = os_regs.r[2];
        regs->r[3] = os_regs.r[3];
        regs->r[4] = os_regs.r[4];
        regs->r[5] = os_regs.r[5];
        return NULL;

    case fsfile_ReadInfo:
        if (is_active_image_path(image, (const char *)regs->r[1])) {
            return (_kernel_oserror *)&err_not_found;
        }

        if (!resolve_directory_path(image, (const char *)regs->r[1],
                                    path_buffer, sizeof(path_buffer),
                                    synthetic_extension,
                                    sizeof(synthetic_extension))) {
            return (_kernel_oserror *)&err_path_too_long;
        }
        if (synthetic_extension[0] != '\0') {
            error = directory_contains_extension(path_buffer, synthetic_extension, &found);
            if (error != NULL) {
                return error;
            }
            if (found) {
                fill_synthetic_dir_info(&cat_entry);
                regs->r[0] = (int)cat_entry.type;
                regs->r[2] = (int)cat_entry.loadaddr;
                regs->r[3] = (int)cat_entry.execaddr;
                regs->r[4] = (int)cat_entry.filelen;
                regs->r[5] = (int)cat_entry.fileattr;
                return NULL;
            }
        }

        if (!build_backing_object_path(image, (const char *)regs->r[1],
                                       path_buffer, sizeof(path_buffer))) {
            return (_kernel_oserror *)&err_path_too_long;
        }

        error = read_file_info(path_buffer, &cat_entry);
        if (error != NULL) {
            return error;
        }
        if (is_mapped_extension(last_component(skip_root_prefix((const char *)regs->r[1])))) {
            return (_kernel_oserror *)&err_not_found;
        }

        regs->r[0] = (int)cat_entry.type;
        regs->r[2] = (int)cat_entry.loadaddr;
        regs->r[3] = (int)cat_entry.execaddr;
        regs->r[4] = (int)cat_entry.filelen;
        regs->r[5] = (int)cat_entry.fileattr;
        return NULL;

    default:
        return operation_not_implemented("FSFile", regs->r[0]);
    }
}

static word dir_entry_size(const char *name, int with_info)
{
    if (with_info) {
        return word_align((word)(offsetof(FS_entry_info, fname) + strlen(name) + 1));
    }

    return (word)strlen(name) + 1;
}

static void write_dir_entry(char *dest, const FS_entry_info *source,
                            const char *name, int with_info,
                            int synthetic_directory)
{
    if (with_info) {
        FS_entry_info *entry = (FS_entry_info *)dest;

        if (synthetic_directory) {
            entry->loadaddr = 0;
            entry->execaddr = 0;
            entry->flength = 0;
            entry->attributes = file_attr_owner_read | file_attr_public_read;
            entry->type = object_directory;
        } else {
            entry->loadaddr = source->loadaddr;
            entry->execaddr = source->execaddr;
            entry->flength = source->flength;
            entry->attributes = source->attributes;
            entry->type = source->type;
        }
        strcpy(entry->fname, name);
    } else {
        strcpy(dest, name);
    }
}

static _kernel_oserror *read_dir(_kernel_swi_regs *regs, int with_info)
{
    HideousFS_Image *image = (HideousFS_Image *)regs->r[6];
    const char *dir_name = (const char *)regs->r[1];
    char *dest = (char *)regs->r[2];
    word num = (word)regs->r[3];
    word buffer_len = (word)regs->r[5];
    word used = 0;
    word output_count = 0;
    word projected_index = 0;
    word requested_offset = (word)regs->r[4];
    word seen_extensions = 0;
    int backing_offset = 0;
    int next_offset = -1;
    char synthetic_extension[16];
    _kernel_oserror *error;

    dir_block.objects_read = 0;
    dir_block.next_offset = -1;

    if (image == NULL || !image->in_use) {
        return (_kernel_oserror *)&err_not_found;
    }

    if (!resolve_directory_path(image, dir_name, path_buffer, sizeof(path_buffer),
                                synthetic_extension,
                                sizeof(synthetic_extension))) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    if ((int)requested_offset == -1 || num == 0) {
        regs->r[3] = 0;
        regs->r[4] = -1;
        return NULL;
    }

    while (backing_offset != -1) {
        _kernel_swi_regs os_regs;
        FS_entry_info *source;
        word entry_size;
        char projected_name[MaxPath];
        const char *leaf;
        const char *base_end;
        int extension_index;
        int synthetic_directory = 0;
        int skip = 0;

        os_regs.r[0] = OSGBPB_DirEntriesInfo;
        os_regs.r[1] = (int)path_buffer;
        os_regs.r[2] = (int)temp_buffer;
        os_regs.r[3] = 1;
        os_regs.r[4] = backing_offset;
        os_regs.r[5] = TempBufferSize;
        os_regs.r[6] = 0;

        error = _kernel_swi(OS_GBPB, &os_regs, &os_regs);
        if (error != NULL) {
            return error;
        }

        backing_offset = os_regs.r[4];
        if (os_regs.r[3] == 0) {
            break;
        }

        source = (FS_entry_info *)temp_buffer;
        leaf = source->fname;

        if (synthetic_extension[0] == '\0' &&
            is_active_image_entry(image, dir_name, leaf)) {
            skip = 1;
        }

        if (!skip && synthetic_extension[0] == '\0' &&
            is_mapped_extension(leaf)) {
            skip = 1;
        }

        extension_index = host_leaf_extension_index(leaf, &base_end);
        if (!skip && synthetic_extension[0] != '\0') {
            if (extension_index < 0 ||
                strcmp(default_reverse_extensions[extension_index],
                       synthetic_extension) != 0) {
                skip = 1;
            } else if ((size_t)(base_end - leaf) >= sizeof(projected_name)) {
                return (_kernel_oserror *)&err_path_too_long;
            } else {
                memcpy(projected_name, leaf, (size_t)(base_end - leaf));
                projected_name[base_end - leaf] = '\0';
            }
        } else if (!skip && extension_index >= 0) {
            if ((seen_extensions & (1u << extension_index)) != 0) {
                skip = 1;
            } else {
                seen_extensions |= 1u << extension_index;
                if (!copy_string(projected_name, sizeof(projected_name),
                                 default_reverse_extensions[extension_index])) {
                    return (_kernel_oserror *)&err_path_too_long;
                }
                synthetic_directory = 1;
            }
        } else if (!skip) {
            if (!copy_string(projected_name, sizeof(projected_name), leaf)) {
                return (_kernel_oserror *)&err_path_too_long;
            }
        }

        if (skip) {
            continue;
        }

        if (projected_index < requested_offset) {
            projected_index++;
            continue;
        }

        entry_size = dir_entry_size(projected_name, with_info);
        if (used + entry_size > buffer_len) {
            next_offset = (int)projected_index;
            break;
        }

        write_dir_entry(dest + used, source, projected_name, with_info,
                        synthetic_directory);
        used += entry_size;
        output_count++;
        projected_index++;

        if (output_count == num) {
            next_offset = (int)projected_index;
            break;
        }
    }

    if (backing_offset == -1) {
        next_offset = -1;
    } else if (next_offset == -1 && output_count != 0) {
        next_offset = (int)projected_index;
    }

    if (output_count == 0 && next_offset != -1) {
        return (_kernel_oserror *)&err_buffer_too_small;
    }

    dir_block.objects_read = output_count;
    dir_block.next_offset = next_offset;
    regs->r[3] = (int)dir_block.objects_read;
    regs->r[4] = dir_block.next_offset;
    return NULL;
}

_kernel_oserror *hideousfs_fsentry_func_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_Image *image;
    _kernel_oserror *error;

    (void)private_word;

    switch (regs->r[0]) {
    case fsfunc_ReadDirEntries:
        return read_dir(regs, 0);

    case fsfunc_ReadDirEntriesInfo:
        return read_dir(regs, 1);

    case fsfunc_NewImage:
        image = claim_image();
        if (image == NULL) {
            return (_kernel_oserror *)&err_no_image_handles;
        }

        image->fileswitch_handle = (word)regs->r[1];
        image->buffer_size = (word)regs->r[2];
        error = read_image_path(image);
        if (error != NULL) {
            release_image(image);
            return error;
        }
        regs->r[1] = (int)image;
        return NULL;

    case fsfunc_ImageClosing:
        image = (HideousFS_Image *)regs->r[1];
        if (image != NULL && image->in_use) {
            release_image(image);
        }
        return NULL;

    case fsfunc_ReadBootOption:
        regs->r[2] = 0;
        return NULL;

    case fsfunc_WriteBootOption:
        return NULL;

    default:
        return operation_not_implemented("FSFunc", regs->r[0]);
    }
}
