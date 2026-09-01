#define _GNU_SOURCE
#include "camera_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <poll.h>
#include <rga/im2d.h>
#include <stdint.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int set_errno(int value)
{
    errno = value;
    return -1;
}

static int is_supported_raw(uint32_t pixfmt)
{
    return pixfmt == V4L2_PIX_FMT_MJPEG ||
           pixfmt == V4L2_PIX_FMT_YUYV ||
           pixfmt == V4L2_PIX_FMT_NV12 ||
           pixfmt == V4L2_PIX_FMT_NV12M ||
           pixfmt == V4L2_PIX_FMT_YUV420 ||
           pixfmt == V4L2_PIX_FMT_YUV420M ||
           pixfmt == V4L2_PIX_FMT_RGB24;
}

static uint32_t requested_fourcc(camera_pixel_format_t format)
{
    switch (format) {
    case CAMERA_PIXFMT_MJPEG:
        return V4L2_PIX_FMT_MJPEG;
    case CAMERA_PIXFMT_YUYV:
        return V4L2_PIX_FMT_YUYV;
    case CAMERA_PIXFMT_NV12:
        return V4L2_PIX_FMT_NV12;
    case CAMERA_PIXFMT_YUV420:
        return V4L2_PIX_FMT_YUV420;
    case CAMERA_PIXFMT_RGB24:
        return V4L2_PIX_FMT_RGB24;
    case CAMERA_PIXFMT_AUTO:
    default:
        return 0U;
    }
}

int camera_pixel_format_parse(const char *text, camera_pixel_format_t *format)
{
    if (text == NULL || format == NULL) {
        return set_errno(EINVAL);
    }
    if (strcasecmp(text, "auto") == 0) {
        *format = CAMERA_PIXFMT_AUTO;
    } else if (strcasecmp(text, "mjpeg") == 0 ||
               strcasecmp(text, "jpeg") == 0) {
        *format = CAMERA_PIXFMT_MJPEG;
    } else if (strcasecmp(text, "yuyv") == 0) {
        *format = CAMERA_PIXFMT_YUYV;
    } else if (strcasecmp(text, "nv12") == 0) {
        *format = CAMERA_PIXFMT_NV12;
    } else if (strcasecmp(text, "yuv420") == 0 ||
               strcasecmp(text, "i420") == 0) {
        *format = CAMERA_PIXFMT_YUV420;
    } else if (strcasecmp(text, "rgb24") == 0) {
        *format = CAMERA_PIXFMT_RGB24;
    } else {
        return set_errno(EINVAL);
    }
    return 0;
}

const char *camera_pixel_format_name(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_MJPEG:
        return "MJPEG";
    case V4L2_PIX_FMT_YUYV:
        return "YUYV";
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV12M:
        return "NV12";
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YUV420M:
        return "YUV420";
    case V4L2_PIX_FMT_RGB24:
        return "RGB24";
    default:
        return "unknown";
    }
}

const char *camera_pixel_format_extension(uint32_t pixfmt)
{
    return pixfmt == V4L2_PIX_FMT_MJPEG ? "jpg" : "ppm";
}

static int enum_has_format(int fd, enum v4l2_buf_type type, uint32_t pixfmt)
{
    struct v4l2_fmtdesc description;
    unsigned int index = 0U;

    for (;;) {
        memset(&description, 0, sizeof(description));
        description.type = type;
        description.index = index++;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &description) < 0) {
            if (errno == EINVAL) {
                return 0;
            }
            return -1;
        }
        if (description.pixelformat == pixfmt) {
            return 1;
        }
    }
}

static int choose_auto_format(int fd, enum v4l2_buf_type type,
                              uint32_t *pixfmt)
{
    static const uint32_t preferred[] = {
        V4L2_PIX_FMT_MJPEG,
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV12M,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_YUV420M,
        V4L2_PIX_FMT_RGB24
    };
    size_t index;

    for (index = 0U; index < sizeof(preferred) / sizeof(preferred[0]); ++index) {
        const int present = enum_has_format(fd, type, preferred[index]);
        if (present < 0) {
            return -1;
        }
        if (present > 0) {
            *pixfmt = preferred[index];
            return 0;
        }
    }
    return set_errno(ENOTSUP);
}

static void init_buffer(struct v4l2_buffer *buffer,
                        struct v4l2_plane *planes,
                        const camera_capture_t *capture,
                        unsigned int index)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->type = capture->buffer_type;
    buffer->memory = V4L2_MEMORY_MMAP;
    buffer->index = index;
    if (capture->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        memset(planes, 0, sizeof(*planes) * VIDEO_MAX_PLANES);
        buffer->length = capture->plane_count;
        buffer->m.planes = planes;
    }
}

static int control_value_valid(const struct v4l2_queryctrl *query, int value)
{
    const int64_t min = query->minimum;
    const int64_t max = query->maximum;
    const int64_t step = query->step > 0 ? query->step : 1;
    if ((int64_t)value < min || (int64_t)value > max) {
        return 0;
    }
    return (((int64_t)value - min) % step) == 0;
}

static int set_v4l2_control(int fd, uint32_t id, int value, const char *name)
{
    struct v4l2_queryctrl query;
    struct v4l2_control control;

    memset(&query, 0, sizeof(query));
    query.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &query) < 0) {
        return -1;
    }
    if ((query.flags & V4L2_CTRL_FLAG_DISABLED) != 0U) {
        return set_errno(ENOTSUP);
    }
    if (!control_value_valid(&query, value)) {
        (void)name;
        return set_errno(ERANGE);
    }
    memset(&control, 0, sizeof(control));
    control.id = id;
    control.value = value;
    if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        return -1;
    }
    return 0;
}

static int contains_case_insensitive(const char *text, const char *needle)
{
    size_t index;
    size_t needle_length;
    if (text == NULL || needle == NULL) {
        return 0;
    }
    needle_length = strlen(needle);
    if (needle_length == 0U) {
        return 1;
    }
    for (; *text != '\0'; ++text) {
        for (index = 0U; index < needle_length && text[index] != '\0'; ++index) {
            if (tolower((unsigned char)text[index]) !=
                tolower((unsigned char)needle[index])) {
                break;
            }
        }
        if (index == needle_length) {
            return 1;
        }
    }
    return 0;
}

static int open_auto_sensor_device(void)
{
    DIR *directory;
    struct dirent *entry;

    directory = opendir("/dev");
    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char sysfs_path[PATH_MAX];
        char name[128];
        int name_fd;
        int fd;
        ssize_t length;
        if (strncmp(entry->d_name, "v4l-subdev", 10U) != 0) {
            continue;
        }
        if (snprintf(sysfs_path, sizeof(sysfs_path),
                     "/sys/class/video4linux/%s/name", entry->d_name) < 0) {
            continue;
        }
        name_fd = open(sysfs_path, O_RDONLY | O_CLOEXEC);
        if (name_fd < 0) {
            continue;
        }
        length = read(name_fd, name, sizeof(name) - 1U);
        (void)close(name_fd);
        if (length <= 0) {
            continue;
        }
        name[length] = '\0';
        if (!contains_case_insensitive(name, "imx415")) {
            continue;
        }
        if (snprintf(sysfs_path, sizeof(sysfs_path), "/dev/%s", entry->d_name) < 0) {
            continue;
        }
        fd = open(sysfs_path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            (void)closedir(directory);
            return fd;
        }
    }
    (void)closedir(directory);
    return set_errno(ENODEV);
}

static int open_sensor_device(const char *sensor_device)
{
    if (sensor_device == NULL || sensor_device[0] == '\0' ||
        strcasecmp(sensor_device, "auto") == 0) {
        return open_auto_sensor_device();
    }
    if (strcasecmp(sensor_device, "none") == 0) {
        return set_errno(ENODEV);
    }
    return open(sensor_device, O_RDWR | O_CLOEXEC);
}

static int open_control_device(const char *control_device,
                               int need_sensor_controls,
                               int sensor_fd)
{
    if (!need_sensor_controls) {
        return -1;
    }
    if (control_device == NULL || control_device[0] == '\0' ||
        strcasecmp(control_device, "auto") == 0) {
        if (sensor_fd >= 0) {
            return dup(sensor_fd);
        }
        return open_auto_sensor_device();
    }
    if (strcasecmp(control_device, "none") == 0) {
        return set_errno(ENODEV);
    }
    return open(control_device, O_RDWR | O_CLOEXEC);
}

static int configure_sensor_format(int fd, unsigned int width,
                                   unsigned int height)
{
    struct v4l2_subdev_format format;

    memset(&format, 0, sizeof(format));
    format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    format.pad = 0U;
    format.format.width = width;
    format.format.height = height;
    format.format.code = MEDIA_BUS_FMT_SGBRG10_1X10;
    format.format.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &format) < 0) {
        return -1;
    }
    if (format.format.width != width || format.format.height != height ||
        format.format.code != MEDIA_BUS_FMT_SGBRG10_1X10) {
        return set_errno(ERANGE);
    }
    return 0;
}

static int configure_center_crop(camera_capture_t *capture,
                                 unsigned int crop_width,
                                 unsigned int crop_height)
{
    struct v4l2_selection selection;
    struct v4l2_rect bounds;
    unsigned int centered_left;
    unsigned int centered_top;

    memset(&selection, 0, sizeof(selection));
    selection.type = capture->buffer_type;
    selection.target = V4L2_SEL_TGT_CROP_BOUNDS;
    if (xioctl(capture->fd, VIDIOC_G_SELECTION, &selection) < 0) {
        return -1;
    }
    bounds = selection.r;
    if (bounds.left < 0 || bounds.top < 0 ||
        bounds.width < crop_width || bounds.height < crop_height) {
        return set_errno(ERANGE);
    }

    centered_left = (unsigned int)bounds.left +
                    ((unsigned int)bounds.width - crop_width) / 2U;
    centered_top = (unsigned int)bounds.top +
                   ((unsigned int)bounds.height - crop_height) / 2U;
    centered_left &= ~1U;
    centered_top &= ~1U;

    memset(&selection, 0, sizeof(selection));
    selection.type = capture->buffer_type;
    selection.target = V4L2_SEL_TGT_CROP;
    selection.r.left = (int32_t)centered_left;
    selection.r.top = (int32_t)centered_top;
    selection.r.width = crop_width;
    selection.r.height = crop_height;
    if (xioctl(capture->fd, VIDIOC_S_SELECTION, &selection) < 0) {
        return -1;
    }
    if (selection.r.left < 0 || selection.r.top < 0 ||
        selection.r.width != crop_width ||
        selection.r.height != crop_height) {
        return set_errno(ERANGE);
    }
    capture->crop_left = (unsigned int)selection.r.left;
    capture->crop_top = (unsigned int)selection.r.top;
    capture->crop_width = (unsigned int)selection.r.width;
    capture->crop_height = (unsigned int)selection.r.height;
    return 0;
}

static int apply_camera_controls(camera_capture_t *capture)
{
    int target_fd;
    int result;
    if (capture->controls.has_brightness) {
        result = set_v4l2_control(capture->fd, V4L2_CID_BRIGHTNESS,
                                  capture->controls.brightness, "brightness");
        if (result < 0 && capture->control_fd >= 0 &&
            (errno == EINVAL || errno == ENOTTY || errno == ENOTSUP)) {
            result = set_v4l2_control(capture->control_fd, V4L2_CID_BRIGHTNESS,
                                      capture->controls.brightness, "brightness");
        }
        if (result < 0 && capture->sensor_fd >= 0 &&
            capture->control_fd < 0 &&
            (errno == EINVAL || errno == ENOTTY || errno == ENOTSUP)) {
            result = set_v4l2_control(capture->sensor_fd, V4L2_CID_BRIGHTNESS,
                                      capture->controls.brightness, "brightness");
        }
        if (result < 0) {
            return -1;
        }
    }
    target_fd = capture->control_fd >= 0 ? capture->control_fd : capture->fd;
    if (capture->control_fd < 0 && capture->sensor_fd >= 0) {
        target_fd = capture->sensor_fd;
    }
    if (capture->controls.has_exposure &&
        set_v4l2_control(target_fd, V4L2_CID_EXPOSURE,
                         capture->controls.exposure, "exposure") < 0) {
        return -1;
    }
    if (capture->controls.has_analogue_gain &&
        set_v4l2_control(target_fd, V4L2_CID_ANALOGUE_GAIN,
                         capture->controls.analogue_gain, "analogue_gain") < 0) {
        return -1;
    }
    return 0;
}

static void cleanup_buffers(camera_capture_t *capture)
{
    unsigned int index;
    unsigned int plane;

    if (capture->buffers != NULL) {
        for (index = 0U; index < capture->buffer_count; ++index) {
            for (plane = 0U; plane < VIDEO_MAX_PLANES; ++plane) {
                if (capture->buffers[index].start[plane] != NULL &&
                    capture->buffers[index].start[plane] != MAP_FAILED &&
                    capture->buffers[index].length[plane] != 0U) {
                    (void)munmap(capture->buffers[index].start[plane],
                                 capture->buffers[index].length[plane]);
                }
            }
        }
        free(capture->buffers);
    }
    capture->buffers = NULL;
    capture->buffer_count = 0U;
}

void camera_capture_close(camera_capture_t *capture)
{
    if (capture == NULL) {
        return;
    }
    if (capture->fd >= 0 && capture->streaming) {
        enum v4l2_buf_type type = capture->buffer_type;
        (void)xioctl(capture->fd, VIDIOC_STREAMOFF, &type);
        capture->streaming = 0;
    }
    cleanup_buffers(capture);
    if (capture->control_fd >= 0) {
        (void)close(capture->control_fd);
    }
    if (capture->sensor_fd >= 0) {
        (void)close(capture->sensor_fd);
    }
    if (capture->cancel_fd >= 0) {
        (void)close(capture->cancel_fd);
    }
    if (capture->fd >= 0) {
        (void)close(capture->fd);
    }
    memset(capture, 0, sizeof(*capture));
    capture->fd = -1;
    capture->sensor_fd = -1;
    capture->control_fd = -1;
    capture->cancel_fd = -1;
}

int camera_capture_init(
    camera_capture_t *capture,
    const char *device,
    unsigned int width,
    unsigned int height,
    camera_pixel_format_t requested_format,
    unsigned int warmup_frames,
    int timeout_ms,
    const char *control_device,
    const camera_control_settings_t *controls)
{
    return camera_capture_init_ex(capture, device, width, height,
                                  requested_format, warmup_frames, timeout_ms,
                                  control_device, controls, NULL);
}

int camera_capture_init_ex(
    camera_capture_t *capture,
    const char *device,
    unsigned int width,
    unsigned int height,
    camera_pixel_format_t requested_format,
    unsigned int warmup_frames,
    int timeout_ms,
    const char *control_device,
    const camera_control_settings_t *controls,
    const camera_pipeline_settings_t *pipeline)
{
    struct v4l2_capability capability;
    struct v4l2_format format;
    struct v4l2_requestbuffers request;
    enum v4l2_buf_type type;
    uint32_t fourcc;
    unsigned int index;
    unsigned int plane;
    uint32_t device_capabilities;

    const int use_pipeline = pipeline != NULL;

    if (capture == NULL || device == NULL || device[0] == '\0' ||
        width == 0U || height == 0U || timeout_ms <= 0) {
        return set_errno(EINVAL);
    }
    if (use_pipeline &&
        (pipeline->sensor_width == 0U || pipeline->sensor_height == 0U ||
         pipeline->crop_width == 0U || pipeline->crop_height == 0U ||
         pipeline->crop_width > pipeline->sensor_width ||
         pipeline->crop_height > pipeline->sensor_height)) {
        return set_errno(EINVAL);
    }
    if (use_pipeline && pipeline->rga_rgb24 &&
        requested_format != CAMERA_PIXFMT_AUTO &&
        requested_format != CAMERA_PIXFMT_NV12) {
        return set_errno(EINVAL);
    }
    memset(capture, 0, sizeof(*capture));
    capture->fd = -1;
    capture->sensor_fd = -1;
    capture->control_fd = -1;
    capture->cancel_fd = -1;
    memset(&capture->controls, 0, sizeof(capture->controls));
    if (controls != NULL) {
        capture->controls = *controls;
    }
    if (use_pipeline) {
        capture->sensor_fd = open_sensor_device(pipeline->sensor_device);
        if (capture->sensor_fd < 0) {
            return -1;
        }
        if (configure_sensor_format(capture->sensor_fd,
                                    pipeline->sensor_width,
                                    pipeline->sensor_height) < 0) {
            const int saved = errno;
            camera_capture_close(capture);
            return set_errno(saved);
        }
        capture->rga_rgb24 = pipeline->rga_rgb24 != 0;
    }
    capture->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (capture->fd < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    capture->cancel_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (capture->cancel_fd < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }

    memset(&capability, 0, sizeof(capability));
    if (xioctl(capture->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    device_capabilities = capability.capabilities;
    if ((device_capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
        device_capabilities = capability.device_caps;
    }
    if ((device_capabilities & V4L2_CAP_STREAMING) == 0U) {
        camera_capture_close(capture);
        return set_errno(ENOTSUP);
    }
    if ((device_capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if ((device_capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        camera_capture_close(capture);
        return set_errno(ENOTSUP);
    }
    capture->buffer_type = type;

    if (use_pipeline &&
        configure_center_crop(capture, pipeline->crop_width,
                              pipeline->crop_height) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }

    fourcc = capture->rga_rgb24 ? V4L2_PIX_FMT_NV12
                                : requested_fourcc(requested_format);
    if (fourcc == 0U && choose_auto_format(capture->fd, type, &fourcc) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    if (!is_supported_raw(fourcc)) {
        camera_capture_close(capture);
        return set_errno(ENOTSUP);
    }

    memset(&format, 0, sizeof(format));
    format.type = type;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        format.fmt.pix_mp.width = width;
        format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.pixelformat = fourcc;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    } else {
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = fourcc;
        format.fmt.pix.field = V4L2_FIELD_ANY;
    }
    if (xioctl(capture->fd, VIDIOC_S_FMT, &format) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }

    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        if (format.fmt.pix_mp.num_planes == 0U ||
            format.fmt.pix_mp.num_planes > VIDEO_MAX_PLANES ||
            !is_supported_raw(format.fmt.pix_mp.pixelformat)) {
            camera_capture_close(capture);
            return set_errno(ENOTSUP);
        }
        capture->width = format.fmt.pix_mp.width;
        capture->height = format.fmt.pix_mp.height;
        capture->pixfmt = format.fmt.pix_mp.pixelformat;
        capture->plane_count = format.fmt.pix_mp.num_planes;
        for (plane = 0U; plane < capture->plane_count; ++plane) {
            capture->plane_bytesperline[plane] =
                format.fmt.pix_mp.plane_fmt[plane].bytesperline;
        }
    } else {
        if (!is_supported_raw(format.fmt.pix.pixelformat)) {
            camera_capture_close(capture);
            return set_errno(ENOTSUP);
        }
        capture->width = format.fmt.pix.width;
        capture->height = format.fmt.pix.height;
        capture->pixfmt = format.fmt.pix.pixelformat;
        capture->plane_count = 1U;
        capture->plane_bytesperline[0] = format.fmt.pix.bytesperline;
    }
    capture->bytesperline = capture->plane_bytesperline[0];
    if (capture->rga_rgb24 &&
        (capture->width != width || capture->height != height ||
         capture->pixfmt != V4L2_PIX_FMT_NV12 || capture->plane_count != 1U ||
         capture->bytesperline < capture->width)) {
        camera_capture_close(capture);
        return set_errno(ERANGE);
    }
    capture->control_fd = open_control_device(
        control_device,
        capture->controls.has_exposure || capture->controls.has_analogue_gain,
        capture->sensor_fd);
    if ((capture->controls.has_exposure || capture->controls.has_analogue_gain) &&
        capture->control_fd < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    /*
     * Do not apply sensor controls until STREAMON.  The IMX415 driver only
     * writes exposure/gain while the sensor is runtime-powered; before
     * streaming, VIDIOC_S_CTRL can appear to succeed without touching the
     * sensor registers.
     */
    capture->timeout_ms = timeout_ms;
    capture->warmup_frames = warmup_frames;

    memset(&request, 0, sizeof(request));
    request.count = 4U;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(capture->fd, VIDIOC_REQBUFS, &request) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    if (request.count < 2U) {
        camera_capture_close(capture);
        return set_errno(ENOMEM);
    }

    capture->buffers = calloc(request.count, sizeof(*capture->buffers));
    if (capture->buffers == NULL) {
        camera_capture_close(capture);
        return -1;
    }
    capture->buffer_count = request.count;
    for (index = 0U; index < capture->buffer_count; ++index) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        init_buffer(&buffer, planes, capture, index);
        if (xioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            const int saved = errno;
            camera_capture_close(capture);
            return set_errno(saved);
        }
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            for (plane = 0U; plane < capture->plane_count; ++plane) {
                if (planes[plane].length == 0U) {
                    camera_capture_close(capture);
                    return set_errno(EIO);
                }
                capture->buffers[index].length[plane] = planes[plane].length;
                capture->buffers[index].start[plane] = mmap(
                    NULL, planes[plane].length, PROT_READ | PROT_WRITE,
                    MAP_SHARED, capture->fd,
                    (off_t)planes[plane].m.mem_offset);
                if (capture->buffers[index].start[plane] == MAP_FAILED) {
                    const int saved = errno;
                    camera_capture_close(capture);
                    return set_errno(saved);
                }
            }
        } else {
            if (buffer.length == 0U) {
                camera_capture_close(capture);
                return set_errno(EIO);
            }
            capture->buffers[index].length[0] = buffer.length;
            capture->buffers[index].start[0] = mmap(
                NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                capture->fd, (off_t)buffer.m.offset);
            if (capture->buffers[index].start[0] == MAP_FAILED) {
                const int saved = errno;
                camera_capture_close(capture);
                return set_errno(saved);
            }
        }
    }

    for (index = 0U; index < capture->buffer_count; ++index) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        init_buffer(&buffer, planes, capture, index);
        if (xioctl(capture->fd, VIDIOC_QBUF, &buffer) < 0) {
            const int saved = errno;
            camera_capture_close(capture);
            return set_errno(saved);
        }
    }
    type = capture->buffer_type;
    if (xioctl(capture->fd, VIDIOC_STREAMON, &type) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    capture->streaming = 1;
    if (apply_camera_controls(capture) < 0) {
        const int saved = errno;
        camera_capture_close(capture);
        return set_errno(saved);
    }
    return 0;
}

static int wait_for_frame(camera_capture_t *capture)
{
    struct pollfd descriptors[2];
    int result;
    uint64_t value;

    descriptors[0].fd = capture->fd;
    descriptors[0].events = POLLIN | POLLPRI;
    descriptors[0].revents = 0;
    descriptors[1].fd = capture->cancel_fd;
    descriptors[1].events = POLLIN;
    descriptors[1].revents = 0;
    do {
        result = poll(descriptors, 2, capture->timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return set_errno(ETIMEDOUT);
    }
    if (result < 0) {
        return -1;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
        const ssize_t read_result =
            read(capture->cancel_fd, &value, sizeof(value));
        if (read_result < 0 && errno != EAGAIN) {
            return -1;
        }
        return set_errno(ECANCELED);
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return set_errno(EIO);
    }
    if ((descriptors[0].revents & (POLLIN | POLLPRI)) == 0) {
        return set_errno(EIO);
    }
    return 0;
}

void camera_capture_cancel(camera_capture_t *capture)
{
    const uint64_t value = 1U;
    if (capture == NULL || capture->cancel_fd < 0) {
        return;
    }
    if (write(capture->cancel_fd, &value, sizeof(value)) < 0 &&
        errno != EAGAIN) {
        return;
    }
}

static int dequeue_buffer(camera_capture_t *capture,
                          struct v4l2_buffer *buffer,
                          struct v4l2_plane *planes)
{
    init_buffer(buffer, planes, capture, 0U);
    return xioctl(capture->fd, VIDIOC_DQBUF, buffer);
}

static int queue_buffer(camera_capture_t *capture,
                        unsigned int index,
                        struct v4l2_plane *planes)
{
    struct v4l2_buffer buffer;
    init_buffer(&buffer, planes, capture, index);
    return xioctl(capture->fd, VIDIOC_QBUF, &buffer);
}

static int discard_one(camera_capture_t *capture)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    if (dequeue_buffer(capture, &buffer, planes) < 0) {
        return -1;
    }
    if (buffer.index >= capture->buffer_count) {
        return set_errno(EIO);
    }
    return queue_buffer(capture, buffer.index, planes);
}

static int ensure_frame_capacity(camera_frame_t *frame, size_t size, int reuse)
{
    uint8_t *storage;

    if (reuse && frame->data != NULL && frame->capacity >= size) {
        return 0;
    }
    storage = reuse ? realloc(frame->data, size) : malloc(size);
    if (storage == NULL) {
        return -1;
    }
    frame->data = storage;
    frame->capacity = size;
    return 0;
}

static int convert_nv12_to_rgb24_rga(camera_capture_t *capture,
                                     camera_frame_t *frame,
                                     const struct v4l2_buffer *buffer,
                                     const struct v4l2_plane *planes,
                                     int reuse)
{
    const uint8_t *source;
    size_t source_size;
    size_t required_source_size;
    size_t output_size;
    rga_buffer_t source_buffer;
    rga_buffer_t destination_buffer;
    IM_STATUS status;

    if (capture->pixfmt != V4L2_PIX_FMT_NV12 ||
        capture->plane_count != 1U || capture->bytesperline < capture->width ||
        capture->width > (unsigned int)INT_MAX ||
        capture->height > (unsigned int)INT_MAX ||
        capture->bytesperline > (unsigned int)INT_MAX ||
        (capture->width & 1U) != 0U || (capture->height & 1U) != 0U) {
        return set_errno(ENOTSUP);
    }
    if ((size_t)capture->bytesperline > SIZE_MAX / capture->height) {
        return set_errno(EOVERFLOW);
    }
    required_source_size = (size_t)capture->bytesperline * capture->height;
    if (required_source_size > SIZE_MAX - required_source_size / 2U) {
        return set_errno(EOVERFLOW);
    }
    required_source_size += required_source_size / 2U;

    if (capture->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        const size_t data_offset = planes[0].data_offset;
        const size_t used = planes[0].bytesused;
        if (data_offset > used ||
            used > capture->buffers[buffer->index].length[0]) {
            return set_errno(EIO);
        }
        source = (const uint8_t *)capture->buffers[buffer->index].start[0] +
                 data_offset;
        source_size = used - data_offset;
    } else {
        source = (const uint8_t *)capture->buffers[buffer->index].start[0];
        source_size = buffer->bytesused;
    }
    if (source_size < required_source_size) {
        return set_errno(EIO);
    }
    if ((size_t)capture->width > SIZE_MAX / capture->height ||
        (size_t)capture->width * capture->height > SIZE_MAX / 3U) {
        return set_errno(EOVERFLOW);
    }
    output_size = (size_t)capture->width * capture->height * 3U;
    if (ensure_frame_capacity(frame, output_size, reuse) < 0) {
        return -1;
    }

    source_buffer = wrapbuffer_virtualaddr_t(
        (void *)source, (int)capture->width, (int)capture->height,
        (int)capture->bytesperline, (int)capture->height,
        RK_FORMAT_YCbCr_420_SP);
    destination_buffer = wrapbuffer_virtualaddr_t(
        frame->data, (int)capture->width, (int)capture->height,
        (int)capture->width, (int)capture->height, RK_FORMAT_RGB_888);
    status = imcvtcolor_t(source_buffer, destination_buffer,
                          RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888,
                          IM_YUV_TO_RGB_BT601_FULL, 1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        fprintf(stderr, "camera: RGA NV12 to RGB24 failed: %s (%d)\n",
                imStrError_t(status), (int)status);
        return set_errno(EIO);
    }

    frame->size = output_size;
    frame->width = capture->width;
    frame->height = capture->height;
    frame->bytesperline = capture->width * 3U;
    frame->plane_count = 1U;
    frame->plane_offset[0] = 0U;
    frame->plane_bytesperline[0] = frame->bytesperline;
    frame->pixfmt = V4L2_PIX_FMT_RGB24;
    return 0;
}

static int camera_capture_read_internal(camera_capture_t *capture,
                                        camera_frame_t *frame, int reuse)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    unsigned int index;
    unsigned int plane;
    size_t total_size;
    uint8_t *copy;
    size_t copy_offset;

    if (capture == NULL || frame == NULL || capture->fd < 0 ||
        !capture->streaming) {
        return set_errno(EINVAL);
    }
    if (!reuse) {
        memset(frame, 0, sizeof(*frame));
    } else {
        frame->size = 0U;
        frame->width = 0U;
        frame->height = 0U;
        frame->bytesperline = 0U;
        frame->plane_count = 0U;
        frame->pixfmt = 0U;
        memset(frame->plane_offset, 0, sizeof(frame->plane_offset));
        memset(frame->plane_bytesperline, 0,
               sizeof(frame->plane_bytesperline));
    }
    for (index = 0U; index < capture->warmup_frames; ++index) {
        if (wait_for_frame(capture) < 0 || discard_one(capture) < 0) {
            return -1;
        }
    }
    capture->warmup_frames = 0U;

    for (;;) {
        if (wait_for_frame(capture) < 0) {
            return -1;
        }
        if (dequeue_buffer(capture, &buffer, planes) == 0) {
            break;
        }
        if (errno != EAGAIN) {
            return -1;
        }
    }
    if (buffer.index >= capture->buffer_count) {
        return set_errno(EIO);
    }

    total_size = 0U;
    if (capture->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        for (plane = 0U; plane < capture->plane_count; ++plane) {
            const size_t data_offset = planes[plane].data_offset;
            const size_t used = planes[plane].bytesused;
            const size_t length = capture->buffers[buffer.index].length[plane];
            if (used == 0U || data_offset > used || used > length ||
                total_size > SIZE_MAX - (used - data_offset)) {
                (void)queue_buffer(capture, buffer.index, planes);
                return set_errno(EIO);
            }
            total_size += used - data_offset;
        }
    } else {
        if (buffer.bytesused == 0U ||
            (size_t)buffer.bytesused > capture->buffers[buffer.index].length[0]) {
            (void)queue_buffer(capture, buffer.index, planes);
            return set_errno(EIO);
        }
        total_size = buffer.bytesused;
    }

    if (capture->rga_rgb24) {
        const int conversion_result = convert_nv12_to_rgb24_rga(
            capture, frame, &buffer, planes, reuse);
        const int conversion_errno = errno;
        if (queue_buffer(capture, buffer.index, planes) < 0) {
            if (!reuse) {
                camera_frame_release(frame);
            }
            return -1;
        }
        if (conversion_result < 0) {
            if (!reuse) {
                camera_frame_release(frame);
            }
            return set_errno(conversion_errno);
        }
        return 0;
    }

    if (ensure_frame_capacity(frame, total_size, reuse) < 0) {
        const int saved = errno;
        (void)queue_buffer(capture, buffer.index, planes);
        return set_errno(saved);
    }
    copy = frame->data;

    copy_offset = 0U;
    if (capture->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        for (plane = 0U; plane < capture->plane_count; ++plane) {
            const size_t data_offset = planes[plane].data_offset;
            const size_t used = planes[plane].bytesused - data_offset;
            const uint8_t *source =
                (const uint8_t *)capture->buffers[buffer.index].start[plane] +
                data_offset;
            memcpy(copy + copy_offset, source, used);
            frame->plane_offset[plane] = copy_offset;
            frame->plane_bytesperline[plane] =
                capture->plane_bytesperline[plane];
            copy_offset += used;
        }
    } else {
        memcpy(copy, capture->buffers[buffer.index].start[0], total_size);
        frame->plane_offset[0] = 0U;
        frame->plane_bytesperline[0] = capture->plane_bytesperline[0];
    }
    if (queue_buffer(capture, buffer.index, planes) < 0) {
        if (!reuse) {
            free(frame->data);
            memset(frame, 0, sizeof(*frame));
        }
        return -1;
    }

    frame->size = total_size;
    frame->width = capture->width;
    frame->height = capture->height;
    frame->bytesperline = capture->bytesperline;
    frame->plane_count = capture->plane_count;
    frame->pixfmt = capture->pixfmt;
    return 0;
}

int camera_capture_read(camera_capture_t *capture, camera_frame_t *frame)
{
    return camera_capture_read_internal(capture, frame, 0);
}

int camera_capture_read_reuse(camera_capture_t *capture, camera_frame_t *frame)
{
    return camera_capture_read_internal(capture, frame, 1);
}

void camera_frame_release(camera_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

static unsigned char clamp_u8(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 255) {
        return 255U;
    }
    return (unsigned char)value;
}

static void yuv_to_rgb(unsigned char y, unsigned char u, unsigned char v,
                       unsigned char *rgb)
{
    const int c = (int)y - 16;
    const int d = (int)u - 128;
    const int e = (int)v - 128;
    rgb[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
    rgb[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgb[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

static int ppm_size(unsigned int width, unsigned int height, size_t *size)
{
    const size_t header = 32U;
    if ((size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > (SIZE_MAX - header) / 3U) {
        return set_errno(EOVERFLOW);
    }
    *size = header + (size_t)width * (size_t)height * 3U;
    return 0;
}

static int frame_plane(const camera_frame_t *frame, unsigned int plane,
                       const uint8_t **data, unsigned int *stride)
{
    if (plane >= frame->plane_count || plane >= VIDEO_MAX_PLANES ||
        frame->plane_offset[plane] >= frame->size ||
        frame->plane_bytesperline[plane] == 0U) {
        return set_errno(EIO);
    }
    *data = frame->data + frame->plane_offset[plane];
    *stride = frame->plane_bytesperline[plane];
    return 0;
}

int camera_frame_to_image_buffer(const camera_frame_t *frame, uint8_t *data,
                                 size_t capacity, size_t *size,
                                 uint32_t *image_format)
{
    size_t output_size;
    size_t header_length;
    unsigned int y;
    unsigned int x;
    const uint8_t *plane0;
    const uint8_t *plane1;
    const uint8_t *plane2;
    unsigned int stride0;
    unsigned int stride1;
    unsigned int stride2;

    if (frame == NULL || data == NULL || size == NULL || image_format == NULL ||
        frame->data == NULL || frame->width == 0U || frame->height == 0U) {
        return set_errno(EINVAL);
    }
    *size = 0U;
    if (frame->pixfmt == V4L2_PIX_FMT_MJPEG) {
        if (capacity < frame->size) {
            return set_errno(ENOSPC);
        }
        memcpy(data, frame->data, frame->size);
        *size = frame->size;
        *image_format = V4L2_PIX_FMT_MJPEG;
        return 0;
    }
    if (ppm_size(frame->width, frame->height, &output_size) < 0) {
        return -1;
    }
    if (capacity < output_size) {
        return set_errno(ENOSPC);
    }
    header_length = (size_t)snprintf((char *)data, output_size,
                                     "P6\n%u %u\n255\n",
                                     frame->width, frame->height);
    if (header_length >= output_size) {
        return set_errno(EOVERFLOW);
    }

    if (frame_plane(frame, 0U, &plane0, &stride0) < 0) {
        return -1;
    }
    if (frame->pixfmt == V4L2_PIX_FMT_YUYV) {
        if (stride0 < frame->width * 2U ||
            (size_t)stride0 * frame->height > frame->size) {
            return set_errno(EIO);
        }
        for (y = 0U; y < frame->height; ++y) {
            const uint8_t *row = plane0 + (size_t)y * stride0;
            uint8_t *dst = data + header_length +
                           (size_t)y * (size_t)frame->width * 3U;
            for (x = 0U; x + 1U < frame->width; x += 2U) {
                yuv_to_rgb(row[x * 2U], row[x * 2U + 1U], row[x * 2U + 3U],
                           dst + x * 3U);
                yuv_to_rgb(row[x * 2U + 2U], row[x * 2U + 1U], row[x * 2U + 3U],
                           dst + (x + 1U) * 3U);
            }
        }
    } else if (frame->pixfmt == V4L2_PIX_FMT_NV12 ||
               frame->pixfmt == V4L2_PIX_FMT_NV12M) {
        if (frame->plane_count >= 2U) {
            if (frame_plane(frame, 1U, &plane1, &stride1) < 0) {
                return -1;
            }
        } else {
            const size_t uv_offset = (size_t)stride0 * frame->height;
            if (uv_offset >= frame->size) {
                return set_errno(EIO);
            }
            plane1 = frame->data + uv_offset;
            stride1 = stride0;
        }
        if (stride0 < frame->width || stride1 < frame->width ||
            (size_t)stride0 * frame->height > frame->size) {
            return set_errno(EIO);
        }
        for (y = 0U; y < frame->height; ++y) {
            const uint8_t *y_row = plane0 + (size_t)y * stride0;
            const uint8_t *uv_row = plane1 + (size_t)(y / 2U) * stride1;
            uint8_t *dst = data + header_length +
                           (size_t)y * (size_t)frame->width * 3U;
            for (x = 0U; x < frame->width; ++x) {
                const unsigned int uv_x = (x / 2U) * 2U;
                if (uv_x + 1U >= stride1) {
                    return set_errno(EIO);
                }
                yuv_to_rgb(y_row[x], uv_row[uv_x], uv_row[uv_x + 1U],
                           dst + x * 3U);
            }
        }
    } else if (frame->pixfmt == V4L2_PIX_FMT_YUV420 ||
               frame->pixfmt == V4L2_PIX_FMT_YUV420M) {
        if (frame->plane_count >= 3U) {
            if (frame_plane(frame, 1U, &plane1, &stride1) < 0 ||
                frame_plane(frame, 2U, &plane2, &stride2) < 0) {
                return -1;
            }
        } else {
            const unsigned int chroma_stride = (stride0 + 1U) / 2U;
            const size_t u_offset = (size_t)stride0 * frame->height;
            const size_t v_offset = u_offset +
                                    (size_t)chroma_stride *
                                        ((frame->height + 1U) / 2U);
            if (v_offset >= frame->size) {
                return set_errno(EIO);
            }
            plane1 = frame->data + u_offset;
            plane2 = frame->data + v_offset;
            stride1 = chroma_stride;
            stride2 = chroma_stride;
        }
        if (stride0 < frame->width ||
            stride1 < (frame->width + 1U) / 2U ||
            stride2 < (frame->width + 1U) / 2U) {
            return set_errno(EIO);
        }
        for (y = 0U; y < frame->height; ++y) {
            const uint8_t *y_row = plane0 + (size_t)y * stride0;
            const uint8_t *u_row = plane1 + (size_t)(y / 2U) * stride1;
            const uint8_t *v_row = plane2 + (size_t)(y / 2U) * stride2;
            uint8_t *dst = data + header_length +
                           (size_t)y * (size_t)frame->width * 3U;
            for (x = 0U; x < frame->width; ++x) {
                yuv_to_rgb(y_row[x], u_row[x / 2U], v_row[x / 2U],
                           dst + x * 3U);
            }
        }
    } else if (frame->pixfmt == V4L2_PIX_FMT_RGB24) {
        if (stride0 < frame->width * 3U ||
            (size_t)stride0 * frame->height > frame->size) {
            return set_errno(EIO);
        }
        for (y = 0U; y < frame->height; ++y) {
            memcpy(data + header_length +
                       (size_t)y * (size_t)frame->width * 3U,
                   plane0 + (size_t)y * stride0,
                   (size_t)frame->width * 3U);
        }
    } else {
        return set_errno(ENOTSUP);
    }
    *size = header_length + (size_t)frame->width * (size_t)frame->height * 3U;
    *image_format = V4L2_PIX_FMT_RGB24;
    return 0;
}

int camera_frame_image_capacity(const camera_frame_t *frame, size_t *capacity)
{
    if (frame == NULL || capacity == NULL || frame->data == NULL ||
        frame->width == 0U || frame->height == 0U) {
        return set_errno(EINVAL);
    }
    if (frame->pixfmt == V4L2_PIX_FMT_MJPEG) {
        *capacity = frame->size;
        return 0;
    }
    if (frame->pixfmt != V4L2_PIX_FMT_YUYV &&
        frame->pixfmt != V4L2_PIX_FMT_NV12 &&
        frame->pixfmt != V4L2_PIX_FMT_NV12M &&
        frame->pixfmt != V4L2_PIX_FMT_YUV420 &&
        frame->pixfmt != V4L2_PIX_FMT_YUV420M &&
        frame->pixfmt != V4L2_PIX_FMT_RGB24) {
        return set_errno(ENOTSUP);
    }
    return ppm_size(frame->width, frame->height, capacity);
}

int camera_frame_to_image(const camera_frame_t *frame, uint8_t **data,
                          size_t *size, uint32_t *image_format)
{
    size_t capacity;
    uint8_t *output;
    if (data == NULL || size == NULL || image_format == NULL) {
        return set_errno(EINVAL);
    }
    *data = NULL;
    *size = 0U;
    if (camera_frame_image_capacity(frame, &capacity) < 0) {
        return -1;
    }
    output = malloc(capacity);
    if (output == NULL) {
        return -1;
    }
    if (camera_frame_to_image_buffer(frame, output, capacity, size,
                                     image_format) < 0) {
        const int saved = errno;
        free(output);
        return set_errno(saved);
    }
    *data = output;
    return 0;
}
