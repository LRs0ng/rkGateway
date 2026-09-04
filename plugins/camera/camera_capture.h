#ifndef RK_GATEWAY_CAMERA_CAPTURE_H
#define RK_GATEWAY_CAMERA_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <linux/videodev2.h>

#define CAMERA_CONTROL_UNSET INT32_MIN

#ifdef __cplusplus
extern "C" {
#endif

typedef enum camera_pixel_format {
    CAMERA_PIXFMT_AUTO = 0,
    CAMERA_PIXFMT_MJPEG,
    CAMERA_PIXFMT_YUYV,
    CAMERA_PIXFMT_NV12,
    CAMERA_PIXFMT_YUV420,
    CAMERA_PIXFMT_RGB24
} camera_pixel_format_t;

typedef struct camera_mmap_buffer {
    void *start[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
} camera_mmap_buffer_t;

typedef struct camera_control_settings {
    int has_brightness;
    int brightness;
    int has_exposure;
    int exposure;
    int has_analogue_gain;
    int analogue_gain;
} camera_control_settings_t;

/*
 * Optional Rockchip camera pipeline configuration.  The sensor/CSI/RKISP
 * sub-device ACTIVE formats are intentionally left unchanged.  RKISP mainpath
 * performs the centered crop and output scaling, and RGA converts every
 * dequeued NV12 frame to tightly packed RGB24 before it is returned.
 */
typedef struct camera_pipeline_settings {
    /* IMX415 driver mode and its effective image area; never forced by S_FMT. */
    unsigned int sensor_mode_width;
    unsigned int sensor_mode_height;
    unsigned int sensor_active_width;
    unsigned int sensor_active_height;
    unsigned int crop_width;
    unsigned int crop_height;
    int rga_rgb24;
} camera_pipeline_settings_t;

typedef struct camera_capture {
    int fd;
    int sensor_fd;
    int control_fd;
    int cancel_fd;
    unsigned int width;
    unsigned int height;
    unsigned int bytesperline;
    unsigned int plane_count;
    unsigned int plane_bytesperline[VIDEO_MAX_PLANES];
    uint32_t pixfmt;
    unsigned int crop_left;
    unsigned int crop_top;
    unsigned int crop_width;
    unsigned int crop_height;
    enum v4l2_buf_type buffer_type;
    int timeout_ms;
    unsigned int warmup_frames;
    camera_mmap_buffer_t *buffers;
    unsigned int buffer_count;
    int streaming;
    int rga_rgb24;
    camera_control_settings_t controls;
} camera_capture_t;

typedef struct camera_frame {
    uint8_t *data;
    size_t size;
    size_t capacity;
    unsigned int width;
    unsigned int height;
    unsigned int bytesperline;
    unsigned int plane_count;
    size_t plane_offset[VIDEO_MAX_PLANES];
    unsigned int plane_bytesperline[VIDEO_MAX_PLANES];
    uint32_t pixfmt;
} camera_frame_t;

int camera_pixel_format_parse(const char *text, camera_pixel_format_t *format);
const char *camera_pixel_format_name(uint32_t pixfmt);
const char *camera_pixel_format_extension(uint32_t pixfmt);

int camera_capture_init(
    camera_capture_t *capture,
    const char *device,
    unsigned int width,
    unsigned int height,
    camera_pixel_format_t requested_format,
    unsigned int warmup_frames,
    int timeout_ms,
    const char *control_device,
    const camera_control_settings_t *controls);

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
    const camera_pipeline_settings_t *pipeline);

int camera_capture_read(camera_capture_t *capture, camera_frame_t *frame);
/* frame must be zero-initialized or previously returned by this function. */
int camera_capture_read_reuse(camera_capture_t *capture, camera_frame_t *frame);
/* Wake a thread blocked in camera_capture_read[_reuse](). */
void camera_capture_cancel(camera_capture_t *capture);
void camera_frame_release(camera_frame_t *frame);
void camera_capture_close(camera_capture_t *capture);

/* Required output capacity. MJPEG stays unchanged; raw formats become PPM. */
int camera_frame_image_capacity(const camera_frame_t *frame, size_t *capacity);
int camera_frame_to_image_buffer(const camera_frame_t *frame, uint8_t *data,
                                 size_t capacity, size_t *size,
                                 uint32_t *image_format);
/* Compatibility allocator used by camera_test and external callers. */
int camera_frame_to_image(const camera_frame_t *frame, uint8_t **data,
                          size_t *size, uint32_t *image_format);

#ifdef __cplusplus
}
#endif

#endif
