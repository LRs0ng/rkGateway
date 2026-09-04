#define _GNU_SOURCE
#include "camera_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -d, --device PATH       V4L2 设备，默认 /dev/video0\n"
            "  -o, --output PATH       输出文件；默认保存到程序目录 capture.<ext>\n"
            "  -w, --width N           请求 RKISP 输出宽度，默认 1920；pipeline 默认 640\n"
            "  -h, --height N          请求 RKISP 输出高度，默认 1080；pipeline 默认 640\n"
            "  -f, --pixfmt FORMAT     auto/mjpeg/nv12/yuyv/yuv420/rgb24\n"
            "  -n, --count N           抓拍次数，0 表示持续抓拍，默认 1\n"
            "  -i, --interval-ms N     连续抓拍间隔，默认 1000 ms\n"
            "  -t, --timeout-ms N      单帧等待超时，默认 3000 ms\n"
            "  -u, --warmup N          丢弃初始帧数，默认 3\n"
            "      --pipeline          保持内核 media pipeline 配置，RKISP 中央裁剪/缩放\n"
            "                          NV12 -> RGA RGB24，默认输出 640x640\n"
            "      --sensor-mode-width N   IMX415 驱动模式宽度，默认 3864\n"
            "      --sensor-mode-height N  IMX415 驱动模式高度，默认 2192\n"
            "      --sensor-active-width N 有效图像宽度，默认 3840\n"
            "      --sensor-active-height N 有效图像高度，默认 2160\n"
            "      --crop-width N      RKISP 中央裁剪宽度，默认 1080\n"
            "      --crop-height N     RKISP 中央裁剪高度，默认 1080\n"
            "      --control-device P  IMX415 控制节点，默认 auto\n"
            "      --brightness N      设置 V4L2 brightness（若节点支持）\n"
            "      --exposure N        设置 IMX415 exposure 行数\n"
            "      --analogue-gain N   设置 IMX415 analogue gain（0..240）\n"
            "      --help              显示帮助\n",
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
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > UINT_MAX) {
        return -1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int parse_positive_int(const char *text, int *value)
{
    unsigned int parsed;
    if (parse_unsigned(text, &parsed) < 0 || parsed == 0U ||
        parsed > (unsigned int)INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_signed_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static char *program_directory(const char *program)
{
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable,
                             sizeof(executable) - 1U);
    char *slash;
    if (length > 0) {
        executable[length] = '\0';
        slash = strrchr(executable, '/');
        if (slash != NULL) {
            *slash = '\0';
            return strdup(executable[0] == '\0' ? "/" : executable);
        }
    }
    if (program != NULL && strchr(program, '/') != NULL) {
        char *copy = strdup(program);
        if (copy != NULL) {
            slash = strrchr(copy, '/');
            if (slash != NULL) {
                *slash = '\0';
                return copy;
            }
        }
        free(copy);
    }
    return strdup(".");
}

static char *default_output_path(const char *directory, uint32_t pixfmt)
{
    const char *extension = camera_pixel_format_extension(pixfmt);
    const size_t length = strlen(directory) + strlen("/capture.") +
                          strlen(extension) + 1U;
    char *path = malloc(length);
    if (path != NULL) {
        (void)snprintf(path, length, "%s/capture.%s", directory, extension);
    }
    return path;
}

static char *numbered_output_path(const char *base, unsigned int number,
                                  unsigned int total)
{
    const char *slash = strrchr(base, '/');
    const char *dot = strrchr(base, '.');
    const size_t prefix_length = dot != NULL && (slash == NULL || dot > slash)
                                     ? (size_t)(dot - base)
                                     : strlen(base);
    const size_t suffix_length = strlen(base) - prefix_length;
    char suffix[32];
    int suffix_written;
    size_t length;
    char *path;
    if (total == 1U) {
        return strdup(base);
    }
    suffix_written = snprintf(suffix, sizeof(suffix), "-%04u", number);
    if (suffix_written < 0 || (size_t)suffix_written >= sizeof(suffix)) {
        return NULL;
    }
    length = prefix_length + (size_t)suffix_written + suffix_length + 1U;
    path = malloc(length);
    if (path != NULL) {
        (void)memcpy(path, base, prefix_length);
        (void)memcpy(path + prefix_length, suffix, (size_t)suffix_written);
        (void)memcpy(path + prefix_length + (size_t)suffix_written,
                     base + prefix_length, suffix_length);
        path[length - 1U] = '\0';
    }
    return path;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size > 0U) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        data += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int save_image(const char *path, const uint8_t *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, data, size) < 0) {
        const int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/video0";
    const char *output_option = NULL;
    unsigned int width = 1920U;
    unsigned int height = 1080U;
    int width_explicit = 0;
    int height_explicit = 0;
    int format_explicit = 0;
    int pipeline_enabled = 0;
    camera_pipeline_settings_t pipeline = {
        .sensor_mode_width = 3864U,
        .sensor_mode_height = 2192U,
        .sensor_active_width = 3840U,
        .sensor_active_height = 2160U,
        .crop_width = 1080U,
        .crop_height = 1080U,
        .rga_rgb24 = 1,
    };
    unsigned int count = 1U;
    unsigned int interval_ms = 1000U;
    unsigned int warmup = 3U;
    int timeout_ms = 3000;
    const char *control_device = "auto";
    camera_control_settings_t controls = {0};
    camera_pixel_format_t requested_format = CAMERA_PIXFMT_AUTO;
    camera_capture_t capture;
    char *directory = NULL;
    char *output_base = NULL;
    unsigned int sample = 0U;
    int exit_code = EXIT_FAILURE;
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"output", required_argument, NULL, 'o'},
        {"width", required_argument, NULL, 'w'},
        {"height", required_argument, NULL, 'h'},
        {"pixfmt", required_argument, NULL, 'f'},
        {"count", required_argument, NULL, 'n'},
        {"interval-ms", required_argument, NULL, 'i'},
        {"timeout-ms", required_argument, NULL, 't'},
        {"warmup", required_argument, NULL, 'u'},
        {"pipeline", no_argument, NULL, 6},
        {"rga-pipeline", no_argument, NULL, 6},
        {"sensor-mode-width", required_argument, NULL, 7},
        {"sensor-mode-height", required_argument, NULL, 8},
        {"sensor-active-width", required_argument, NULL, 9},
        {"sensor-active-height", required_argument, NULL, 12},
        {"crop-width", required_argument, NULL, 10},
        {"crop-height", required_argument, NULL, 11},
        {"control-device", required_argument, NULL, 2},
        {"brightness", required_argument, NULL, 3},
        {"exposure", required_argument, NULL, 4},
        {"analogue-gain", required_argument, NULL, 5},
        {"help", no_argument, NULL, 1},
        {NULL, 0, NULL, 0}
    };
    int option;

    while ((option = getopt_long(argc, argv, "d:o:w:h:f:n:i:t:u:",
                                 options, NULL)) != -1) {
        switch (option) {
        case 'd':
            device = optarg;
            break;
        case 'o':
            output_option = optarg;
            break;
        case 'w':
            if (parse_unsigned(optarg, &width) < 0 || width == 0U) {
                fprintf(stderr, "无效宽度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            width_explicit = 1;
            break;
        case 'h':
            if (parse_unsigned(optarg, &height) < 0 || height == 0U) {
                fprintf(stderr, "无效高度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            height_explicit = 1;
            break;
        case 'f':
            if (camera_pixel_format_parse(optarg, &requested_format) < 0) {
                fprintf(stderr, "无效像素格式: %s\n", optarg);
                return EXIT_FAILURE;
            }
            format_explicit = 1;
            break;
        case 'n':
            if (parse_unsigned(optarg, &count) < 0) {
                fprintf(stderr, "无效抓拍次数: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'i':
            if (parse_unsigned(optarg, &interval_ms) < 0) {
                fprintf(stderr, "无效间隔: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 't':
            if (parse_positive_int(optarg, &timeout_ms) < 0) {
                fprintf(stderr, "无效超时: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'u':
            if (parse_unsigned(optarg, &warmup) < 0) {
                fprintf(stderr, "无效预热帧数: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 6:
            pipeline_enabled = 1;
            break;
        case 7:
            if (parse_unsigned(optarg, &pipeline.sensor_mode_width) < 0 ||
                pipeline.sensor_mode_width == 0U) {
                fprintf(stderr, "无效 sensor mode 宽度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 8:
            if (parse_unsigned(optarg, &pipeline.sensor_mode_height) < 0 ||
                pipeline.sensor_mode_height == 0U) {
                fprintf(stderr, "无效 sensor mode 高度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 9:
            if (parse_unsigned(optarg, &pipeline.sensor_active_width) < 0 ||
                pipeline.sensor_active_width == 0U) {
                fprintf(stderr, "无效 sensor 有效宽度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 12:
            if (parse_unsigned(optarg, &pipeline.sensor_active_height) < 0 ||
                pipeline.sensor_active_height == 0U) {
                fprintf(stderr, "无效 sensor 有效高度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 10:
            if (parse_unsigned(optarg, &pipeline.crop_width) < 0 ||
                pipeline.crop_width == 0U) {
                fprintf(stderr, "无效裁剪宽度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 11:
            if (parse_unsigned(optarg, &pipeline.crop_height) < 0 ||
                pipeline.crop_height == 0U) {
                fprintf(stderr, "无效裁剪高度: %s\n", optarg);
                return EXIT_FAILURE;
            }
            pipeline_enabled = 1;
            break;
        case 2:
            control_device = optarg;
            break;
        case 3:
            if (parse_signed_int(optarg, &controls.brightness) < 0) {
                fprintf(stderr, "无效 brightness: %s\n", optarg);
                return EXIT_FAILURE;
            }
            controls.has_brightness = 1;
            break;
        case 4:
            if (parse_signed_int(optarg, &controls.exposure) < 0) {
                fprintf(stderr, "无效 exposure: %s\n", optarg);
                return EXIT_FAILURE;
            }
            controls.has_exposure = 1;
            break;
        case 5:
            if (parse_signed_int(optarg, &controls.analogue_gain) < 0) {
                fprintf(stderr, "无效 analogue gain: %s\n", optarg);
                return EXIT_FAILURE;
            }
            controls.has_analogue_gain = 1;
            break;
        case 1:
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (pipeline_enabled) {
        if (!width_explicit) {
            width = 640U;
        }
        if (!height_explicit) {
            height = 640U;
        }
        if (!format_explicit) {
            requested_format = CAMERA_PIXFMT_NV12;
        }
        if (pipeline.sensor_active_width > pipeline.sensor_mode_width ||
            pipeline.sensor_active_height > pipeline.sensor_mode_height ||
            pipeline.crop_width > pipeline.sensor_active_width ||
            pipeline.crop_height > pipeline.sensor_active_height) {
            fprintf(stderr,
                    "sensor mode、有效图像区域和裁剪尺寸的关系无效\n");
            return EXIT_FAILURE;
        }
        if (requested_format != CAMERA_PIXFMT_AUTO &&
            requested_format != CAMERA_PIXFMT_NV12) {
            fprintf(stderr, "pipeline 模式的 RKISP 输入格式必须是 nv12 或 auto\n");
            return EXIT_FAILURE;
        }
    }
    if (count != 1U && interval_ms == 0U) {
        fprintf(stderr, "连续抓拍时 interval-ms 不能为 0\n");
        return EXIT_FAILURE;
    }

    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);
    if (camera_capture_init_ex(
            &capture, device, width, height, requested_format, warmup,
            timeout_ms, control_device, &controls,
            pipeline_enabled ? &pipeline : NULL) < 0) {
        fprintf(stderr, "打开摄像头失败: %s: %s\n", device, strerror(errno));
        return EXIT_FAILURE;
    }
    directory = program_directory(argv[0]);
    if (directory == NULL) {
        fprintf(stderr, "获取程序目录失败\n");
        camera_capture_close(&capture);
        return EXIT_FAILURE;
    }
    if (output_option != NULL) {
        output_base = strdup(output_option);
    } else {
        output_base = default_output_path(directory, capture.pixfmt);
    }
    if (output_base == NULL) {
        fprintf(stderr, "分配输出路径失败\n");
        free(directory);
        camera_capture_close(&capture);
        return EXIT_FAILURE;
    }

    printf("device=%s buffer=%s planes=%u format=%s size=%ux%u output=%s\n",
           device,
           capture.buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
               ? "mplane" : "single-plane",
           capture.plane_count,
           camera_pixel_format_name(capture.pixfmt), capture.width,
           capture.height, output_base);
    if (pipeline_enabled) {
        printf("pipeline=sensor-mode:%ux%u active:%ux%u(kernel-configured) "
               "mainpath-crop:%u,%u/%ux%u rkisp-output:%ux%u NV12 "
               "frame-output=RGB24(RGA)\n",
               pipeline.sensor_mode_width, pipeline.sensor_mode_height,
               pipeline.sensor_active_width, pipeline.sensor_active_height,
               capture.crop_left, capture.crop_top, capture.crop_width,
               capture.crop_height, capture.width, capture.height);
    }
    while (!stop_requested && (count == 0U || sample < count)) {
        camera_frame_t frame;
        uint8_t *image = NULL;
        size_t image_size = 0U;
        uint32_t image_format = 0U;
        char *path;
        ++sample;
        if (camera_capture_read(&capture, &frame) < 0) {
            fprintf(stderr, "第 %u 次抓拍失败: %s (%d)\n", sample,
                    strerror(errno), errno);
            break;
        }
        if (camera_frame_to_image(&frame, &image, &image_size,
                                  &image_format) < 0) {
            fprintf(stderr, "第 %u 次图像转换失败: %s (%d)\n", sample,
                    strerror(errno), errno);
            camera_frame_release(&frame);
            break;
        }
        path = numbered_output_path(output_base, sample, count);
        if (path == NULL || save_image(path, image, image_size) < 0) {
            fprintf(stderr, "保存第 %u 张图片失败: %s\n", sample,
                    path == NULL ? "内存不足" : strerror(errno));
            free(path);
            free(image);
            camera_frame_release(&frame);
            break;
        }
        printf("sample=%u saved=%s format=%s bytes=%zu\n", sample, path,
               camera_pixel_format_name(image_format), image_size);
        free(path);
        free(image);
        camera_frame_release(&frame);
        exit_code = EXIT_SUCCESS;
        if (!stop_requested && (count == 0U || sample < count) &&
            interval_ms > 0U) {
            usleep((useconds_t)interval_ms * 1000U);
        }
    }
    free(output_base);
    free(directory);
    camera_capture_close(&capture);
    return exit_code;
}
