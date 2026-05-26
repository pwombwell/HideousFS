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
    OS_FSControl = 0x29,

    OSFind_Close = 0,
    OSFind_ReadFileNoPath = 0x4f,

    OSGBPB_ReadAt = 3,
    OSGBPB_DirEntries = 9,
    OSGBPB_DirEntriesInfo = 10,

    OSArgs_ReadExt = 2,
    OSArgs_ReadPath = 7,

    FSControl_RegisterImageFS = 0x23,
    FSControl_DeregisterImageFS = 0x24,

    fsfile_ReadInfo = 5,

    fsargs_ReadEXT = 2,
    fsargs_ReadSize = 4,
    fsargs_Flush = 6,
    fsargs_ReadLoadExec = 9,

    fsfunc_ReadDirEntries = 14,
    fsfunc_ReadDirEntriesInfo = 15,
    fsfunc_NewImage = 21,
    fsfunc_ImageClosing = 22,
    fsfunc_ReadBootOption = 27,
    fsfunc_WriteBootOption = 28,

    object_file = 1,

    fsopen_ReadPermission = 1u << 30,

    MaxPath = 768,
    TempBufferSize = 1024,
    MaxImages = 8,
    MaxOpenFiles = 8
};

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
    word os_handle;
    word loadaddr;
    word execaddr;
    word extent;
    word attr;
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

static int build_backing_path(const HideousFS_Image *image, const char *name,
                              char *dest, size_t dest_size)
{
    size_t dir_len;
    size_t name_len;

    if (is_root_name(name)) {
        return copy_string(dest, dest_size, image->backing_dir);
    }

    name = skip_root_prefix(name);
    dir_len = strlen(image->backing_dir);
    name_len = strlen(name);

    if (dir_len + 1 + name_len >= dest_size) {
        return 0;
    }

    memcpy(dest, image->backing_dir, dir_len);
    dest[dir_len] = '.';
    memcpy(dest + dir_len + 1, name, name_len + 1);
    return 1;
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
    file->os_handle = 0;
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

    (void)private_word;

    if (image == NULL || !image->in_use || regs->r[1] == 0 ||
        is_active_image_path(image, (const char *)regs->r[1])) {
        return (_kernel_oserror *)&err_not_found;
    }

    if (!build_backing_path(image, (const char *)regs->r[1],
                            path_buffer, sizeof(path_buffer))) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    error = read_file_info(path_buffer, &cat_entry);
    if (error != NULL) {
        return error;
    }
    if (cat_entry.type != object_file) {
        return (_kernel_oserror *)&err_not_found;
    }

    file = claim_file_handle();
    if (file == NULL) {
        return (_kernel_oserror *)&err_no_file_handles;
    }

    os_regs.r[0] = OSFind_ReadFileNoPath;
    os_regs.r[1] = (int)path_buffer;
    error = _kernel_swi(OS_Find, &os_regs, &os_regs);
    if (error != NULL) {
        release_file_handle(file);
        return error;
    }

    file->os_handle = (word)os_regs.r[0];
    file->loadaddr = cat_entry.loadaddr;
    file->execaddr = cat_entry.execaddr;
    file->extent = cat_entry.filelen;
    file->attr = cat_entry.fileattr;

    open_block.information = fsopen_ReadPermission;
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

    if (file == NULL || !file->in_use) {
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
    (void)private_word;

    return operation_not_implemented("PutBytes", regs->r[0]);
}

_kernel_oserror *hideousfs_fsentry_args_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_File *file = (HideousFS_File *)regs->r[1];

    (void)private_word;

    if (file == NULL || !file->in_use) {
        return (_kernel_oserror *)&err_not_found;
    }

    switch (regs->r[0]) {
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

    os_regs.r[0] = OSFind_Close;
    os_regs.r[1] = (int)file->os_handle;
    error = _kernel_swi(OS_Find, &os_regs, &os_regs);

    release_file_handle(file);
    return error;
}

_kernel_oserror *hideousfs_fsentry_file_handler(_kernel_swi_regs *regs, void *private_word)
{
    HideousFS_Image *image = (HideousFS_Image *)regs->r[6];
    _kernel_oserror *error;

    (void)private_word;

    if (regs->r[0] == fsfile_ReadInfo && image != NULL && image->in_use &&
        regs->r[1] != 0) {
        if (is_active_image_path(image, (const char *)regs->r[1])) {
            return (_kernel_oserror *)&err_not_found;
        }

        if (!build_backing_path(image, (const char *)regs->r[1],
                                path_buffer, sizeof(path_buffer))) {
            return (_kernel_oserror *)&err_path_too_long;
        }

        error = read_file_info(path_buffer, &cat_entry);
        if (error != NULL) {
            return error;
        }

        regs->r[0] = (int)cat_entry.type;
        regs->r[2] = (int)cat_entry.loadaddr;
        regs->r[3] = (int)cat_entry.execaddr;
        regs->r[4] = (int)cat_entry.filelen;
        regs->r[5] = (int)cat_entry.fileattr;
        return NULL;
    }

    return operation_not_implemented("FSFile", regs->r[0]);
}

static word dir_entry_info_size(const FS_entry_info *entry)
{
    return word_align((word)(offsetof(FS_entry_info, fname) + strlen(entry->fname) + 1));
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
    int next_offset = regs->r[4];
    _kernel_oserror *error;

    dir_block.objects_read = 0;
    dir_block.next_offset = -1;

    if (image == NULL || !image->in_use) {
        return (_kernel_oserror *)&err_not_found;
    }

    if (!build_backing_path(image, dir_name, path_buffer, sizeof(path_buffer))) {
        return (_kernel_oserror *)&err_path_too_long;
    }

    if (next_offset == -1 || num == 0) {
        regs->r[3] = 0;
        regs->r[4] = -1;
        return NULL;
    }

    while (output_count < num && next_offset != -1) {
        _kernel_swi_regs os_regs;
        int before_offset = next_offset;
        word entry_size;
        const char *leaf;

        os_regs.r[0] = with_info ? OSGBPB_DirEntriesInfo : OSGBPB_DirEntries;
        os_regs.r[1] = (int)path_buffer;
        os_regs.r[2] = (int)temp_buffer;
        os_regs.r[3] = 1;
        os_regs.r[4] = next_offset;
        os_regs.r[5] = TempBufferSize;
        os_regs.r[6] = 0;

        error = _kernel_swi(OS_GBPB, &os_regs, &os_regs);
        if (error != NULL) {
            return error;
        }

        next_offset = os_regs.r[4];
        if (os_regs.r[3] == 0) {
            break;
        }

        if (with_info) {
            FS_entry_info *source = (FS_entry_info *)temp_buffer;

            leaf = source->fname;
            entry_size = dir_entry_info_size(source);
        } else {
            leaf = temp_buffer;
            entry_size = (word)strlen(leaf) + 1;
        }

        if (is_active_image_entry(image, dir_name, leaf)) {
            continue;
        }

        if (used + entry_size > buffer_len) {
            next_offset = before_offset;
            break;
        }

        memcpy(dest + used, temp_buffer, entry_size);
        used += entry_size;
        output_count++;
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
