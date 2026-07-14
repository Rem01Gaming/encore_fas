#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas_client.h"
#include "fas_elf.h"

/*
 * Mangled Surface::queueBuffer signatures seen across AOSP revisions. Tried
 * in order; add more here if resolution fails on a given device (dump the
 * real symbol with `readelf -sW libgui.so | grep queueBuffer` first).
 */
static const char *const kQueueBufferSymbols[] = {
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi",
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE",
};

static const char *event_type_name(uint32_t type) {
    switch (type) {
    case FAS_EVENT_FRAME_OK:
        return "frame_ok";
    case FAS_EVENT_SMALL_JANK:
        return "small_jank";
    case FAS_EVENT_BIG_JANK:
        return "big_jank";
    case FAS_EVENT_BOOST_SOFT:
        return "boost_soft";
    case FAS_EVENT_BOOST_HARD:
        return "boost_hard";
    default:
        return "unknown";
    }
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s set-offset <libgui.so path> [manual hex offset]\n"
        "  %s get-offset\n"
        "  %s attach <pid> [fps]\n"
        "  %s detach <ctx_id>\n"
        "  %s hint <ctx_id> <fps>\n"
        "  %s listen\n",
        argv0, argv0, argv0, argv0, argv0, argv0);
}

/**
 * @brief Resolves libgui's queueBuffer offset (unless a manual override hex
 *        offset is given) and pushes it to the driver.
 */
static int cmd_set_offset(int fd, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "set-offset requires a libgui.so path\n");
        return 1;
    }

    const char *path = argv[0];
    uint64_t offset;

    if (argc >= 2) {
        offset = strtoull(argv[1], NULL, 16);
    } else if (fas_elf_resolve_offset(path, kQueueBufferSymbols,
                                       sizeof(kQueueBufferSymbols) / sizeof(kQueueBufferSymbols[0]),
                                       &offset) != 0) {
        fprintf(stderr,
            "failed to resolve queueBuffer symbol in %s\n"
            "run `readelf -sW %s | grep queueBuffer` and pass the offset manually\n",
            path, path);
        return 1;
    }

    if (fas_client_set_libgui_offset(fd, path, offset) != 0) {
        perror("set-offset ioctl");
        return 1;
    }

    printf("libgui offset set: %s @ 0x%" PRIx64 "\n", path, offset);
    return 0;
}

static int cmd_get_offset(int fd) {
    struct fas_libgui_offset info;

    if (fas_client_get_libgui_offset(fd, &info) != 0) {
        perror("get-offset ioctl");
        return 1;
    }

    printf("path: %s\noffset: 0x%" PRIx64 "\n", info.path, (uint64_t)info.offset);
    return 0;
}

static int cmd_attach(int fd, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "attach requires a pid\n");
        return 1;
    }

    int pid = atoi(argv[0]);
    int ctx_id;

    if (fas_client_register_listener(fd, pid, &ctx_id) != 0) {
        perror("attach ioctl");
        return 1;
    }

    if (argc >= 2) {
        uint32_t fps = (uint32_t)atoi(argv[1]);
        if (fas_client_hint_frametime(fd, ctx_id, fps) != 0)
            perror("hint ioctl (listener still attached)");
    }

    printf("ctx_id: %d\n", ctx_id);
    return 0;
}

static int cmd_detach(int fd, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "detach requires a ctx_id\n");
        return 1;
    }

    if (fas_client_remove_listener(fd, atoi(argv[0])) != 0) {
        perror("detach ioctl");
        return 1;
    }

    return 0;
}

static int cmd_hint(int fd, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "hint requires a ctx_id and fps\n");
        return 1;
    }

    if (fas_client_hint_frametime(fd, atoi(argv[0]), (uint32_t)atoi(argv[1])) != 0) {
        perror("hint ioctl");
        return 1;
    }

    return 0;
}

static int cmd_listen(int fd) {
    struct fas_jank_event ev;

    while (fas_client_read_event(fd, &ev) == 0) {
        printf("ctx=%d type=%-11s ts=%" PRIu64 "ms frametime=%" PRIu64 "ms\n", ev.ctx_id,
               event_type_name(ev.type), (uint64_t)ev.timestamp_ms, (uint64_t)ev.frametime_ms);
    }

    perror("read");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = fas_client_open();
    if (fd < 0) {
        perror("open " FAS_DEV_PATH);
        fprintf(stderr, "is the module loaded and are you root?\n");
        return 1;
    }

    const char *cmd = argv[1];
    int ret;

    if (strcmp(cmd, "set-offset") == 0)
        ret = cmd_set_offset(fd, argc - 2, argv + 2);
    else if (strcmp(cmd, "get-offset") == 0)
        ret = cmd_get_offset(fd);
    else if (strcmp(cmd, "attach") == 0)
        ret = cmd_attach(fd, argc - 2, argv + 2);
    else if (strcmp(cmd, "detach") == 0)
        ret = cmd_detach(fd, argc - 2, argv + 2);
    else if (strcmp(cmd, "hint") == 0)
        ret = cmd_hint(fd, argc - 2, argv + 2);
    else if (strcmp(cmd, "listen") == 0)
        ret = cmd_listen(fd);
    else {
        print_usage(argv[0]);
        ret = 1;
    }

    close(fd);
    return ret;
}
