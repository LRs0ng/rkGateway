#ifndef RK_GATEWAY_SCREEN_FB_H
#define RK_GATEWAY_SCREEN_FB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct screen_fb screen_fb_t;

/* Open a Linux framebuffer device and map its scanout memory. */
screen_fb_t *screen_fb_open(const char *device);
void screen_fb_close(screen_fb_t *screen);

unsigned int screen_fb_width(const screen_fb_t *screen);
unsigned int screen_fb_height(const screen_fb_t *screen);
unsigned int screen_fb_bpp(const screen_fb_t *screen);

/* True when the framebuffer exposes 8-bit red, green and blue channels. */
int screen_fb_is_rgb888(const screen_fb_t *screen);

/*
 * Drawing is performed in a userspace shadow framebuffer.  A flush copies the
 * completed frame to the scanout framebuffer in one short operation, avoiding
 * the visible top-to-bottom update caused by drawing directly into /dev/fb0.
 *
 * Deferred mode lets callers compose an image and overlays, then expose them
 * together with one screen_fb_flush().  Immediate mode remains the default for
 * compatibility with simple callers.
 */
void screen_fb_set_deferred_present(screen_fb_t *screen, int deferred);
void screen_fb_set_wait_for_vsync(screen_fb_t *screen, int wait_for_vsync);
int screen_fb_flush(screen_fb_t *screen);

int screen_fb_clear(screen_fb_t *screen, uint32_t rgb888);

/*
 * Present an image. Supported formats:
 *   auto  - detect P6 PPM, otherwise use configured raw format
 *   ppm   - binary P6 PPM (dimensions are read from the header)
 *   rgb888 - packed RGBRGB... bytes, width * height * 3
 *   rgb24  - compatibility alias for rgb888
 *   rgb565 - little-endian RGB565 bytes, width * height * 2
 *
 * Raw formats require non-zero width and height. The image is fitted inside
 * the default (unrotated) framebuffer canvas while preserving aspect ratio
 * when fit is non-zero.
 */
int screen_fb_present(screen_fb_t *screen,
                      const uint8_t *data,
                      size_t size,
                      const char *format,
                      unsigned int width,
                      unsigned int height,
                      int fit,
                      uint32_t background_rgb888);

/*
 * Present an image in a logical canvas and rotate it into the physical
 * framebuffer. Supported rotations are 0, 90, 180 and 270 degrees clockwise.
 * For 90/270 degrees, the logical canvas is framebuffer_height x
 * framebuffer_width (1920x1080 for the 1080x1920 panel).
 */
int screen_fb_present_rotated(screen_fb_t *screen,
                              const uint8_t *data,
                              size_t size,
                              const char *format,
                              unsigned int width,
                              unsigned int height,
                              int fit,
                              uint32_t background_rgb888,
                              unsigned int rotation_degrees);

/* Draw a small 5x7 numeric string in logical coordinates. */
int screen_fb_draw_number(screen_fb_t *screen,
                          double value,
                          int x,
                          int y,
                          unsigned int scale,
                          uint32_t foreground_rgb888,
                          uint32_t background_rgb888,
                          int clear_background);

int screen_fb_draw_number_rotated(screen_fb_t *screen,
                                  double value,
                                  int x,
                                  int y,
                                  unsigned int scale,
                                  uint32_t foreground_rgb888,
                                  uint32_t background_rgb888,
                                  int clear_background,
                                  unsigned int rotation_degrees);

#ifdef __cplusplus
}
#endif

#endif
