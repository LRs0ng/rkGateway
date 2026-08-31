#define _GNU_SOURCE
#include "screen_fb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct screen_fb {
    int fd;
    uint8_t *memory;
    size_t memory_size;
    uint8_t *shadow;
    size_t shadow_size;
    size_t visible_row_size;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    int deferred_present;
    int wait_for_vsync;
    int vsync_supported;
    int dirty;
};

static int set_errno_value(int value)
{
    errno = value;
    return -1;
}

static uint32_t component_to_field(uint8_t value, struct fb_bitfield field)
{
    uint32_t maximum;
    if (field.length == 0U) {
        return 0U;
    }
    if (field.length >= 32U) {
        maximum = UINT32_MAX;
    } else {
        maximum = (UINT32_C(1) << field.length) - 1U;
    }
    return ((uint32_t)value * maximum + 127U) / 255U << field.offset;
}

static uint32_t pack_rgb(const screen_fb_t *screen, uint8_t red,
                         uint8_t green, uint8_t blue)
{
    return component_to_field(red, screen->var.red) |
           component_to_field(green, screen->var.green) |
           component_to_field(blue, screen->var.blue);
}

static int put_pixel(screen_fb_t *screen, int x, int y, uint32_t rgb888)
{
    uint8_t *pixel;
    const uint32_t packed = pack_rgb(screen,
                                     (uint8_t)(rgb888 >> 16),
                                     (uint8_t)(rgb888 >> 8),
                                     (uint8_t)rgb888);
    unsigned int bytes_per_pixel;

    if (x < 0 || y < 0 || (unsigned int)x >= screen->var.xres ||
        (unsigned int)y >= screen->var.yres) {
        return 0;
    }
    bytes_per_pixel = (screen->var.bits_per_pixel + 7U) / 8U;
    if (bytes_per_pixel == 0U || bytes_per_pixel > 4U) {
        return set_errno_value(ENOTSUP);
    }
    pixel = screen->shadow + (size_t)y * screen->fix.line_length +
            (size_t)x * bytes_per_pixel;
    if ((size_t)(pixel - screen->shadow) + bytes_per_pixel >
        screen->shadow_size) {
        return set_errno_value(EIO);
    }
    if (bytes_per_pixel == 2U) {
        const uint16_t value = (uint16_t)packed;
        memcpy(pixel, &value, sizeof(value));
    } else if (bytes_per_pixel == 3U) {
        pixel[0] = (uint8_t)packed;
        pixel[1] = (uint8_t)(packed >> 8);
        pixel[2] = (uint8_t)(packed >> 16);
    } else if (bytes_per_pixel == 4U) {
        memcpy(pixel, &packed, sizeof(packed));
    } else {
        pixel[0] = (uint8_t)packed;
    }
    screen->dirty = 1;
    return 0;
}

static int flush_if_immediate(screen_fb_t *screen)
{
    return screen->deferred_present ? 0 : screen_fb_flush(screen);
}

static int fill_rect(screen_fb_t *screen, int x, int y, unsigned int width,
                     unsigned int height, uint32_t color)
{
    unsigned int row;
    unsigned int column;
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            if (put_pixel(screen, x + (int)column, y + (int)row, color) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int valid_rotation(unsigned int rotation_degrees)
{
    return rotation_degrees == 0U || rotation_degrees == 90U ||
           rotation_degrees == 180U || rotation_degrees == 270U;
}

static void logical_dimensions(const screen_fb_t *screen,
                               unsigned int rotation_degrees,
                               unsigned int *width, unsigned int *height)
{
    if (rotation_degrees == 90U || rotation_degrees == 270U) {
        *width = screen->var.yres;
        *height = screen->var.xres;
    } else {
        *width = screen->var.xres;
        *height = screen->var.yres;
    }
}

static int logical_to_physical(const screen_fb_t *screen,
                               unsigned int rotation_degrees,
                               unsigned int logical_x, unsigned int logical_y,
                               int *physical_x, int *physical_y)
{
    const unsigned int physical_width = screen->var.xres;
    const unsigned int physical_height = screen->var.yres;
    unsigned int logical_width;
    unsigned int logical_height;

    logical_dimensions(screen, rotation_degrees, &logical_width, &logical_height);
    if (logical_x >= logical_width || logical_y >= logical_height) {
        return 0;
    }
    switch (rotation_degrees) {
    case 0U:
        *physical_x = (int)logical_x;
        *physical_y = (int)logical_y;
        break;
    case 90U: /* clockwise: logical 1920x1080 -> physical 1080x1920 */
        *physical_x = (int)(physical_width - 1U - logical_y);
        *physical_y = (int)logical_x;
        break;
    case 180U:
        *physical_x = (int)(physical_width - 1U - logical_x);
        *physical_y = (int)(physical_height - 1U - logical_y);
        break;
    case 270U: /* counter-clockwise */
        *physical_x = (int)logical_y;
        *physical_y = (int)(logical_width - 1U - logical_x);
        break;
    default:
        return 0;
    }
    return 1;
}

static int put_logical_pixel(screen_fb_t *screen, unsigned int rotation_degrees,
                             int x, int y, uint32_t rgb888)
{
    unsigned int logical_width;
    unsigned int logical_height;
    int physical_x;
    int physical_y;

    logical_dimensions(screen, rotation_degrees, &logical_width, &logical_height);
    if (x < 0 || y < 0 || (unsigned int)x >= logical_width ||
        (unsigned int)y >= logical_height) {
        return 0;
    }
    if (!logical_to_physical(screen, rotation_degrees, (unsigned int)x,
                             (unsigned int)y, &physical_x, &physical_y)) {
        return 0;
    }
    return put_pixel(screen, physical_x, physical_y, rgb888);
}

static int fill_logical_rect(screen_fb_t *screen, unsigned int rotation_degrees,
                             int x, int y, unsigned int width,
                             unsigned int height, uint32_t color)
{
    unsigned int row;
    unsigned int column;
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            if (put_logical_pixel(screen, rotation_degrees,
                                  x + (int)column, y + (int)row, color) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int next_token(const uint8_t *data, size_t size, size_t *offset,
                      char *token, size_t token_size)
{
    size_t start;
    size_t length;
    if (data == NULL || offset == NULL || token == NULL || token_size < 2U) {
        return set_errno_value(EINVAL);
    }
    while (*offset < size) {
        const uint8_t current = data[*offset];
        if (current == '#') {
            while (*offset < size && data[*offset] != '\n') {
                ++*offset;
            }
        } else if (current == ' ' || current == '\t' || current == '\r' ||
                   current == '\n') {
            ++*offset;
        } else {
            break;
        }
    }
    if (*offset >= size) {
        return set_errno_value(EINVAL);
    }
    start = *offset;
    while (*offset < size) {
        const uint8_t current = data[*offset];
        if (current == ' ' || current == '\t' || current == '\r' ||
            current == '\n' || current == '#') {
            break;
        }
        ++*offset;
    }
    length = *offset - start;
    if (length == 0U || length >= token_size) {
        return set_errno_value(EINVAL);
    }
    memcpy(token, data + start, length);
    token[length] = '\0';
    return 0;
}

static int parse_ppm(const uint8_t *data, size_t size, const uint8_t **pixels,
                     size_t *pixel_size, unsigned int *width,
                     unsigned int *height)
{
    char token[32];
    size_t offset = 0U;
    unsigned long parsed_width;
    unsigned long parsed_height;
    unsigned long max_value;
    char *end;
    if (next_token(data, size, &offset, token, sizeof(token)) < 0 ||
        strcmp(token, "P6") != 0 ||
        next_token(data, size, &offset, token, sizeof(token)) < 0) {
        return set_errno_value(EINVAL);
    }
    errno = 0;
    parsed_width = strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || parsed_width == 0U ||
        parsed_width > UINT_MAX) {
        return set_errno_value(EINVAL);
    }
    if (next_token(data, size, &offset, token, sizeof(token)) < 0) {
        return -1;
    }
    errno = 0;
    parsed_height = strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || parsed_height == 0U ||
        parsed_height > UINT_MAX) {
        return set_errno_value(EINVAL);
    }
    if (next_token(data, size, &offset, token, sizeof(token)) < 0) {
        return -1;
    }
    errno = 0;
    max_value = strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || max_value != 255U) {
        return set_errno_value(ENOTSUP);
    }
    while (offset < size && (data[offset] == ' ' || data[offset] == '\t' ||
                             data[offset] == '\r' || data[offset] == '\n')) {
        ++offset;
    }
    if (parsed_width > SIZE_MAX / parsed_height ||
        parsed_width * parsed_height > SIZE_MAX / 3U) {
        return set_errno_value(EOVERFLOW);
    }
    *pixel_size = (size_t)parsed_width * (size_t)parsed_height * 3U;
    if (offset > size || *pixel_size > size - offset) {
        return set_errno_value(EINVAL);
    }
    *pixels = data + offset;
    *width = (unsigned int)parsed_width;
    *height = (unsigned int)parsed_height;
    return 0;
}

screen_fb_t *screen_fb_open(const char *device)
{
    screen_fb_t *screen;
    unsigned int bytes_per_pixel;
    size_t mapped_offset;
    unsigned int row;
    if (device == NULL || device[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    screen = calloc(1U, sizeof(*screen));
    if (screen == NULL) {
        return NULL;
    }
    screen->fd = open(device, O_RDWR | O_CLOEXEC);
    if (screen->fd < 0) {
        free(screen);
        return NULL;
    }
    if (ioctl(screen->fd, FBIOGET_FSCREENINFO, &screen->fix) < 0 ||
        ioctl(screen->fd, FBIOGET_VSCREENINFO, &screen->var) < 0 ||
        screen->var.xres == 0U || screen->var.yres == 0U ||
        screen->fix.line_length == 0U || screen->fix.smem_len == 0U) {
        const int saved = errno == 0 ? EIO : errno;
        close(screen->fd);
        free(screen);
        errno = saved;
        return NULL;
    }
    screen->memory_size = screen->fix.smem_len;
    screen->memory = mmap(NULL, screen->memory_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, screen->fd, 0);
    if (screen->memory == MAP_FAILED) {
        const int saved = errno;
        close(screen->fd);
        free(screen);
        errno = saved;
        return NULL;
    }
    if (screen->var.bits_per_pixel != 16U && screen->var.bits_per_pixel != 24U &&
        screen->var.bits_per_pixel != 32U) {
        screen_fb_close(screen);
        errno = ENOTSUP;
        return NULL;
    }
    bytes_per_pixel = (screen->var.bits_per_pixel + 7U) / 8U;
    if ((size_t)screen->var.xres > SIZE_MAX / bytes_per_pixel ||
        (size_t)screen->fix.line_length > SIZE_MAX / screen->var.yres) {
        screen_fb_close(screen);
        errno = EOVERFLOW;
        return NULL;
    }
    screen->visible_row_size = (size_t)screen->var.xres * bytes_per_pixel;
    screen->shadow_size = (size_t)screen->fix.line_length * screen->var.yres;
    mapped_offset = (size_t)screen->var.yoffset * screen->fix.line_length +
                    (size_t)screen->var.xoffset * bytes_per_pixel;
    if (screen->visible_row_size > screen->fix.line_length ||
        mapped_offset > screen->memory_size ||
        screen->shadow_size > screen->memory_size - mapped_offset) {
        screen_fb_close(screen);
        errno = EIO;
        return NULL;
    }
    screen->shadow = malloc(screen->shadow_size);
    if (screen->shadow == NULL) {
        screen_fb_close(screen);
        return NULL;
    }
    memset(screen->shadow, 0, screen->shadow_size);
    for (row = 0U; row < screen->var.yres; ++row) {
        memcpy(screen->shadow + (size_t)row * screen->fix.line_length,
               screen->memory + mapped_offset +
                   (size_t)row * screen->fix.line_length,
               screen->visible_row_size);
    }
    screen->wait_for_vsync = 1;
    screen->vsync_supported = -1;
    return screen;
}

void screen_fb_close(screen_fb_t *screen)
{
    if (screen == NULL) {
        return;
    }
    if (screen->memory != NULL && screen->memory != MAP_FAILED) {
        (void)munmap(screen->memory, screen->memory_size);
    }
    free(screen->shadow);
    if (screen->fd >= 0) {
        (void)close(screen->fd);
    }
    free(screen);
}

unsigned int screen_fb_width(const screen_fb_t *screen)
{
    return screen == NULL ? 0U : screen->var.xres;
}

unsigned int screen_fb_height(const screen_fb_t *screen)
{
    return screen == NULL ? 0U : screen->var.yres;
}

unsigned int screen_fb_bpp(const screen_fb_t *screen)
{
    return screen == NULL ? 0U : screen->var.bits_per_pixel;
}

int screen_fb_is_rgb888(const screen_fb_t *screen)
{
    if (screen == NULL) {
        return 0;
    }
    return screen->var.red.length == 8U &&
           screen->var.green.length == 8U &&
           screen->var.blue.length == 8U;
}

void screen_fb_set_deferred_present(screen_fb_t *screen, int deferred)
{
    if (screen != NULL) {
        screen->deferred_present = deferred != 0;
    }
}

void screen_fb_set_wait_for_vsync(screen_fb_t *screen, int wait_for_vsync)
{
    if (screen != NULL) {
        screen->wait_for_vsync = wait_for_vsync != 0;
    }
}

int screen_fb_flush(screen_fb_t *screen)
{
    unsigned int row;
    unsigned int bytes_per_pixel;
    size_t mapped_offset;

    if (screen == NULL || screen->shadow == NULL) {
        return set_errno_value(EINVAL);
    }
    if (!screen->dirty) {
        return 0;
    }
    if (screen->wait_for_vsync && screen->vsync_supported != 0) {
        __u32 crtc = 0U;
        if (ioctl(screen->fd, FBIO_WAITFORVSYNC, &crtc) == 0) {
            screen->vsync_supported = 1;
        } else if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS ||
                   errno == ENOTSUP) {
            screen->vsync_supported = 0;
        } else {
            return -1;
        }
    }

    bytes_per_pixel = (screen->var.bits_per_pixel + 7U) / 8U;
    mapped_offset = (size_t)screen->var.yoffset * screen->fix.line_length +
                    (size_t)screen->var.xoffset * bytes_per_pixel;
    if (screen->var.xoffset == 0U &&
        screen->visible_row_size == screen->fix.line_length) {
        memcpy(screen->memory + mapped_offset, screen->shadow,
               screen->shadow_size);
    } else {
        for (row = 0U; row < screen->var.yres; ++row) {
            memcpy(screen->memory + mapped_offset +
                       (size_t)row * screen->fix.line_length,
                   screen->shadow + (size_t)row * screen->fix.line_length,
                   screen->visible_row_size);
        }
    }
    screen->dirty = 0;
    return 0;
}

int screen_fb_clear(screen_fb_t *screen, uint32_t rgb888)
{
    if (screen == NULL) {
        return set_errno_value(EINVAL);
    }
    if (fill_rect(screen, 0, 0, screen->var.xres, screen->var.yres,
                  rgb888 & UINT32_C(0xffffff)) < 0) {
        return -1;
    }
    return flush_if_immediate(screen);
}

static const uint8_t *raw_rgb24_pixel(const uint8_t *pixels, unsigned int width,
                                      unsigned int x, unsigned int y)
{
    return pixels + ((size_t)y * width + x) * 3U;
}

static uint32_t raw_pixel(const uint8_t *pixels, unsigned int width,
                          unsigned int height, unsigned int x, unsigned int y,
                          const char *format)
{
    if (strcmp(format, "rgb565") == 0) {
        const uint8_t *source = pixels + ((size_t)y * width + x) * 2U;
        const uint16_t value = (uint16_t)source[0] | (uint16_t)source[1] << 8;
        const uint8_t red = (uint8_t)(((value >> 11) & 0x1fU) * 255U / 31U);
        const uint8_t green = (uint8_t)(((value >> 5) & 0x3fU) * 255U / 63U);
        const uint8_t blue = (uint8_t)((value & 0x1fU) * 255U / 31U);
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    }
    (void)height;
    if (strcmp(format, "rgb888") != 0 && strcmp(format, "rgb24") != 0) {
        return 0U;
    }
    {
        const uint8_t *source = raw_rgb24_pixel(pixels, width, x, y);
        return ((uint32_t)source[0] << 16) | ((uint32_t)source[1] << 8) |
               source[2];
    }
}

int screen_fb_present_rotated(screen_fb_t *screen, const uint8_t *data, size_t size,
                              const char *format, unsigned int width,
                              unsigned int height, int fit,
                              uint32_t background_rgb888,
                              unsigned int rotation_degrees)
{
    const uint8_t *pixels = data;
    size_t pixel_size = size;
    char selected_format[16];
    unsigned int source_width = width;
    unsigned int source_height = height;
    unsigned int logical_width;
    unsigned int logical_height;
    unsigned int output_width;
    unsigned int output_height;
    unsigned int x;
    unsigned int y;
    int x_offset;
    int y_offset;
    if (screen == NULL || data == NULL || size == 0U) {
        return set_errno_value(EINVAL);
    }
    if (!valid_rotation(rotation_degrees)) {
        return set_errno_value(EINVAL);
    }
    logical_dimensions(screen, rotation_degrees, &logical_width, &logical_height);
    if (format == NULL || format[0] == '\0' || strcmp(format, "auto") == 0) {
        if (size >= 2U && data[0] == 'P' && data[1] == '6') {
            (void)snprintf(selected_format, sizeof(selected_format), "ppm");
        } else {
            (void)snprintf(selected_format, sizeof(selected_format), "rgb888");
        }
    } else {
        (void)snprintf(selected_format, sizeof(selected_format), "%s", format);
    }
    if (strcmp(selected_format, "ppm") == 0) {
        if (parse_ppm(data, size, &pixels, &pixel_size, &source_width,
                      &source_height) < 0) {
            return -1;
        }
        (void)pixel_size;
    } else if (strcmp(selected_format, "rgb565") == 0) {
        if (source_width == 0U || source_height == 0U ||
            (size_t)source_width > SIZE_MAX / source_height ||
            (size_t)source_width * source_height > SIZE_MAX / 2U ||
            size < (size_t)source_width * source_height * 2U) {
            return set_errno_value(EINVAL);
        }
    } else if (strcmp(selected_format, "rgb888") == 0 ||
               strcmp(selected_format, "rgb24") == 0) {
        if (source_width == 0U || source_height == 0U ||
            (size_t)source_width > SIZE_MAX / source_height ||
            (size_t)source_width * source_height > SIZE_MAX / 3U ||
            size < (size_t)source_width * source_height * 3U) {
            return set_errno_value(EINVAL);
        }
    } else {
        return set_errno_value(ENOTSUP);
    }
    if (source_width == 0U || source_height == 0U) {
        return set_errno_value(EINVAL);
    }
    output_width = logical_width;
    output_height = logical_height;
    if (fit) {
        const uint64_t width_scale = (uint64_t)output_width * source_height;
        const uint64_t height_scale = (uint64_t)output_height * source_width;
        if (width_scale < height_scale) {
            output_width = (unsigned int)width_scale / source_height;
            output_height = output_width * source_height / source_width;
        } else {
            output_height = (unsigned int)height_scale / source_width;
            output_width = output_height * source_width / source_height;
        }
    }
    if (output_width == 0U || output_height == 0U) {
        return set_errno_value(EINVAL);
    }
    x_offset = ((int)logical_width - (int)output_width) / 2;
    y_offset = ((int)logical_height - (int)output_height) / 2;
    if (fill_rect(screen, 0, 0, screen->var.xres, screen->var.yres,
                  background_rgb888 & UINT32_C(0xffffff)) < 0) {
        return -1;
    }
    for (y = 0U; y < output_height; ++y) {
        const unsigned int source_y = (unsigned int)((uint64_t)y * source_height /
                                                     output_height);
        for (x = 0U; x < output_width; ++x) {
            const unsigned int source_x = (unsigned int)((uint64_t)x * source_width /
                                                         output_width);
            const uint32_t color = raw_pixel(pixels, source_width, source_height,
                                              source_x, source_y,
                                              strcmp(selected_format, "ppm") == 0
                                                  ? "rgb24" : selected_format);
            if (put_logical_pixel(screen, rotation_degrees,
                                  x_offset + (int)x, y_offset + (int)y,
                                  color) < 0) {
                return -1;
            }
        }
    }
    return flush_if_immediate(screen);
}

int screen_fb_present(screen_fb_t *screen, const uint8_t *data, size_t size,
                      const char *format, unsigned int width,
                      unsigned int height, int fit, uint32_t background_rgb888)
{
    return screen_fb_present_rotated(screen, data, size, format, width, height,
                                     fit, background_rgb888, 0U);
}

static const uint8_t digit_glyphs[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
    {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
};

static uint8_t glyph_row(char character, unsigned int row)
{
    if (character >= '0' && character <= '9') {
        return digit_glyphs[character - '0'][row];
    }
    if (character == '-') {
        return row == 3U ? 0x1fU : 0U;
    }
    if (character == '+') {
        if (row == 3U) {
            return 0x1fU;
        }
        return row == 1U || row == 2U || row == 4U || row == 5U ? 0x04U : 0U;
    }
    if (character == '.') {
        return row == 6U ? 0x04U : 0U;
    }
    if (character == 'e' || character == 'E') {
        static const uint8_t glyph[7] = {0x00, 0x0e, 0x11, 0x1f, 0x10, 0x11, 0x0e};
        return glyph[row];
    }
    return 0U;
}

int screen_fb_draw_number_rotated(screen_fb_t *screen, double value, int x, int y,
                                  unsigned int scale, uint32_t foreground,
                                  uint32_t background, int clear_background,
                                  unsigned int rotation_degrees)
{
    char text[64];
    size_t length;
    size_t index;
    unsigned int row;
    unsigned int column;
    unsigned int glyph_width;
    if (screen == NULL || scale == 0U || !isfinite(value) ||
        !valid_rotation(rotation_degrees)) {
        return set_errno_value(EINVAL);
    }
    (void)snprintf(text, sizeof(text), "%.3f", value);
    length = strlen(text);
    glyph_width = 6U * scale;
    if (clear_background &&
        fill_logical_rect(screen, rotation_degrees, x, y,
                          (unsigned int)length * glyph_width, 7U * scale,
                          background) < 0) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        const char character = text[index];
        for (row = 0U; row < 7U; ++row) {
            const uint8_t bits = glyph_row(character, row);
            for (column = 0U; column < 5U; ++column) {
                if ((bits & (uint8_t)(1U << (4U - column))) != 0U) {
                    unsigned int dy;
                    unsigned int dx;
                    for (dy = 0U; dy < scale; ++dy) {
                        for (dx = 0U; dx < scale; ++dx) {
                            if (put_logical_pixel(
                                    screen, rotation_degrees,
                                    x + (int)(index * glyph_width +
                                               column * scale + dx),
                                    y + (int)(row * scale + dy), foreground) < 0) {
                                return -1;
                            }
                        }
                    }
                }
            }
        }
    }
    return flush_if_immediate(screen);
}

int screen_fb_draw_number(screen_fb_t *screen, double value, int x, int y,
                          unsigned int scale, uint32_t foreground,
                          uint32_t background, int clear_background)
{
    return screen_fb_draw_number_rotated(screen, value, x, y, scale,
                                          foreground, background,
                                          clear_background, 0U);
}
