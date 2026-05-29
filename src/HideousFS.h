#ifndef HIDEOUSFS_H
#define HIDEOUSFS_H

#include <stddef.h>

typedef unsigned int word;

enum {
    MaxPath = 768,
    TempBufferSize = 1024,
    MaxReverseExtensions = 16,
    MaxExtensionLen = 16,
    MaxImages = 16,
    MaxOpenFiles = 32
};

typedef struct HideousFS_Image {
    int in_use;
    word fileswitch_handle;
    word buffer_size;
    int beautiful_mode;
    int reverse_extension_count;
    char reverse_extensions[MaxReverseExtensions][MaxExtensionLen];
    char image_path[MaxPath];
    char backing_dir[MaxPath];
    char image_leaf[MaxPath];
} HideousFS_Image;

int hideousfs_mapped_extension_index(const HideousFS_Image *image,
                                     const char *name, size_t len);
int hideousfs_is_mapped_extension(const HideousFS_Image *image,
                                  const char *name);
int hideousfs_is_root_name(const char *name);
int hideousfs_copy_string(char *dest, size_t dest_size, const char *src);
const char *hideousfs_skip_root_prefix(const char *name);
int hideousfs_path_has_mapped_component(const HideousFS_Image *image,
                                        const char *name);
int hideousfs_append_relative_to_backing(const HideousFS_Image *image,
                                         const char *relative,
                                         char *dest, size_t dest_size);
const char *hideousfs_last_component(const char *name);
int hideousfs_is_projected_object_name(const HideousFS_Image *image,
                                       const char *name);
int hideousfs_build_backing_object_path(const HideousFS_Image *image,
                                        const char *name,
                                        char *dest, size_t dest_size);
int hideousfs_resolve_directory_path(const HideousFS_Image *image,
                                     const char *name,
                                     char *dest, size_t dest_size,
                                     char *synthetic_extension,
                                     size_t synthetic_extension_size);
int hideousfs_is_active_image_entry(const HideousFS_Image *image,
                                    const char *dir, const char *leaf);
int hideousfs_is_active_image_path(const HideousFS_Image *image,
                                   const char *name);
int hideousfs_host_leaf_extension_index(const HideousFS_Image *image,
                                        const char *leaf,
                                        const char **base_end);
int hideousfs_add_reverse_extension(HideousFS_Image *image,
                                    const char *extension);
void hideousfs_initialise_default_config(HideousFS_Image *image);
int hideousfs_parse_config(HideousFS_Image *image, char *buffer);
int hideousfs_build_writable_object_path(const HideousFS_Image *image,
                                         const char *name,
                                         char *dest, size_t dest_size);
int hideousfs_is_hidden_object_path(const HideousFS_Image *image,
                                    const char *name);
int hideousfs_build_writable_directory_path(const HideousFS_Image *image,
                                            const char *name,
                                            char *dest, size_t dest_size);
int hideousfs_append_leaf_slash_extension(char *dest, size_t dest_size,
                                          const char *leaf,
                                          const char *extension);

#endif
