#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/ds18b20"
#define DEFAULT_COUNT 1UL
#define DEFAULT_INTERVAL_MS 1000UL
#define DS18B20_MIN_TEMP (-55.0)
#define DS18B20_MAX_TEMP 125.0

static void usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [-d device] [-n count] [-i interval_ms]\n"
            "\n"
            "  -d, --device PATH       字符设备路径，默认 %s\n"
            "  -n, --count N           读取次数，默认 %lu；0 表示持续读取\n"
            "  -i, --interval MS       两次读取之间的间隔，默认 %lu ms\n"
            "  -h, --help              显示帮助\n"
            "\n"
            "驱动返回的是一个二进制 IEEE-754 double，而不是文本。\n",
            program, DEFAULT_DEVICE, DEFAULT_COUNT, DEFAULT_INTERVAL_MS);
}

static int parse_unsigned(const char *text, unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int sleep_milliseconds(unsigned long milliseconds)
{
    struct timespec request;

    request.tv_sec = (time_t)(milliseconds / 1000UL);
    request.tv_nsec = (long)((milliseconds % 1000UL) * 1000000UL);
    while (nanosleep(&request, &request) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_temperature(int fd, double *temperature)
{
    ssize_t received;

    do {
        /* The driver returns one sample at offset 0 for every read. */
        received = pread(fd, temperature, sizeof(*temperature), 0);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        return -1;
    }
    if (received != (ssize_t)sizeof(*temperature)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"device", required_argument, NULL, 'd'},
        {"count", required_argument, NULL, 'n'},
        {"interval", required_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *device_path = DEFAULT_DEVICE;
    unsigned long count = DEFAULT_COUNT;
    unsigned long interval_ms = DEFAULT_INTERVAL_MS;
    unsigned long sample = 0;
    int fd;
    int option;
    int exit_code = EXIT_SUCCESS;

    while ((option = getopt_long(argc, argv, "d:n:i:h", options, NULL)) != -1) {
        switch (option) {
        case 'd':
            device_path = optarg;
            break;
        case 'n':
            if (parse_unsigned(optarg, &count) != 0) {
                fprintf(stderr, "无效的读取次数: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'i':
            if (parse_unsigned(optarg, &interval_ms) != 0) {
                fprintf(stderr, "无效的读取间隔: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    fd = open(device_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "打开 %s 失败: %s (%d)\n",
                device_path, strerror(errno), errno);
        return EXIT_FAILURE;
    }

    printf("正在读取 %s，count=%s，interval=%lu ms\n",
           device_path, count == 0 ? "infinite" : "finite", interval_ms);

    while (count == 0 || sample < count) {
        double temperature = 0.0;
        uint64_t bits = 0;

        if (read_temperature(fd, &temperature) != 0) {
            fprintf(stderr, "第 %lu 次读取失败: %s (%d)\n",
                    sample + 1UL, strerror(errno), errno);
            exit_code = EXIT_FAILURE;
            break;
        }

        memcpy(&bits, &temperature, sizeof(bits));
        printf("sample=%lu temperature=%.4f C raw=0x%016llx",
               sample + 1UL, temperature,
               (unsigned long long)bits);
        if (!isfinite(temperature)) {
            printf(" status=INVALID_NON_FINITE");
            exit_code = EXIT_FAILURE;
        } else if (temperature < DS18B20_MIN_TEMP ||
                   temperature > DS18B20_MAX_TEMP) {
            printf(" status=WARNING_OUT_OF_DS18B20_RANGE");
        } else {
            printf(" status=OK");
        }
        putchar('\n');
        fflush(stdout);

        ++sample;
        if ((count == 0 || sample < count) && interval_ms != 0UL) {
            if (sleep_milliseconds(interval_ms) != 0) {
                fprintf(stderr, "等待下一次读取失败: %s (%d)\n",
                        strerror(errno), errno);
                exit_code = EXIT_FAILURE;
                break;
            }
        }
    }

    if (close(fd) != 0) {
        fprintf(stderr, "关闭 %s 失败: %s (%d)\n",
                device_path, strerror(errno), errno);
        exit_code = EXIT_FAILURE;
    }
    return exit_code;
}
