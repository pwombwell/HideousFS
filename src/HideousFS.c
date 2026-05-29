#include "kernel.h"

#include "cmhg.h"

#include "HideousFSRiscos.h"

extern char __module_header[];

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
