#define FUSE_USE_VERSION 31

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <fuse.h>

#include "HideousFS.h"

typedef enum HideousFS_FuseExtensionMode {
    fuse_extension_directory,
    fuse_extension_pass
} HideousFS_FuseExtensionMode;

typedef struct HideousFS_Fuse {
    char backing_dir[PATH_MAX];
    HideousFS_FuseExtensionMode extension_mode;
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

static void stat_to_fuse_attr(const struct stat *st, HideousFS_FuseAttr *attr);
static int lstat_fuse_attr(const char *path, HideousFS_FuseAttr *attr);
static int parse_reverse_list(HideousFS_Fuse *fs, const char *list);
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
static int is_mapped_extension_component(const HideousFS_Fuse *fs,
                                         const char *component,
                                         size_t component_len);
static int is_hidden_real_name(const HideousFS_Fuse *fs, const char *leaf);
static int build_backing_path(const HideousFS_Fuse *fs, const char *path,
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
    const char *dot = strrchr(leaf, '.');

    if (dot == NULL || dot == leaf || dot[1] == '\0') {
        return -1;
    }

    if (extension != NULL) {
        *extension = dot + 1;
    }
    if (base_len != NULL) {
        *base_len = (size_t)(dot - leaf);
    }

    return hideousfs_mapped_extension_index(&fs->mapping, dot + 1,
                                            strlen(dot + 1));
}

static int is_mapped_extension_component(const HideousFS_Fuse *fs,
                                         const char *component,
                                         size_t component_len)
{
    return hideousfs_mapped_extension_index(&fs->mapping, component,
                                            component_len) >= 0;
}

static int is_hidden_real_name(const HideousFS_Fuse *fs, const char *leaf)
{
    if (fs->extension_mode != fuse_extension_directory) {
        return 0;
    }

    return hideousfs_is_mapped_extension(&fs->mapping, leaf);
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
    leaf_len = strlen(leaf);

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
    return 1;
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
        if (split_extension(fs, entry->d_name, &entry_extension, NULL) >= 0 &&
            strcmp(entry_extension, extension) == 0) {
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
        const char *extension;
        int extension_index;
        const char *projected_name = entry->d_name;
        HideousFS_FuseAttr attr;
        int synthetic = 0;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (fs->extension_mode == fuse_extension_directory &&
            is_hidden_real_name(fs, entry->d_name)) {
            continue;
        }

        extension_index = split_extension(fs, entry->d_name, &extension, NULL);
        if (fs->extension_mode == fuse_extension_directory &&
            extension_index >= 0) {
            if ((seen_extensions & (1u << extension_index)) != 0) {
                continue;
            }
            seen_extensions |= 1u << extension_index;
            projected_name = extension;
            synthetic = 1;
        }

        if (index++ < offset) {
            continue;
        }

        memset(&attr, 0, sizeof(attr));
        if (synthetic) {
            fill_synthetic_dir_attr(&attr);
        } else {
            char path[PATH_MAX];
            int error;

            if (!join_path(path, sizeof(path), backing_dir, entry->d_name)) {
                closedir(dir);
                return -ENAMETOOLONG;
            }
            error = lstat_fuse_attr(path, &attr);
            if (error != 0) {
                closedir(dir);
                return error;
            }
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
        const char *entry_extension;
        size_t base_len;
        char projected_name[PATH_MAX];
        char path[PATH_MAX];
        HideousFS_FuseAttr attr;
        int error;

        if (split_extension(fs, entry->d_name, &entry_extension, &base_len) < 0 ||
            strcmp(entry_extension, extension) != 0) {
            continue;
        }

        if (base_len >= sizeof(projected_name)) {
            closedir(dir);
            return -ENAMETOOLONG;
        }
        memcpy(projected_name, entry->d_name, base_len);
        projected_name[base_len] = '\0';

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

static int hideousfs_fuse_getattr(const char *path, HideousFS_FuseAttr *attr,
                                  struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    char synthetic_extension[MaxExtensionLen];
    int found;

    (void)fi;

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

    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    if (is_hidden_real_name(fs, last_path_component(path))) {
        return -ENOENT;
    }

    return lstat_fuse_attr(backing_path, attr);
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
        error = read_projected_directory(fs, backing_path, buf, filler,
                                         offset, flags);
    }

    return error;
}

static int hideousfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    HideousFS_Fuse *fs = (HideousFS_Fuse *)fuse_get_context()->private_data;
    char backing_path[PATH_MAX];
    int fd;

    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    if (is_hidden_real_name(fs, last_path_component(path))) {
        return -ENOENT;
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
    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
        return -ENAMETOOLONG;
    }
    if (is_hidden_real_name(fs, last_path_component(path))) {
        return -ENOENT;
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

    if (fi != NULL) {
        return ftruncate((int)fi->fh, size) == 0 ? 0 : -errno;
    }

    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
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
    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
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
    if (is_hidden_real_name(fs, last_path_component(path))) {
        return -EEXIST;
    }
    if (!build_backing_path(fs, path, backing_path, sizeof(backing_path))) {
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
    if (!build_backing_path(fs, from, backing_from, sizeof(backing_from)) ||
        !build_backing_path(fs, to, backing_to, sizeof(backing_to))) {
        return -ENAMETOOLONG;
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
    .fsync = hideousfs_fuse_fsync
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
        } else if (strcmp(argv[i], "--extension=pass") == 0) {
            fs->extension_mode = fuse_extension_pass;
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
        fprintf(stderr, "options: --extension=directory|pass --reverse=a,b,c --readonly --foreground --debug\n");
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
    strcpy(fs.backing_dir, "/tmp/hideousfs-backing");

    ok = expect_path(&fs, "/", "/tmp/hideousfs-backing") && ok;
    ok = expect_path(&fs, "/Readme", "/tmp/hideousfs-backing/Readme") && ok;
    ok = expect_path(&fs, "/c/leaf", "/tmp/hideousfs-backing/leaf.c") && ok;
    ok = expect_path(&fs, "/src/h/header", "/tmp/hideousfs-backing/src/header.h") && ok;
    ok = expect_path(&fs, "/src/doc/Readme", "/tmp/hideousfs-backing/src/doc/Readme") && ok;

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
