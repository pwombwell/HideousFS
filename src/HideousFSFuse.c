#define FUSE_USE_VERSION 31

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <fuse.h>

#include "HideousFS.h"

typedef enum HideousFS_FuseExtensionMode {
    fuse_extension_directory,
    fuse_extension_suffix,
    fuse_extension_pass
} HideousFS_FuseExtensionMode;

typedef enum HideousFS_FuseFiletypeMode {
    fuse_filetypes_pass,
    fuse_filetypes_suffix,
    fuse_filetypes_xattr
} HideousFS_FuseFiletypeMode;

typedef struct HideousFS_Fuse {
    char backing_dir[PATH_MAX];
    HideousFS_FuseExtensionMode extension_mode;
    HideousFS_FuseFiletypeMode filetype_mode;
    HideousFS_Image mapping;
    int readonly;
} HideousFS_Fuse;

#ifdef __APPLE__
typedef struct fuse_darwin_attr HideousFS_FuseAttr;
typedef fuse_darwin_fill_dir_t HideousFS_FuseFillDir;
#else
typedef struct stat HideousFS_FuseAttr;
typedef fuse_fill_dir_t HideousFS_FuseFillDir;
#endif

#define RISCOS_LOAD_EXEC_XATTR "user.RISC_OS.LoadExec"
#define MaxMetadataSuffix 32

static void stat_to_fuse_attr(const struct stat *st, HideousFS_FuseAttr *attr);
static int lstat_fuse_attr(const char *path, HideousFS_FuseAttr *attr);
static int host_getxattr(const char *path, const char *name, void *value,
                         size_t size);
static int host_setxattr(const char *path, const char *name, const void *value,
                         size_t size, int flags);
static int host_listxattr(const char *path, char *list, size_t size);
static int host_removexattr(const char *path, const char *name);
static int parse_reverse_list(HideousFS_Fuse *fs, const char *list);
static int is_hex_digit(char ch);
static int is_hex_string(const char *text, size_t len);
static int is_metadata_suffix_text(const char *text);
static int split_metadata_suffix(const char *leaf, size_t leaf_len,
                                 size_t *base_len, const char **suffix);
static int strip_metadata_suffix(const char *src, char *dest,
                                 size_t dest_size);
static int loadexec_to_suffix(const void *value, size_t size,
                              char *suffix, size_t suffix_size);
static int suffix_to_loadexec(const char *suffix, unsigned char *value,
                              size_t *size);
static int read_xattr_suffix(const char *path, char *suffix,
                             size_t suffix_size);
static int append_metadata_suffix(char *dest, size_t dest_size,
                                  const char *suffix);
static int append_path_component(char *dest, size_t dest_size,
                                 const char *component,
                                 size_t component_len);
static int join_path(char *dest, size_t dest_size, const char *base,
                     const char *relative);
static const char *skip_mount_root(const char *path);
static const char *last_path_component(const char *path);
static const char *previous_path_component(const char *path,
                                           const char *component);
static int split_extension(const HideousFS_Fuse *fs, const char *leaf,
                           const char **extension, size_t *base_len);
static int split_mapped_extension(const HideousFS_Fuse *fs, const char *leaf,
                                  const char **extension, size_t *base_len);
static int is_mapped_extension_component(const HideousFS_Fuse *fs,
                                         const char *component,
                                         size_t component_len);
static int is_hidden_real_name(const HideousFS_Fuse *fs, const char *leaf,
                               int is_directory);
static int is_hidden_presented_path(const HideousFS_Fuse *fs,
                                    const char *path);
static int ensure_parent_directory(const char *path);
static int find_comma_suffix_variant(const char *path, char *dest,
                                     size_t dest_size);
static int find_xattr_base_variant(const char *path, char *dest,
                                   size_t dest_size);
static int build_backing_path(const HideousFS_Fuse *fs, const char *path,
                              char *dest, size_t dest_size);
static int resolve_existing_backing_path(const HideousFS_Fuse *fs,
                                         const char *path,
                                         char *dest, size_t dest_size);
static int build_presented_leaf(const HideousFS_Fuse *fs,
                                const char *backing_dir,
                                const char *leaf,
                                char *dest, size_t dest_size);
static int resolve_directory_path(const HideousFS_Fuse *fs, const char *path,
                                  char *dest, size_t dest_size,
                                  char *synthetic_extension,
                                  size_t synthetic_extension_size);
static int directory_contains_extension(const HideousFS_Fuse *fs,
                                        const char *backing_dir,
                                        const char *extension);
static int fill_synthetic_dir_attr(HideousFS_FuseAttr *attr);
static int add_readdir_entry(void *buf, HideousFS_FuseFillDir filler,
                             const char *name, const HideousFS_FuseAttr *attr,
                             off_t offset);
static int read_projected_directory(const HideousFS_Fuse *fs,
                                    const char *backing_dir, void *buf,
                                    HideousFS_FuseFillDir filler, off_t offset,
                                    enum fuse_readdir_flags flags);
static int read_synthetic_directory(const HideousFS_Fuse *fs,
                                    const char *backing_dir,
                                    const char *extension, void *buf,
                                    HideousFS_FuseFillDir filler, off_t offset,
                                    enum fuse_readdir_flags flags);
static int read_suffix_projected_directory(const HideousFS_Fuse *fs,
                                           const char *backing_dir,
                                           void *buf,
                                           HideousFS_FuseFillDir filler,
                                           off_t offset,
                                           enum fuse_readdir_flags flags);
static int parse_args(int argc, char **argv, HideousFS_Fuse *fs,
                      char ***fuse_argv_out, int *fuse_argc_out,
                      int *run_selftest);
static int run_selftest(void);

static void stat_to_fuse_attr(const struct stat *st, HideousFS_FuseAttr *attr)
{
#ifdef __APPLE__
    memset(attr, 0, sizeof(*attr));
    attr->ino = st->st_ino;
    attr->mode = st->st_mode;
    attr->nlink = st->st_nlink;
    attr->uid = st->st_uid;
    attr->gid = st->st_gid;
    attr->rdev = st->st_rdev;
    attr->atimespec = st->st_atimespec;
    attr->mtimespec = st->st_mtimespec;
    attr->ctimespec = st->st_ctimespec;
    attr->btimespec = st->st_birthtimespec;
    attr->size = st->st_size;
    attr->blocks = st->st_blocks;
    attr->blksize = st->st_blksize;
    attr->flags = st->st_flags;
#else
    *attr = *st;
#endif
}

static int lstat_fuse_attr(const char *path, HideousFS_FuseAttr *attr)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        return -errno;
    }

    stat_to_fuse_attr(&st, attr);
    return 0;
}

static int host_getxattr(const char *path, const char *name, void *value,
                         size_t size)
{
#ifdef __APPLE__
    return (int)getxattr(path, name, value, size, 0, 0);
#else
    return (int)getxattr(path, name, value, size);
#endif
}

static int host_setxattr(const char *path, const char *name, const void *value,
                         size_t size, int flags)
{
#ifdef __APPLE__
    return setxattr(path, name, value, size, 0, flags);
#else
    return setxattr(path, name, value, size, flags);
#endif
}

static int host_listxattr(const char *path, char *list, size_t size)
{
#ifdef __APPLE__
    return (int)listxattr(path, list, size, 0);
#else
    return (int)listxattr(path, list, size);
#endif
}

static int host_removexattr(const char *path, const char *name)
{
#ifdef __APPLE__
    return removexattr(path, name, 0);
#else
    return removexattr(path, name);
#endif
}

static int parse_reverse_list(HideousFS_Fuse *fs, const char *list)
{
    char copy[256];
    char *cursor;
    char *start;
    size_t len;

    if (strlen(list) >= sizeof(copy)) {
        return 0;
    }

    strcpy(copy, list);
    fs->mapping.reverse_extension_count = 0;

    cursor = copy;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        start = cursor;
        while (*cursor != '\0' && *cursor != ',' &&
               *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }

        len = (size_t)(cursor - start);
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
        if (len == 0 || !hideousfs_add_reverse_extension(&fs->mapping, start)) {
            return 0;
        }
    }

    return fs->mapping.reverse_extension_count != 0;
}

static int is_hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int is_hex_string(const char *text, size_t len)
{
    size_t i;

    if (len == 0) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        if (!is_hex_digit(text[i])) {
            return 0;
        }
    }

    return 1;
}

static int is_metadata_suffix_text(const char *text)
{
    const char *dash;
    size_t len = strlen(text);
    size_t left_len;
    size_t right_len;

    if (len >= 1 && len <= 3 && is_hex_string(text, len)) {
        return 1;
    }

    dash = strchr(text, '-');
    if (dash == NULL || strchr(dash + 1, '-') != NULL) {
        return 0;
    }

    left_len = (size_t)(dash - text);
    right_len = strlen(dash + 1);
    return left_len >= 1 && left_len <= 8 &&
           right_len >= 1 && right_len <= 8 &&
           is_hex_string(text, left_len) &&
           is_hex_string(dash + 1, right_len);
}

static int split_metadata_suffix(const char *leaf, size_t leaf_len,
                                 size_t *base_len, const char **suffix)
{
    const char *comma = NULL;
    size_t i;

    for (i = leaf_len; i > 0; --i) {
        if (leaf[i - 1] == ',') {
            comma = leaf + i - 1;
            break;
        }
    }
    if (comma == NULL || comma == leaf || comma[1] == '\0') {
        return 0;
    }

    if (!is_metadata_suffix_text(comma + 1)) {
        return 0;
    }

    if (base_len != NULL) {
        *base_len = (size_t)(comma - leaf);
    }
    if (suffix != NULL) {
        *suffix = comma;
    }
    return 1;
}

static int strip_metadata_suffix(const char *src, char *dest,
                                 size_t dest_size)
{
    const char *leaf = last_path_component(src);
    const char *suffix;
    size_t prefix_len = (size_t)(leaf - src);
    size_t base_len;

    if (!split_metadata_suffix(leaf, strlen(leaf), &base_len, &suffix)) {
        return hideousfs_copy_string(dest, dest_size, src);
    }

    if (prefix_len + base_len >= dest_size) {
        return 0;
    }

    memcpy(dest, src, prefix_len);
    memcpy(dest + prefix_len, leaf, base_len);
    dest[prefix_len + base_len] = '\0';
    return 1;
}

static int loadexec_to_suffix(const void *value, size_t size,
                              char *suffix, size_t suffix_size)
{
    uint32_t load;
    uint32_t exec;

    if (size < 8) {
        return 0;
    }

    memcpy(&load, value, sizeof(load));
    memcpy(&exec, (const char *)value + 4, sizeof(exec));

    if ((load & 0xfff00000u) == 0xfff00000u) {
        unsigned int filetype = (unsigned int)((load >> 8) & 0xfffu);

        if (filetype == 0xfff) {
            return 0;
        }
        return snprintf(suffix, suffix_size, ",%03x", filetype) <
               (int)suffix_size;
    }

    return snprintf(suffix, suffix_size, ",%x-%x", load, exec) <
           (int)suffix_size;
}

static int suffix_to_loadexec(const char *suffix, unsigned char *value,
                              size_t *size)
{
    const char *text = suffix[0] == ',' ? suffix + 1 : suffix;
    const char *dash = strchr(text, '-');
    uint32_t load;
    uint32_t exec;
    uint32_t attr = 0;

    if (!is_metadata_suffix_text(text)) {
        return 0;
    }

    if (dash == NULL) {
        unsigned long filetype = strtoul(text, NULL, 16);

        load = 0xfff00000u | (((uint32_t)filetype & 0xfffu) << 8);
        exec = 0;
    } else {
        load = (uint32_t)strtoul(text, NULL, 16);
        exec = (uint32_t)strtoul(dash + 1, NULL, 16);
    }

    memcpy(value, &load, sizeof(load));
    memcpy(value + 4, &exec, sizeof(exec));
    memcpy(value + 8, &attr, sizeof(attr));
    *size = 12;
    return 1;
}

static int read_xattr_suffix(const char *path, char *suffix,
                             size_t suffix_size)
{
    unsigned char loadexec[12];
    int bytes;

    bytes = host_getxattr(path, RISCOS_LOAD_EXEC_XATTR, loadexec,
                          sizeof(loadexec));
    if (bytes < 0) {
        return 0;
    }

    return loadexec_to_suffix(loadexec, (size_t)bytes, suffix, suffix_size);
}

static int append_metadata_suffix(char *dest, size_t dest_size,
                                  const char *suffix)
{
    size_t len = strlen(dest);
    size_t suffix_len = strlen(suffix);

    if (len + suffix_len >= dest_size) {
        return 0;
    }

    memcpy(dest + len, suffix, suffix_len + 1);
    return 1;
}

static int append_path_component(char *dest, size_t dest_size,
                                 const char *component,
                                 size_t component_len)
{
    size_t len = strlen(dest);

    if (component_len == 0) {
        return 1;
    }

    if (len != 0 && dest[len - 1] != '/') {
        if (len + 1 >= dest_size) {
            return 0;
        }
        dest[len++] = '/';
        dest[len] = '\0';
    }

    if (len + component_len >= dest_size) {
        return 0;
    }

    memcpy(dest + len, component, component_len);
    dest[len + component_len] = '\0';
    return 1;
}

static int join_path(char *dest, size_t dest_size, const char *base,
                     const char *relative)
{
    if (!hideousfs_copy_string(dest, dest_size, base)) {
        return 0;
    }

    return append_path_component(dest, dest_size, relative, strlen(relative));
}

static const char *skip_mount_root(const char *path)
{
    while (*path == '/') {
        ++path;
    }

    return path;
}

static const char *last_path_component(const char *path)
{
    const char *relative = skip_mount_root(path);
    const char *slash = strrchr(relative, '/');

    return slash == NULL ? relative : slash + 1;
}

static const char *previous_path_component(const char *path,
                                           const char *component)
{
    const char *relative = skip_mount_root(path);
    const char *scan = component;

    if (component == relative) {
        return NULL;
    }

    --scan;
    if (scan == relative) {
        return NULL;
    }

    --scan;
    while (scan > relative && scan[-1] != '/') {
        --scan;
    }

    return scan;
}

static int split_extension(const HideousFS_Fuse *fs, const char *leaf,
                           const char **extension, size_t *base_len)
{
    size_t logical_len = strlen(leaf);
    size_t metadata_base_len;
    const char *dot;
    const char *scan;

    if (split_metadata_suffix(leaf, logical_len, &metadata_base_len, NULL)) {
        logical_len = metadata_base_len;
    }

    dot = NULL;
    for (scan = leaf + logical_len; scan > leaf; --scan) {
        if (scan[-1] == '.') {
            dot = scan - 1;
            break;
        }
    }

    if (dot == NULL || dot == leaf || dot + 1 >= leaf + logical_len) {
        return -1;
    }

    if (extension != NULL) {
        *extension = dot + 1;
    }
    if (base_len != NULL) {
        *base_len = (size_t)(dot - leaf);
    }

    return hideousfs_mapped_extension_index(&fs->mapping, dot + 1,
                                            (size_t)((leaf + logical_len) -
                                                     (dot + 1)));
}

static int split_mapped_extension(const HideousFS_Fuse *fs, const char *leaf,
                                  const char **extension, size_t *base_len)
{
    int index = split_extension(fs, leaf, extension, base_len);

    return index >= 0;
}

static int is_mapped_extension_component(const HideousFS_Fuse *fs,
                                         const char *component,
                                         size_t component_len)
{
    return hideousfs_mapped_extension_index(&fs->mapping, component,
                                            component_len) >= 0;
}

static int is_hidden_real_name(const HideousFS_Fuse *fs, const char *leaf,
                               int is_directory)
{
    switch (fs->extension_mode) {
    case fuse_extension_directory:
        return is_directory && hideousfs_is_mapped_extension(&fs->mapping, leaf);

    case fuse_extension_suffix:
        if (is_directory) {
            return hideousfs_is_mapped_extension(&fs->mapping, leaf);
        }
        return split_mapped_extension(fs, leaf, NULL, NULL);

    case fuse_extension_pass:
        return 0;
    }

    return 0;
}

static int is_hidden_presented_path(const HideousFS_Fuse *fs,
                                    const char *path)
{
    const char *leaf;

    if (fs->filetype_mode != fuse_filetypes_xattr) {
        return 0;
    }

    leaf = last_path_component(path);
    return split_metadata_suffix(leaf, strlen(leaf), NULL, NULL);
}

static int ensure_parent_directory(const char *path)
{
    char parent[PATH_MAX];
    char *slash;
    struct stat st;

    if (!hideousfs_copy_string(parent, sizeof(parent), path)) {
        return -ENAMETOOLONG;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        return 0;
    }

    *slash = '\0';
    if (lstat(parent, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
    }
    if (errno != ENOENT) {
        return -errno;
    }

    {
        int error = ensure_parent_directory(parent);

        if (error != 0) {
            return error;
        }
    }

    if (mkdir(parent, 0777) != 0 && errno != EEXIST) {
        return -errno;
    }

    return 0;
}

static int find_comma_suffix_variant(const char *path, char *dest,
                                     size_t dest_size)
{
    char parent[PATH_MAX];
    const char *leaf;
    char *slash;
    DIR *dir;
    struct dirent *entry;
    size_t leaf_len;

    if (!hideousfs_copy_string(parent, sizeof(parent), path)) {
        return 0;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    leaf = slash + 1;
    leaf_len = strlen(leaf);

    dir = opendir(parent);
    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t base_len;
        const char *suffix;

        if (strncmp(entry->d_name, leaf, leaf_len) != 0 ||
            entry->d_name[leaf_len] != ',') {
            continue;
        }
        if (!split_metadata_suffix(entry->d_name, strlen(entry->d_name),
                                   &base_len, &suffix) ||
            base_len != leaf_len) {
            continue;
        }

        closedir(dir);
        if (!hideousfs_copy_string(dest, dest_size, parent)) {
            return 0;
        }
        return append_path_component(dest, dest_size, entry->d_name,
                                     strlen(entry->d_name));
    }

    closedir(dir);
    return 0;
}

static int find_xattr_base_variant(const char *path, char *dest,
                                   size_t dest_size)
{
    char stripped[PATH_MAX];

    if (!strip_metadata_suffix(path, stripped, sizeof(stripped))) {
        return 0;
    }
    if (strcmp(stripped, path) == 0) {
        return 0;
    }
    if (host_getxattr(stripped, RISCOS_LOAD_EXEC_XATTR, NULL, 0) < 0 &&
        errno != ERANGE) {
        return 0;
    }

    return hideousfs_copy_string(dest, dest_size, stripped);
}

static int build_backing_path(const HideousFS_Fuse *fs, const char *path,
                              char *dest, size_t dest_size)
{
    const char *relative = skip_mount_root(path);
    const char *leaf;
    const char *extension;
    const char *prefix_end;
    size_t extension_len;
    size_t prefix_len;
    size_t leaf_len;

    if (*relative == '\0') {
        return hideousfs_copy_string(dest, dest_size, fs->backing_dir);
    }

    if (fs->extension_mode == fuse_extension_pass) {
        return join_path(dest, dest_size, fs->backing_dir, relative);
    }

    leaf = last_path_component(path);

    if (fs->extension_mode == fuse_extension_suffix) {
        size_t base_len;
        const char *metadata_suffix = NULL;

        if (!split_mapped_extension(fs, leaf, &extension, &base_len)) {
            return join_path(dest, dest_size, fs->backing_dir, relative);
        }
        (void)split_metadata_suffix(leaf, strlen(leaf), NULL,
                                    &metadata_suffix);

        prefix_end = leaf == relative ? leaf : leaf - 1;
        prefix_len = (size_t)(prefix_end - relative);
        extension_len = strcspn(extension, ",");

        if (!hideousfs_copy_string(dest, dest_size, fs->backing_dir)) {
            return 0;
        }
        if (!append_path_component(dest, dest_size, relative, prefix_len)) {
            return 0;
        }
        if (!append_path_component(dest, dest_size, extension, extension_len)) {
            return 0;
        }
        if (!append_path_component(dest, dest_size, leaf, base_len)) {
            return 0;
        }
        if (metadata_suffix != NULL) {
            return append_metadata_suffix(dest, dest_size, metadata_suffix);
        }
        return 1;
    }

    extension = previous_path_component(path, leaf);
    if (extension == NULL) {
        return join_path(dest, dest_size, fs->backing_dir, relative);
    }

    extension_len = (size_t)((leaf - 1) - extension);
    if (!is_mapped_extension_component(fs, extension, extension_len)) {
        return join_path(dest, dest_size, fs->backing_dir, relative);
    }

    prefix_end = extension == relative ? extension : extension - 1;
    prefix_len = (size_t)(prefix_end - relative);
    {
        size_t metadata_base_len;

        leaf_len = strlen(leaf);
        if (split_metadata_suffix(leaf, leaf_len, &metadata_base_len, NULL)) {
            leaf_len = metadata_base_len;
        }
    }

    if (!hideousfs_copy_string(dest, dest_size, fs->backing_dir)) {
        return 0;
    }
    if (!append_path_component(dest, dest_size, relative, prefix_len)) {
        return 0;
    }

    if (strlen(dest) + 1 + leaf_len + 1 + extension_len >= dest_size) {
        return 0;
    }
    append_path_component(dest, dest_size, leaf, leaf_len);
    strcat(dest, ".");
    strncat(dest, extension, extension_len);
    {
        const char *metadata_suffix = NULL;

        if (split_metadata_suffix(leaf, strlen(leaf), NULL,
                                  &metadata_suffix)) {
            return append_metadata_suffix(dest, dest_size, metadata_suffix);
        }
    }
    return 1;
}

static int resolve_existing_backing_path(const HideousFS_Fuse *fs,
                                         const char *path,
                                         char *dest, size_t dest_size)
{
    if (!build_backing_path(fs, path, dest, dest_size)) {
        return 0;
    }
    if (lstat(dest, &(struct stat){0}) == 0) {
        return 1;
    }

    if (fs->filetype_mode == fuse_filetypes_xattr &&
        find_comma_suffix_variant(dest, dest, dest_size)) {
        return 1;
    }

    if (fs->filetype_mode == fuse_filetypes_suffix &&
        find_xattr_base_variant(dest, dest, dest_size)) {
        return 1;
    }

    return 1;
}

static int build_presented_leaf(const HideousFS_Fuse *fs,
                                const char *backing_dir,
                                const char *leaf,
                                char *dest, size_t dest_size)
{
    char path[PATH_MAX];
    char suffix[MaxMetadataSuffix];
    size_t base_len;
    const char *metadata_suffix;

    switch (fs->filetype_mode) {
    case fuse_filetypes_pass:
        return hideousfs_copy_string(dest, dest_size, leaf);

    case fuse_filetypes_suffix:
        if (split_metadata_suffix(leaf, strlen(leaf), NULL, NULL)) {
            return hideousfs_copy_string(dest, dest_size, leaf);
        }
        if (!join_path(path, sizeof(path), backing_dir, leaf)) {
            return 0;
        }
        if (!hideousfs_copy_string(dest, dest_size, leaf)) {
            return 0;
        }
        if (read_xattr_suffix(path, suffix, sizeof(suffix))) {
            return append_metadata_suffix(dest, dest_size, suffix);
        }
        return 1;

    case fuse_filetypes_xattr:
        if (split_metadata_suffix(leaf, strlen(leaf), &base_len,
                                  &metadata_suffix)) {
            if (base_len >= dest_size) {
                return 0;
            }
            memcpy(dest, leaf, base_len);
            dest[base_len] = '\0';
            return 1;
        }
        return hideousfs_copy_string(dest, dest_size, leaf);
    }

    return 0;
}

static int resolve_directory_path(const HideousFS_Fuse *fs, const char *path,
                                  char *dest, size_t dest_size,
                                  char *synthetic_extension,
                                  size_t synthetic_extension_size)
{
    const char *relative = skip_mount_root(path);
    const char *component;
    const char *prefix_end;
    size_t extension_len;

    synthetic_extension[0] = '\0';

    if (*relative == '\0') {
        return hideousfs_copy_string(dest, dest_size, fs->backing_dir);
    }

    if (fs->extension_mode == fuse_extension_pass) {
        return join_path(dest, dest_size, fs->backing_dir, relative);
    }
    if (fs->extension_mode == fuse_extension_suffix) {
        return build_backing_path(fs, path, dest, dest_size);
    }

    component = last_path_component(path);
    extension_len = strlen(component);
    if (is_mapped_extension_component(fs, component, extension_len)) {
        if (extension_len >= synthetic_extension_size) {
            return 0;
        }
        memcpy(synthetic_extension, component, extension_len + 1);

        if (component == relative) {
            return hideousfs_copy_string(dest, dest_size, fs->backing_dir);
        }

        prefix_end = component - 1;
        if (!hideousfs_copy_string(dest, dest_size, fs->backing_dir)) {
            return 0;
        }
        return append_path_component(dest, dest_size, relative,
                                     (size_t)(prefix_end - relative));
    }

    return join_path(dest, dest_size, fs->backing_dir, relative);
}

static int directory_contains_extension(const HideousFS_Fuse *fs,
                                        const char *backing_dir,
                                        const char *extension)
{
    DIR *dir;
    struct dirent *entry;
    const char *entry_extension;

    dir = opendir(backing_dir);
    if (dir == NULL) {
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL) {
        int extension_index = split_extension(fs, entry->d_name,
                                              &entry_extension, NULL);

        if (extension_index >= 0 &&
            strcmp(fs->mapping.reverse_extensions[extension_index],
                   extension) == 0) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static int fill_synthetic_dir_attr(HideousFS_FuseAttr *attr)
{
    struct stat st;

    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR | 0777;
    st.st_nlink = 2;
    stat_to_fuse_attr(&st, attr);
    return 0;
}

static int add_readdir_entry(void *buf, HideousFS_FuseFillDir filler,
                             const char *name, const HideousFS_FuseAttr *attr,
                             off_t offset)
{
    return filler(buf, name, attr, offset, 0) == 0 ? 0 : -ENOMEM;
}

static int read_projected_directory(const HideousFS_Fuse *fs,
                                    const char *backing_dir, void *buf,
                                    HideousFS_FuseFillDir filler, off_t offset,
                                    enum fuse_readdir_flags flags)
{
    DIR *dir;
    struct dirent *entry;
    unsigned int seen_extensions = 0;
    off_t index = 0;

    (void)flags;

    dir = opendir(backing_dir);
    if (dir == NULL) {
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL) {
        int extension_index;
        char presented_leaf[PATH_MAX];
        const char *projected_name = presented_leaf;
        char path[PATH_MAX];
        struct stat raw_st;
        HideousFS_FuseAttr attr;
        int synthetic = 0;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (!join_path(path, sizeof(path), backing_dir, entry->d_name)) {
            closedir(dir);
            return -ENAMETOOLONG;
        }
        if (lstat(path, &raw_st) != 0) {
            closedir(dir);
            return -errno;
        }

        if (!build_presented_leaf(fs, backing_dir, entry->d_name,
                                  presented_leaf, sizeof(presented_leaf))) {
            closedir(dir);
            return -ENAMETOOLONG;
        }

        if (is_hidden_real_name(fs, presented_leaf, S_ISDIR(raw_st.st_mode))) {
            continue;
        }

        extension_index = split_extension(fs, presented_leaf, NULL, NULL);
        if (fs->extension_mode == fuse_extension_directory &&
            extension_index >= 0) {
            if ((seen_extensions & (1u << extension_index)) != 0) {
                continue;
            }
            seen_extensions |= 1u << extension_index;
            projected_name = fs->mapping.reverse_extensions[extension_index];
            synthetic = 1;
        }

        if (index++ < offset) {
            continue;
        }

        memset(&attr, 0, sizeof(attr));
        if (synthetic) {
            fill_synthetic_dir_attr(&attr);
        } else {
            stat_to_fuse_attr(&raw_st, &attr);
        }

        if (add_readdir_entry(buf, filler, projected_name, &attr, index) != 0) {
            closedir(dir);
            return -ENOMEM;
        }
    }

    closedir(dir);
    return 0;
}

static int read_synthetic_directory(const HideousFS_Fuse *fs,
                                    const char *backing_dir,
                                    const char *extension, void *buf,
                                    HideousFS_FuseFillDir filler, off_t offset,
                                    enum fuse_readdir_flags flags)
{
    DIR *dir;
    struct dirent *entry;
    off_t index = 0;

    (void)flags;

    dir = opendir(backing_dir);
    if (dir == NULL) {
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t base_len;
        char presented_leaf[PATH_MAX];
        char projected_name[PATH_MAX];
        char path[PATH_MAX];
        HideousFS_FuseAttr attr;
        int error;

        if (!build_presented_leaf(fs, backing_dir, entry->d_name,
                                  presented_leaf, sizeof(presented_leaf))) {
            closedir(dir);
            return -ENAMETOOLONG;
        }

        {
            int extension_index = split_extension(fs, presented_leaf,
                                                  NULL, &base_len);

            if (extension_index < 0 ||
                strcmp(fs->mapping.reverse_extensions[extension_index],
                       extension) != 0) {
                continue;
            }
        }

        {
            const char *metadata_suffix = NULL;

            (void)split_metadata_suffix(presented_leaf, strlen(presented_leaf),
                                        NULL, &metadata_suffix);
            if (base_len >= sizeof(projected_name)) {
                closedir(dir);
                return -ENAMETOOLONG;
            }
            memcpy(projected_name, presented_leaf, base_len);
            projected_name[base_len] = '\0';
            if (metadata_suffix != NULL &&
                !append_metadata_suffix(projected_name, sizeof(projected_name),
                                        metadata_suffix)) {
                closedir(dir);
                return -ENAMETOOLONG;
            }
        }

        if (index++ < offset) {
            continue;
        }

        if (!join_path(path, sizeof(path), backing_dir, entry->d_name)) {
            closedir(dir);
            return -ENAMETOOLONG;
        }
        error = lstat_fuse_attr(path, &attr);
        if (error != 0) {
            closedir(dir);
            return error;
        }

        if (add_readdir_entry(buf, filler, projected_name, &attr, index) != 0) {
            closedir(dir);
            return -ENOMEM;
        }
    }

    closedir(dir);
    return 0;
}

static int read_suffix_projected_directory(const HideousFS_Fuse *fs,
                                           const char *backing_dir,
                                           void *buf,
                                           HideousFS_FuseFillDir filler,
                                           off_t offset,
                                           enum fuse_readdir_flags flags)
{
    DIR *dir;
    struct dirent *entry;
    off_t index = 0;

    (void)flags;

    dir = opendir(backing_dir);
    if (dir == NULL) {
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat raw_st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!join_path(path, sizeof(path), backing_dir, entry->d_name)) {
            closedir(dir);
            return -ENAMETOOLONG;
        }
        if (lstat(path, &raw_st) != 0) {
            closedir(dir);
            return -errno;
        }

        if (S_ISDIR(raw_st.st_mode) &&
            hideousfs_is_mapped_extension(&fs->mapping, entry->d_name)) {
            DIR *bucket;
            struct dirent *child;

            bucket = opendir(path);
            if (bucket == NULL) {
                closedir(dir);
                return -errno;
            }

            while ((child = readdir(bucket)) != NULL) {
                char child_path[PATH_MAX];
                char presented_leaf[PATH_MAX];
                char projected_name[PATH_MAX];
                HideousFS_FuseAttr attr;
                size_t child_len;
                size_t extension_len;
                size_t logical_child_len;
                const char *metadata_suffix = NULL;
                int error;

                if (strcmp(child->d_name, ".") == 0 ||
                    strcmp(child->d_name, "..") == 0) {
                    continue;
                }

                if (!build_presented_leaf(fs, path, child->d_name,
                                          presented_leaf,
                                          sizeof(presented_leaf))) {
                    closedir(bucket);
                    closedir(dir);
                    return -ENAMETOOLONG;
                }

                child_len = strlen(presented_leaf);
                logical_child_len = child_len;
                if (split_metadata_suffix(presented_leaf, child_len,
                                          &logical_child_len,
                                          &metadata_suffix)) {
                    child_len = logical_child_len;
                }
                extension_len = strlen(entry->d_name);
                if (strlen(presented_leaf) + 1 + extension_len >=
                    sizeof(projected_name)) {
                    closedir(bucket);
                    closedir(dir);
                    return -ENAMETOOLONG;
                }

                memcpy(projected_name, presented_leaf, child_len);
                projected_name[child_len] = '.';
                memcpy(projected_name + child_len + 1, entry->d_name,
                       extension_len + 1);
                if (metadata_suffix != NULL &&
                    !append_metadata_suffix(projected_name,
                                            sizeof(projected_name),
                                            metadata_suffix)) {
                    closedir(bucket);
                    closedir(dir);
                    return -ENAMETOOLONG;
                }

                if (index++ < offset) {
                    continue;
                }

                if (!join_path(child_path, sizeof(child_path), path,
                               child->d_name)) {
                    closedir(bucket);
                    closedir(dir);
                    return -ENAMETOOLONG;
                }
                error = lstat_fuse_attr(child_path, &attr);
                if (error != 0) {
                    closedir(bucket);
                    closedir(dir);
                    return error;
                }
                if (add_readdir_entry(buf, filler, projected_name, &attr,
                                      index) != 0) {
                    closedir(bucket);
                    closedir(dir);
                    return -ENOMEM;
                }
            }

            closedir(bucket);
            continue;
        }

        if (is_hidden_real_name(fs, entry->d_name, S_ISDIR(raw_st.st_mode))) {
            continue;
        }

        if (index++ < offset) {
            continue;
        }

        {
            HideousFS_FuseAttr attr;

            stat_to_fuse_attr(&raw_st, &attr);
            if (add_readdir_entry(buf, filler, entry->d_name, &attr,
                                  index) != 0) {
                closedir(dir);
                return -ENOMEM;
            }
        }
    }

    closedir(dir);
    return 0;
}

static int hideousfs_fuse_getattr(const char *path, HideousFS_FuseAttr *attr,
                                  struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    char synthetic_extension[MaxExtensionLen];
    struct stat raw_st;
    int found;

    (void)fi;

    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    if (!resolve_directory_path(fs, path, backing_path, sizeof(backing_path),
                                synthetic_extension,
                                sizeof(synthetic_extension))) {
        return -ENAMETOOLONG;
    }

    if (synthetic_extension[0] != '\0') {
        found = directory_contains_extension(fs, backing_path,
                                             synthetic_extension);
        if (found < 0) {
            return found;
        }
        (void)found;
        return fill_synthetic_dir_attr(attr);
    }

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    if (lstat(backing_path, &raw_st) != 0) {
        return -errno;
    }
    if (!(fs->extension_mode == fuse_extension_suffix &&
          split_mapped_extension(fs, last_path_component(path), NULL, NULL)) &&
        is_hidden_real_name(fs, last_path_component(path),
                            S_ISDIR(raw_st.st_mode))) {
        return -ENOENT;
    }

    stat_to_fuse_attr(&raw_st, attr);
    return 0;
}

static int hideousfs_fuse_readdir(const char *path, void *buf,
                                  HideousFS_FuseFillDir filler, off_t offset,
                                  struct fuse_file_info *fi,
                                  enum fuse_readdir_flags flags)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    char synthetic_extension[MaxExtensionLen];
    int found;
    int error;

    (void)fi;

    if (offset == 0) {
        if (add_readdir_entry(buf, filler, ".", NULL, 1) != 0 ||
            add_readdir_entry(buf, filler, "..", NULL, 2) != 0) {
            return -ENOMEM;
        }
    }

    if (!resolve_directory_path(fs, path, backing_path, sizeof(backing_path),
                                synthetic_extension,
                                sizeof(synthetic_extension))) {
        return -ENAMETOOLONG;
    }

    if (synthetic_extension[0] != '\0') {
        found = directory_contains_extension(fs, backing_path,
                                             synthetic_extension);
        if (found < 0) {
            return found;
        }
        (void)found;
        error = read_synthetic_directory(fs, backing_path, synthetic_extension,
                                         buf, filler, offset, flags);
    } else {
        if (fs->extension_mode == fuse_extension_suffix) {
            error = read_suffix_projected_directory(fs, backing_path, buf,
                                                    filler, offset, flags);
        } else {
            error = read_projected_directory(fs, backing_path, buf, filler,
                                             offset, flags);
        }
    }

    return error;
}

static int hideousfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    int fd;

    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    fd = open(backing_path, fi->flags);
    if (fd < 0) {
        return -errno;
    }

    fi->fh = (uint64_t)fd;
    return 0;
}

static int hideousfs_fuse_create(const char *path, mode_t mode,
                                 struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    int fd;

    if (fs->readonly) {
        return -EROFS;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    {
        int error = ensure_parent_directory(backing_path);

        if (error != 0) {
            return error;
        }
    }

    fd = open(backing_path, fi->flags | O_CREAT, mode);
    if (fd < 0) {
        return -errno;
    }

    fi->fh = (uint64_t)fd;
    return 0;
}

static int hideousfs_fuse_read(const char *path, char *buf, size_t size,
                               off_t offset, struct fuse_file_info *fi)
{
    ssize_t bytes;

    (void)path;

    bytes = pread((int)fi->fh, buf, size, offset);
    if (bytes < 0) {
        return -errno;
    }
    return (int)bytes;
}

static int hideousfs_fuse_write(const char *path, const char *buf, size_t size,
                                off_t offset, struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    ssize_t bytes;

    (void)path;

    if (fs->readonly) {
        return -EROFS;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    bytes = pwrite((int)fi->fh, buf, size, offset);
    if (bytes < 0) {
        return -errno;
    }
    return (int)bytes;
}

static int hideousfs_fuse_truncate(const char *path, off_t size,
                                   struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];

    if (fs->readonly) {
        return -EROFS;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    if (fi != NULL) {
        return ftruncate((int)fi->fh, size) == 0 ? 0 : -errno;
    }

    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    return truncate(backing_path, size) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_unlink(const char *path)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];

    if (fs->readonly) {
        return -EROFS;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }
    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    return unlink(backing_path) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_mkdir(const char *path, mode_t mode)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];

    if (fs->readonly) {
        return -EROFS;
    }
    if (is_hidden_presented_path(fs, path)) {
        return -ENOENT;
    }
    if (is_hidden_real_name(fs, last_path_component(path), 1)) {
        return -EEXIST;
    }
    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    return mkdir(backing_path, mode) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_rmdir(const char *path)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    char synthetic_extension[MaxExtensionLen];

    if (fs->readonly) {
        return -EROFS;
    }
    if (!resolve_directory_path(fs, path, backing_path, sizeof(backing_path),
                                synthetic_extension,
                                sizeof(synthetic_extension))) {
        return -ENAMETOOLONG;
    }
    if (synthetic_extension[0] != '\0') {
        return -EROFS;
    }
    return rmdir(backing_path) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_rename(const char *from, const char *to,
                                 unsigned int flags)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_from[PATH_MAX];
    char backing_to[PATH_MAX];

    if (fs->readonly) {
        return -EROFS;
    }
    if (flags != 0) {
        return -EINVAL;
    }
    if (is_hidden_presented_path(fs, from) ||
        is_hidden_presented_path(fs, to)) {
        return -ENOENT;
    }
    if (!resolve_existing_backing_path(fs, from, backing_from,
                                       sizeof(backing_from)) ||
        !build_backing_path(fs, to, backing_to, sizeof(backing_to))) {
        return -ENAMETOOLONG;
    }
    {
        int error = ensure_parent_directory(backing_to);

        if (error != 0) {
            return error;
        }
    }
    return rename(backing_from, backing_to) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    return close((int)fi->fh) == 0 ? 0 : -errno;
}

static int hideousfs_fuse_fsync(const char *path, int datasync,
                                struct fuse_file_info *fi)
{
    int result;

    (void)path;

#ifdef __APPLE__
    (void)datasync;
    result = fsync((int)fi->fh);
#else
    result = datasync ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
#endif

    return result == 0 ? 0 : -errno;
}

#ifdef __APPLE__
static int hideousfs_fuse_setxattr(const char *path, const char *name,
                                   const char *value, size_t size, int flags,
                                   uint32_t position)
#else
static int hideousfs_fuse_setxattr(const char *path, const char *name,
                                   const char *value, size_t size, int flags)
#endif
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];

#ifdef __APPLE__
    if (position != 0) {
        return -EINVAL;
    }
#endif
    if (fs->readonly) {
        return -EROFS;
    }
    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    return host_setxattr(backing_path, name, value, size, flags) == 0 ?
           0 : -errno;
}

#ifdef __APPLE__
static int hideousfs_fuse_getxattr(const char *path, const char *name,
                                   char *value, size_t size,
                                   uint32_t position)
#else
static int hideousfs_fuse_getxattr(const char *path, const char *name,
                                   char *value, size_t size)
#endif
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    const char *leaf;
    const char *suffix;
    unsigned char loadexec[12];
    size_t loadexec_size;
    int bytes;

#ifdef __APPLE__
    if (position != 0) {
        return -EINVAL;
    }
#endif

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    bytes = host_getxattr(backing_path, name, value, size);
    if (bytes >= 0) {
        return bytes;
    }

    if (fs->filetype_mode != fuse_filetypes_xattr ||
        strcmp(name, RISCOS_LOAD_EXEC_XATTR) != 0) {
        return -errno;
    }

    leaf = last_path_component(backing_path);
    if (!split_metadata_suffix(leaf, strlen(leaf), NULL, &suffix) ||
        !suffix_to_loadexec(suffix, loadexec, &loadexec_size)) {
        return -errno;
    }

    if (size == 0) {
        return (int)loadexec_size;
    }
    if (size < loadexec_size) {
        return -ERANGE;
    }

    memcpy(value, loadexec, loadexec_size);
    return (int)loadexec_size;
}

static int hideousfs_fuse_listxattr(const char *path, char *list, size_t size)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    const char *leaf;
    const char *suffix;
    int bytes;
    size_t required;
    size_t name_len = sizeof(RISCOS_LOAD_EXEC_XATTR);
    int has_riscos_xattr = 0;

    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    bytes = host_listxattr(backing_path, list, size);
    if (bytes < 0) {
        return -errno;
    }

    if (fs->filetype_mode != fuse_filetypes_xattr) {
        return bytes;
    }

    leaf = last_path_component(backing_path);
    if (!split_metadata_suffix(leaf, strlen(leaf), NULL, &suffix)) {
        return bytes;
    }

    if (size != 0) {
        size_t offset = 0;

        while (offset < (size_t)bytes) {
            if (strcmp(list + offset, RISCOS_LOAD_EXEC_XATTR) == 0) {
                has_riscos_xattr = 1;
                break;
            }
            offset += strlen(list + offset) + 1;
        }
    }

    if (has_riscos_xattr) {
        return bytes;
    }

    required = (size_t)bytes + name_len;
    if (size == 0) {
        return (int)required;
    }
    if (size < required) {
        return -ERANGE;
    }

    memcpy(list + bytes, RISCOS_LOAD_EXEC_XATTR, name_len);
    return (int)required;
}

static int hideousfs_fuse_removexattr(const char *path, const char *name)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];

    if (fs->readonly) {
        return -EROFS;
    }
    if (!resolve_existing_backing_path(fs, path, backing_path,
                                       sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }

    return host_removexattr(backing_path, name) == 0 ? 0 : -errno;
}

static const struct fuse_operations hideousfs_fuse_ops = {
    .getattr = hideousfs_fuse_getattr,
    .readdir = hideousfs_fuse_readdir,
    .open = hideousfs_fuse_open,
    .create = hideousfs_fuse_create,
    .read = hideousfs_fuse_read,
    .write = hideousfs_fuse_write,
    .truncate = hideousfs_fuse_truncate,
    .unlink = hideousfs_fuse_unlink,
    .mkdir = hideousfs_fuse_mkdir,
    .rmdir = hideousfs_fuse_rmdir,
    .rename = hideousfs_fuse_rename,
    .release = hideousfs_fuse_release,
    .fsync = hideousfs_fuse_fsync,
    .setxattr = hideousfs_fuse_setxattr,
    .getxattr = hideousfs_fuse_getxattr,
    .listxattr = hideousfs_fuse_listxattr,
    .removexattr = hideousfs_fuse_removexattr
};

static int parse_args(int argc, char **argv, HideousFS_Fuse *fs,
                      char ***fuse_argv_out, int *fuse_argc_out,
                      int *run_selftest)
{
    char **fuse_argv;
    int fuse_argc = 0;
    int backing_seen = 0;
    int mount_seen = 0;
    int i;

    fuse_argv = calloc((size_t)argc + 1, sizeof(*fuse_argv));
    if (fuse_argv == NULL) {
        return -ENOMEM;
    }

    fuse_argv[fuse_argc++] = argv[0];
    *run_selftest = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--extension=directory") == 0) {
            fs->extension_mode = fuse_extension_directory;
        } else if (strcmp(argv[i], "--extension=suffix") == 0) {
            fs->extension_mode = fuse_extension_suffix;
        } else if (strcmp(argv[i], "--extension=pass") == 0) {
            fs->extension_mode = fuse_extension_pass;
        } else if (strcmp(argv[i], "--filetypes=pass") == 0) {
            fs->filetype_mode = fuse_filetypes_pass;
        } else if (strcmp(argv[i], "--filetypes=suffix") == 0) {
            fs->filetype_mode = fuse_filetypes_suffix;
        } else if (strcmp(argv[i], "--filetypes=xattr") == 0) {
            fs->filetype_mode = fuse_filetypes_xattr;
        } else if (strcmp(argv[i], "--readonly") == 0) {
            fs->readonly = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            fuse_argv[fuse_argc++] = "-f";
        } else if (strcmp(argv[i], "--debug") == 0) {
            fuse_argv[fuse_argc++] = "-d";
        } else if (strncmp(argv[i], "--reverse=", 10) == 0) {
            if (!parse_reverse_list(fs, argv[i] + 10)) {
                free(fuse_argv);
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--selftest") == 0) {
            *run_selftest = 1;
        } else if (argv[i][0] == '-') {
            fuse_argv[fuse_argc++] = argv[i];
        } else if (!backing_seen) {
            if (realpath(argv[i], fs->backing_dir) == NULL) {
                free(fuse_argv);
                return -errno;
            }
            backing_seen = 1;
        } else if (!mount_seen) {
            fuse_argv[fuse_argc++] = argv[i];
            mount_seen = 1;
        } else {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }

    if (*run_selftest) {
        *fuse_argv_out = fuse_argv;
        *fuse_argc_out = fuse_argc;
        return 0;
    }

    if (!backing_seen || !mount_seen) {
        fprintf(stderr, "usage: %s [options] <backing-directory> <mount-point>\n",
                argv[0]);
        fprintf(stderr, "options: --extension=directory|suffix|pass --filetypes=pass --reverse=a,b,c --readonly --foreground --debug\n");
        free(fuse_argv);
        return -EINVAL;
    }

    *fuse_argv_out = fuse_argv;
    *fuse_argc_out = fuse_argc;
    return 0;
}

static int expect_path(const HideousFS_Fuse *fs, const char *view,
                       const char *expected)
{
    char path[PATH_MAX];

    if (!build_backing_path(fs, view, path, sizeof(path))) {
        fprintf(stderr, "selftest: mapping failed for %s\n", view);
        return 0;
    }
    if (strcmp(path, expected) != 0) {
        fprintf(stderr, "selftest: %s mapped to %s, expected %s\n",
                view, path, expected);
        return 0;
    }
    return 1;
}

static int run_selftest(void)
{
    HideousFS_Fuse fs;
    int ok = 1;

    memset(&fs, 0, sizeof(fs));
    hideousfs_initialise_default_config(&fs.mapping);
    fs.extension_mode = fuse_extension_directory;
    fs.filetype_mode = fuse_filetypes_pass;
    strcpy(fs.backing_dir, "/tmp/hideousfs-backing");

    ok = expect_path(&fs, "/", "/tmp/hideousfs-backing") && ok;
    ok = expect_path(&fs, "/Readme", "/tmp/hideousfs-backing/Readme") && ok;
    ok = expect_path(&fs, "/c/leaf", "/tmp/hideousfs-backing/leaf.c") && ok;
    ok = expect_path(&fs, "/src/h/header", "/tmp/hideousfs-backing/src/header.h") && ok;
    ok = expect_path(&fs, "/src/doc/Readme", "/tmp/hideousfs-backing/src/doc/Readme") && ok;

    fs.extension_mode = fuse_extension_suffix;
    ok = expect_path(&fs, "/", "/tmp/hideousfs-backing") && ok;
    ok = expect_path(&fs, "/Readme", "/tmp/hideousfs-backing/Readme") && ok;
    ok = expect_path(&fs, "/leaf.c", "/tmp/hideousfs-backing/c/leaf") && ok;
    ok = expect_path(&fs, "/src/header.h", "/tmp/hideousfs-backing/src/h/header") && ok;
    ok = expect_path(&fs, "/src/doc/Readme", "/tmp/hideousfs-backing/src/doc/Readme") && ok;

    fs.extension_mode = fuse_extension_pass;
    ok = expect_path(&fs, "/leaf.c", "/tmp/hideousfs-backing/leaf.c") && ok;
    ok = expect_path(&fs, "/c/leaf", "/tmp/hideousfs-backing/c/leaf") && ok;

    if (!ok) {
        return 1;
    }

    printf("HideousFS FUSE selftest passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    HideousFS_Fuse fs;
    char **fuse_argv;
    int fuse_argc;
    int run_tests;
    int error;

    memset(&fs, 0, sizeof(fs));
    hideousfs_initialise_default_config(&fs.mapping);
    fs.extension_mode = fuse_extension_directory;
    fs.filetype_mode = fuse_filetypes_pass;

    error = parse_args(argc, argv, &fs, &fuse_argv, &fuse_argc, &run_tests);
    if (error != 0) {
        return 1;
    }

    if (run_tests) {
        free(fuse_argv);
        return run_selftest();
    }

    error = fuse_main(fuse_argc, fuse_argv, &hideousfs_fuse_ops, &fs);
    free(fuse_argv);
    return error;
}
