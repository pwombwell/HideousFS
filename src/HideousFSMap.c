#include <string.h>

#include "HideousFS.h"

static const char *default_reverse_extensions[] = {
    "c", "h", "s", "o", "a", "cpp", "c++"
};

static int hideousfs_default_reverse_extension_count(void);
static const char *hideousfs_previous_component(const char *name,
                                                const char *component);
static int hideousfs_is_space_char(char ch);
static char *hideousfs_next_word(char **cursor);
static int hideousfs_strings_equal(const char *left, const char *right);
static int hideousfs_parse_config_line(HideousFS_Image *image, char *line,
                                       int *saw_directive);

int hideousfs_mapped_extension_index(const HideousFS_Image *image,
                                  const char *name, size_t len)
{
    int i;

    for (i = 0; i < image->reverse_extension_count; ++i) {
        if (strlen(image->reverse_extensions[i]) == len &&
            strncmp(image->reverse_extensions[i], name, len) == 0) {
            return i;
        }
    }

    return -1;
}

int hideousfs_is_mapped_extension(const HideousFS_Image *image, const char *name)
{
    return hideousfs_mapped_extension_index(image, name, strlen(name)) >= 0;
}

int hideousfs_is_root_name(const char *name)
{
    return name == NULL || name[0] == '\0' ||
           (name[0] == '$' && name[1] == '\0');
}

int hideousfs_copy_string(char *dest, size_t dest_size, const char *src)
{
    size_t len = strlen(src);

    if (len >= dest_size) {
        return 0;
    }

    memcpy(dest, src, len + 1);
    return 1;
}

const char *hideousfs_skip_root_prefix(const char *name)
{
    if (name != NULL && name[0] == '$' && name[1] == '.') {
        return name + 2;
    }

    return name;
}

int hideousfs_path_has_mapped_component(const HideousFS_Image *image,
                                     const char *name)
{
    const char *component = hideousfs_skip_root_prefix(name);

    while (component != NULL && component[0] != '\0') {
        const char *dot = strchr(component, '.');
        size_t len = dot == NULL ? strlen(component) : (size_t)(dot - component);

        if (hideousfs_mapped_extension_index(image, component, len) >= 0) {
            return 1;
        }

        component = dot == NULL ? NULL : dot + 1;
    }

    return 0;
}

int hideousfs_append_relative_to_backing(const HideousFS_Image *image,
                                      const char *relative,
                                      char *dest, size_t dest_size)
{
    size_t dir_len;
    size_t relative_len;

    dir_len = strlen(image->backing_dir);
    relative_len = strlen(relative);

    if (relative_len == 0) {
        return hideousfs_copy_string(dest, dest_size, image->backing_dir);
    }

    if (dir_len + 1 + relative_len >= dest_size) {
        return 0;
    }

    memcpy(dest, image->backing_dir, dir_len);
    dest[dir_len] = '.';
    memcpy(dest + dir_len + 1, relative, relative_len + 1);
    return 1;
}

const char *hideousfs_last_component(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot == NULL ? name : dot + 1;
}

int hideousfs_is_projected_object_name(const HideousFS_Image *image,
                                    const char *name)
{
    const char *relative;
    const char *leaf;
    const char *extension;

    if (hideousfs_is_root_name(name)) {
        return 0;
    }

    relative = hideousfs_skip_root_prefix(name);
    leaf = hideousfs_last_component(relative);

    if (image->beautiful_mode) {
        const char *slash = strrchr(leaf, '/');

        return slash != NULL && slash != leaf && slash[1] != '\0' &&
               hideousfs_mapped_extension_index(image, slash + 1,
                                      strlen(slash + 1)) >= 0;
    }

    extension = hideousfs_previous_component(relative, leaf);
    if (extension == NULL) {
        return 0;
    }

    return hideousfs_mapped_extension_index(image, extension,
                                  (size_t)((leaf - 1) - extension)) >= 0;
}

int hideousfs_build_backing_object_path(const HideousFS_Image *image,
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

    if (hideousfs_is_root_name(name)) {
        return hideousfs_copy_string(dest, dest_size, image->backing_dir);
    }

    relative = hideousfs_skip_root_prefix(name);

    if (image->beautiful_mode) {
        const char *slash;

        leaf = hideousfs_last_component(relative);
        slash = strrchr(leaf, '/');
        if (slash == NULL || slash == leaf || slash[1] == '\0' ||
            hideousfs_mapped_extension_index(image, slash + 1, strlen(slash + 1)) < 0) {
            return hideousfs_append_relative_to_backing(image, relative, dest, dest_size);
        }

        prefix_end = leaf == relative ? leaf : leaf - 1;
        prefix_len = (size_t)(prefix_end - relative);
        leaf_len = (size_t)(slash - leaf);
        extension = slash + 1;
        extension_len = strlen(extension);
        dir_len = strlen(image->backing_dir);

        if (dir_len + 1 + prefix_len + (prefix_len == 0 ? 0 : 1) +
            extension_len + 1 + leaf_len >= dest_size) {
            return 0;
        }

        memcpy(dest, image->backing_dir, dir_len);
        dest[dir_len++] = '.';
        if (prefix_len != 0) {
            memcpy(dest + dir_len, relative, prefix_len);
            dir_len += prefix_len;
            dest[dir_len++] = '.';
        }
        memcpy(dest + dir_len, extension, extension_len);
        dir_len += extension_len;
        dest[dir_len++] = '.';
        memcpy(dest + dir_len, leaf, leaf_len);
        dest[dir_len + leaf_len] = '\0';
        return 1;
    }

    leaf = hideousfs_last_component(relative);
    extension = hideousfs_previous_component(relative, leaf);

    if (extension == NULL) {
        return hideousfs_append_relative_to_backing(image, relative, dest, dest_size);
    }

    prefix_end = extension == relative ? extension : extension - 1;
    extension_len = (size_t)((leaf - 1) - extension);
    if (hideousfs_mapped_extension_index(image, extension, extension_len) < 0) {
        return hideousfs_append_relative_to_backing(image, relative, dest, dest_size);
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

int hideousfs_resolve_directory_path(const HideousFS_Image *image,
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

    if (hideousfs_is_root_name(name)) {
        return hideousfs_copy_string(dest, dest_size, image->backing_dir);
    }

    relative = hideousfs_skip_root_prefix(name);
    if (image->beautiful_mode) {
        return hideousfs_append_relative_to_backing(image, relative, dest, dest_size);
    }

    component = hideousfs_last_component(relative);
    extension_len = strlen(component);

    if (hideousfs_mapped_extension_index(image, component, extension_len) >= 0) {
        if (extension_len >= synthetic_extension_size) {
            return 0;
        }
        memcpy(synthetic_extension, component, extension_len + 1);

        if (component == relative) {
            return hideousfs_copy_string(dest, dest_size, image->backing_dir);
        }

        prefix_end = component - 1;
        if ((size_t)(prefix_end - relative) >= dest_size) {
            return 0;
        }

        memcpy(prefix, relative, (size_t)(prefix_end - relative));
        prefix[prefix_end - relative] = '\0';
        return hideousfs_append_relative_to_backing(image, prefix, dest, dest_size);
    }

    return hideousfs_append_relative_to_backing(image, relative, dest, dest_size);
}

int hideousfs_is_active_image_entry(const HideousFS_Image *image, const char *dir,
                                 const char *leaf)
{
    return hideousfs_is_root_name(dir) && strcmp(leaf, image->image_leaf) == 0;
}

int hideousfs_is_active_image_path(const HideousFS_Image *image, const char *name)
{
    return name != NULL && strcmp(hideousfs_skip_root_prefix(name), image->image_leaf) == 0;
}

int hideousfs_host_leaf_extension_index(const HideousFS_Image *image,
                                     const char *leaf, const char **base_end)
{
    const char *slash = strrchr(leaf, '/');

    if (slash == NULL || slash == leaf || slash[1] == '\0') {
        return -1;
    }

    if (base_end != NULL) {
        *base_end = slash;
    }

    return hideousfs_mapped_extension_index(image, slash + 1, strlen(slash + 1));
}

int hideousfs_add_reverse_extension(HideousFS_Image *image, const char *extension)
{
    size_t len = strlen(extension);

    if (len == 0 || len >= MaxExtensionLen) {
        return 0;
    }

    if (hideousfs_mapped_extension_index(image, extension, len) >= 0) {
        return 1;
    }

    if (image->reverse_extension_count >= MaxReverseExtensions) {
        return 0;
    }

    memcpy(image->reverse_extensions[image->reverse_extension_count],
           extension, len + 1);
    image->reverse_extension_count++;
    return 1;
}

void hideousfs_initialise_default_config(HideousFS_Image *image)
{
    int i;

    image->beautiful_mode = 0;
    image->reverse_extension_count = 0;
    for (i = 0; i < hideousfs_default_reverse_extension_count(); ++i) {
        (void)hideousfs_add_reverse_extension(image, default_reverse_extensions[i]);
    }
}

int hideousfs_parse_config(HideousFS_Image *image, char *buffer)
{
    char *line = buffer;
    int saw_directive = 0;

    image->beautiful_mode = 0;
    image->reverse_extension_count = 0;

    while (*line != '\0') {
        char *next = line;

        while (*next != '\0' && *next != '\n' && *next != '\r') {
            ++next;
        }
        if (*next != '\0') {
            char separator = *next;

            *next = '\0';
            ++next;
            if (separator == '\r' && *next == '\n') {
                *next = '\0';
                ++next;
            }
        }

        if (!hideousfs_parse_config_line(image, line, &saw_directive)) {
            return 0;
        }
        line = next;
    }

    return saw_directive && image->reverse_extension_count != 0;
}

int hideousfs_build_writable_object_path(const HideousFS_Image *image,
                                      const char *name,
                                      char *dest, size_t dest_size)
{
    if (hideousfs_is_active_image_path(image, name)) {
        return 0;
    }

    if (image->beautiful_mode && hideousfs_path_has_mapped_component(image, name)) {
        return 0;
    }

    if (!image->beautiful_mode &&
        hideousfs_is_mapped_extension(image, hideousfs_last_component(hideousfs_skip_root_prefix(name)))) {
        return 0;
    }

    return hideousfs_build_backing_object_path(image, name, dest, dest_size);
}

int hideousfs_is_hidden_object_path(const HideousFS_Image *image, const char *name)
{
    if (image->beautiful_mode) {
        return hideousfs_path_has_mapped_component(image, name);
    }

    return hideousfs_is_mapped_extension(image, hideousfs_last_component(hideousfs_skip_root_prefix(name)));
}

int hideousfs_build_writable_directory_path(const HideousFS_Image *image,
                                         const char *name,
                                         char *dest, size_t dest_size)
{
    if (hideousfs_is_active_image_path(image, name) ||
        hideousfs_path_has_mapped_component(image, name)) {
        return 0;
    }

    if (hideousfs_is_root_name(name)) {
        return 0;
    }

    if (image->beautiful_mode) {
        return hideousfs_build_backing_object_path(image, name, dest, dest_size);
    }

    return hideousfs_append_relative_to_backing(image, hideousfs_skip_root_prefix(name),
                                      dest, dest_size);
}

int hideousfs_append_leaf_slash_extension(char *dest, size_t dest_size,
                                       const char *leaf,
                                       const char *extension)
{
    size_t leaf_len = strlen(leaf);
    size_t extension_len = strlen(extension);

    if (leaf_len + 1 + extension_len >= dest_size) {
        return 0;
    }

    memcpy(dest, leaf, leaf_len);
    dest[leaf_len] = '/';
    memcpy(dest + leaf_len + 1, extension, extension_len + 1);
    return 1;
}

static int hideousfs_default_reverse_extension_count(void)
{
    return (int)(sizeof(default_reverse_extensions) /
                 sizeof(default_reverse_extensions[0]));
}

static const char *hideousfs_previous_component(const char *name, const char *component)
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

static int hideousfs_is_space_char(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static char *hideousfs_next_word(char **cursor)
{
    char *start;

    while (**cursor != '\0' && hideousfs_is_space_char(**cursor)) {
        ++*cursor;
    }

    if (**cursor == '\0') {
        return NULL;
    }

    start = *cursor;
    while (**cursor != '\0' && !hideousfs_is_space_char(**cursor)) {
        ++*cursor;
    }

    if (**cursor != '\0') {
        **cursor = '\0';
        ++*cursor;
    }

    return start;
}

static int hideousfs_strings_equal(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

static int hideousfs_parse_config_line(HideousFS_Image *image, char *line,
                             int *saw_directive)
{
    char *comment = strchr(line, '#');
    char *cursor;
    char *keyword;

    if (comment != NULL) {
        *comment = '\0';
    }

    cursor = line;
    keyword = hideousfs_next_word(&cursor);
    if (keyword == NULL) {
        return 1;
    }

    *saw_directive = 1;

    if (hideousfs_strings_equal(keyword, "mode")) {
        char *mode = hideousfs_next_word(&cursor);

        if (mode == NULL || hideousfs_next_word(&cursor) != NULL) {
            return 0;
        }
        if (hideousfs_strings_equal(mode, "hideous")) {
            image->beautiful_mode = 0;
            return 1;
        }
        if (hideousfs_strings_equal(mode, "beautiful")) {
            image->beautiful_mode = 1;
            return 1;
        }
        return 0;
    }

    if (hideousfs_strings_equal(keyword, "reverse")) {
        char *extension;
        int saw_extension = 0;

        while ((extension = hideousfs_next_word(&cursor)) != NULL) {
            if (!hideousfs_add_reverse_extension(image, extension)) {
                return 0;
            }
            saw_extension = 1;
        }

        return saw_extension;
    }

    return 0;
}
