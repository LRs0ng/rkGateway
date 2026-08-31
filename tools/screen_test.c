#define _GNU_SOURCE
#include "screen_fb.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -d, --device PATH       framebuffer，默认 /dev/fb0\n"
            "  -i, --image PATH        输入图片（P6 PPM、RGB888 或 RGB565）\n"
            "  -f, --format FORMAT      auto/ppm/rgb888/rgb24/rgb565，默认 auto\n"
            "  -w, --width N            原始格式宽度；按方向自动默认为 1080 或 1920\n"
            "  -h, --height N           原始格式高度；按方向自动默认为 1920 或 1080\n"
            "  -m, --mode MODE          portrait/landscape，默认 portrait\n"
            "  -r, --rotation DEG       0/90/180/270，默认由 mode 决定\n"
            "      --no-fit             不保持比例，直接缩放到逻辑画布\n"
            "      --number VALUE       在左上角显示一个数字\n"
            "      --clear              只清屏，不显示图片\n"
            "      --no-vsync           提交帧前不等待垂直同步\n"
            "      --help               显示帮助\n",
            program);
}

static int parse_unsigned(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > UINT_MAX) {
        return -1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int parse_rotation(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        (parsed != 0U && parsed != 90U && parsed != 180U && parsed != 270U)) {
        return -1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int mode_is_landscape(const char *mode)
{
    return mode != NULL && strcmp(mode, "landscape") == 0;
}

static int read_file(const char *path, unsigned char **data, size_t *size)
{
    int fd;
    struct stat st;
    unsigned char *buffer;
    size_t total = 0U;
    ssize_t count;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) {
        const int saved = errno == 0 ? EIO : errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    buffer = malloc((size_t)st.st_size == 0U ? 1U : (size_t)st.st_size);
    if (buffer == NULL) {
        (void)close(fd);
        return -1;
    }
    while (total < (size_t)st.st_size) {
        count = read(fd, buffer + total, (size_t)st.st_size - total);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int saved = count < 0 ? errno : EIO;
            free(buffer);
            (void)close(fd);
            errno = saved;
            return -1;
        }
        total += (size_t)count;
    }
    if (close(fd) < 0) {
        const int saved = errno;
        free(buffer);
        errno = saved;
        return -1;
    }
    *data = buffer;
    *size = total;
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/fb0";
    const char *image_path = NULL;
    const char *format = "auto";
    const char *mode = "portrait";
    unsigned int width = 1080U;
    unsigned int height = 1920U;
    unsigned int rotation = 0U;
    int width_set = 0;
    int height_set = 0;
    int rotation_set = 0;
    int fit = 1;
    int clear_only = 0;
    int have_number = 0;
    int wait_for_vsync = 1;
    double number = 0.0;
    unsigned char *image = NULL;
    size_t image_size = 0U;
    screen_fb_t *screen = NULL;
    int option;
    int exit_code = EXIT_FAILURE;
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"image", required_argument, NULL, 'i'},
        {"format", required_argument, NULL, 'f'},
        {"width", required_argument, NULL, 'w'},
        {"height", required_argument, NULL, 'h'},
        {"mode", required_argument, NULL, 'm'},
        {"rotation", required_argument, NULL, 'r'},
        {"no-fit", no_argument, NULL, 2},
        {"number", required_argument, NULL, 3},
        {"clear", no_argument, NULL, 4},
        {"no-vsync", no_argument, NULL, 5},
        {"help", no_argument, NULL, 1},
        {NULL, 0, NULL, 0}
    };

    while ((option = getopt_long(argc, argv, "d:i:f:w:h:m:r:", options, NULL)) != -1) {
        switch (option) {
        case 'd':
            device = optarg;
            break;
        case 'i':
            image_path = optarg;
            break;
        case 'f':
            format = optarg;
            break;
        case 'w':
            if (parse_unsigned(optarg, &width) < 0) {
                fprintf(stderr, "宽度无效: %s\n", optarg);
                goto cleanup;
            }
            width_set = 1;
            break;
        case 'h':
            if (parse_unsigned(optarg, &height) < 0) {
                fprintf(stderr, "高度无效: %s\n", optarg);
                goto cleanup;
            }
            height_set = 1;
            break;
        case 'm':
            if (strcmp(optarg, "portrait") != 0 &&
                strcmp(optarg, "landscape") != 0) {
                fprintf(stderr, "模式无效: %s（应为 portrait 或 landscape）\n",
                        optarg);
                goto cleanup;
            }
            mode = optarg;
            break;
        case 'r':
            if (parse_rotation(optarg, &rotation) < 0) {
                fprintf(stderr, "旋转角度无效: %s（应为 0/90/180/270）\n",
                        optarg);
                goto cleanup;
            }
            rotation_set = 1;
            break;
        case 1:
            usage(argv[0]);
            return EXIT_SUCCESS;
        case 2:
            fit = 0;
            break;
        case 3: {
            char *end = NULL;
            errno = 0;
            number = strtod(optarg, &end);
            if (errno != 0 || end == optarg || *end != '\0') {
                fprintf(stderr, "数字无效: %s\n", optarg);
                goto cleanup;
            }
            have_number = 1;
            break;
        }
        case 4:
            clear_only = 1;
            break;
        case 5:
            wait_for_vsync = 0;
            break;
        default:
            usage(argv[0]);
            goto cleanup;
        }
    }
    if (!rotation_set) {
        rotation = mode_is_landscape(mode) ? 90U : 0U;
    }
    if (!width_set) {
        width = (rotation == 90U || rotation == 270U) ? 1920U : 1080U;
    }
    if (!height_set) {
        height = (rotation == 90U || rotation == 270U) ? 1080U : 1920U;
    }
    if (!clear_only && image_path == NULL) {
        fprintf(stderr, "必须指定 --image，或使用 --clear\n");
        usage(argv[0]);
        goto cleanup;
    }
    if (strcmp(format, "auto") != 0 && strcmp(format, "ppm") != 0 &&
        strcmp(format, "rgb888") != 0 && strcmp(format, "rgb24") != 0 &&
        strcmp(format, "rgb565") != 0) {
        fprintf(stderr, "格式无效: %s\n", format);
        goto cleanup;
    }
    if (clear_only && image_path != NULL) {
        fprintf(stderr, "--clear 不能和 --image 同时使用\n");
        goto cleanup;
    }

    screen = screen_fb_open(device);
    if (screen == NULL) {
        fprintf(stderr, "打开 framebuffer 失败: %s: %s\n", device,
                strerror(errno));
        goto cleanup;
    }
    fprintf(stdout, "device=%s size=%ux%u bpp=%u framebuffer_rgb888=%s\n", device,
            screen_fb_width(screen), screen_fb_height(screen), screen_fb_bpp(screen),
            screen_fb_is_rgb888(screen) ? "yes" : "no");
    if (screen_fb_width(screen) != 1080U || screen_fb_height(screen) != 1920U) {
        fprintf(stderr, "警告: framebuffer 不是目标分辨率 1080x1920\n");
    }
    if (!screen_fb_is_rgb888(screen)) {
        fprintf(stderr, "警告: framebuffer 不是 RGB888 通道布局，目标屏幕应为 RGB888\n");
    }
    screen_fb_set_deferred_present(screen, 1);
    screen_fb_set_wait_for_vsync(screen, wait_for_vsync);
    fprintf(stdout, "mode=%s rotation=%u logical_size=%ux%u input_size=%ux%u\n",
            mode, rotation,
            (rotation == 90U || rotation == 270U) ? screen_fb_height(screen)
                                                  : screen_fb_width(screen),
            (rotation == 90U || rotation == 270U) ? screen_fb_width(screen)
                                                  : screen_fb_height(screen),
            width, height);
    if (clear_only) {
        if (screen_fb_clear(screen, 0x000000U) < 0) {
            fprintf(stderr, "清屏失败: %s\n", strerror(errno));
            goto cleanup;
        }
    } else {
        if (read_file(image_path, &image, &image_size) < 0) {
            fprintf(stderr, "读取图片失败: %s: %s\n", image_path, strerror(errno));
            goto cleanup;
        }
        if (screen_fb_present_rotated(screen, image, image_size, format,
                                      width, height, fit, 0x000000U,
                                      rotation) < 0) {
            fprintf(stderr, "显示图片失败: %s\n", strerror(errno));
            goto cleanup;
        }
        fprintf(stdout, "image=%s bytes=%zu format=%s\n", image_path, image_size,
                format);
    }
    if (have_number &&
        screen_fb_draw_number_rotated(screen, number, 0, 0, 1U,
                                      0xffffffU, 0x000000U, 1,
                                      rotation) < 0) {
        fprintf(stderr, "显示数字失败: %s\n", strerror(errno));
        goto cleanup;
    }
    if (screen_fb_flush(screen) < 0) {
        fprintf(stderr, "提交完整帧失败: %s\n", strerror(errno));
        goto cleanup;
    }
    fprintf(stdout, "buffering=shadow-copy vsync=%s\n",
            wait_for_vsync ? "on" : "off");
    exit_code = EXIT_SUCCESS;

cleanup:
    free(image);
    screen_fb_close(screen);
    return exit_code;
}
