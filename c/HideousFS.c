#include <stddef.h>
#include <string.h>

#include "kernel.h"

#include "cmhg.h"

typedef unsigned int word;

enum {
    HideousFS_FileType = 0x001,

    OS_FSControl = 0x29,
    FSControl_RegisterImageFS = 0x23,
    FSControl_DeregisterImageFS = 0x24,

    fsfile_ReadInfo = 5,

    fsargs_ReadSize = 2,
    fsargs_Flush = 3,
    fsargs_ReadLoadExec = 6,

    fsfunc_ReadDirEntries = 14,
    fsfunc_ReadDirEntriesInfo = 15,
    fsfunc_NewImage = 21,
    fsfunc_ImageClosing = 22,
    fsfunc_ReadBootOption = 27,
    fsfunc_WriteBootOption = 28,

    object_file = 1,

    file_attr_owner_read = 1 << 0,
    file_attr_owner_write = 1 << 1
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
    word fileswitch_handle;
    word buffer_size;
} HideousFS_Image;

extern char __module_header[];

static FS_cat_entry cat_entry;
static FS_datestamp datestamp;
static FS_open_block open_block;
static FS_dir_block dir_block;
static const char hello_contents[] = "";
static const char hello_name[] = "Hello";
static HideousFS_Image single_image;

static const _kernel_oserror err_buffer_too_small = {
    0x808002, "HideousFS: buffer too small"
};

static const _kernel_oserror err_not_found = {
    0x808003, "HideousFS: object not found"
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
    (void)private_word;

    if (regs->r[1] != 0 && strcmp((const char *)regs->r[1], hello_name) == 0) {
        open_block.information = object_file;
        open_block.inhand = (void *)hello_contents;
        open_block.buffsize = 0;
        open_block.fileext = sizeof(hello_contents) - 1;
        open_block.falloc = sizeof(hello_contents) - 1;

        regs->r[0] = (int)open_block.information;
        regs->r[1] = (int)open_block.inhand;
        regs->r[2] = (int)open_block.buffsize;
        regs->r[3] = (int)open_block.fileext;
        regs->r[4] = (int)open_block.falloc;
        return NULL;
    }

    return (_kernel_oserror *)&err_not_found;
}

_kernel_oserror *hideousfs_fsentry_getbytes_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)private_word;

    regs->r[3] = 0;
    return NULL;
}

_kernel_oserror *hideousfs_fsentry_putbytes_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)private_word;

    return operation_not_implemented("PutBytes", regs->r[0]);
}

_kernel_oserror *hideousfs_fsentry_args_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)private_word;

    switch (regs->r[0]) {
    case fsargs_ReadSize:
        regs->r[2] = sizeof(hello_contents) - 1;
        return NULL;

    case fsargs_Flush:
    case fsargs_ReadLoadExec:
        datestamp.loadaddr = 0xfff00000u;
        datestamp.execaddr = 0;
        regs->r[2] = (int)datestamp.loadaddr;
        regs->r[3] = (int)datestamp.execaddr;
        return NULL;

    default:
        return operation_not_implemented("Args", regs->r[0]);
    }
}

_kernel_oserror *hideousfs_fsentry_close_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)regs;
    (void)private_word;

    return NULL;
}

_kernel_oserror *hideousfs_fsentry_file_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)private_word;

    if (regs->r[0] == fsfile_ReadInfo && regs->r[1] != 0 &&
        strcmp((const char *)regs->r[1], hello_name) == 0) {
        cat_entry.type = object_file;
        cat_entry.loadaddr = 0xfff00000u;
        cat_entry.execaddr = 0;
        cat_entry.filelen = 0;
        cat_entry.fileattr = file_attr_owner_read;

        regs->r[0] = (int)cat_entry.type;
        regs->r[2] = (int)cat_entry.loadaddr;
        regs->r[3] = (int)cat_entry.execaddr;
        regs->r[4] = (int)cat_entry.filelen;
        regs->r[5] = (int)cat_entry.fileattr;
        return NULL;
    }

    return operation_not_implemented("FSFile", regs->r[0]);
}

static _kernel_oserror *read_dir(_kernel_swi_regs *regs, int with_info)
{
    char *dest = (char *)regs->r[2];
    word num = (word)regs->r[3];
    word offset = (word)regs->r[4];
    word buffer_len = (word)regs->r[5];
    word required;

    dir_block.objects_read = 0;
    dir_block.next_offset = -1;

    if (offset != 0 || num == 0) {
        regs->r[3] = 0;
        regs->r[4] = -1;
        return NULL;
    }

    if (with_info) {
        FS_entry_info *entry = (FS_entry_info *)dest;

        required = word_align((word)(offsetof(FS_entry_info, fname) + sizeof(hello_name)));
        if (buffer_len < required) {
            return (_kernel_oserror *)&err_buffer_too_small;
        }

        entry->loadaddr = 0xfff00000u;
        entry->execaddr = 0;
        entry->flength = 0;
        entry->attributes = file_attr_owner_read | file_attr_owner_write;
        entry->type = object_file;
        strcpy(entry->fname, hello_name);
    } else {
        required = sizeof(hello_name);
        if (buffer_len < required) {
            return (_kernel_oserror *)&err_buffer_too_small;
        }

        strcpy(dest, hello_name);
    }

    dir_block.objects_read = 1;
    dir_block.next_offset = -1;

    regs->r[3] = (int)dir_block.objects_read;
    regs->r[4] = dir_block.next_offset;
    return NULL;
}

_kernel_oserror *hideousfs_fsentry_func_handler(_kernel_swi_regs *regs, void *private_word)
{
    (void)private_word;

    switch (regs->r[0]) {
    case fsfunc_ReadDirEntries:
        return read_dir(regs, 0);

    case fsfunc_ReadDirEntriesInfo:
        return read_dir(regs, 1);

    case fsfunc_NewImage:
        single_image.fileswitch_handle = (word)regs->r[1];
        single_image.buffer_size = (word)regs->r[2];
        regs->r[1] = (int)&single_image;
        return NULL;

    case fsfunc_ImageClosing:
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
