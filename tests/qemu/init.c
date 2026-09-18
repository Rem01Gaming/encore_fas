// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int load_module(const char *path) {
    int fd = open(path, O_RDONLY);
    int ret;

    if (fd < 0) { perror("open module"); return -1; }
    ret = (int)syscall(SYS_finit_module, fd, "", 0);
    if (ret) perror("finit_module");
    close(fd);
    return ret;
}

static int unload_module(const char *name) {
    int ret = (int)syscall(SYS_delete_module, name, O_NONBLOCK);
    if (ret) perror("delete_module");
    return ret;
}

static int run_capture(char *const argv[], char *out, int max) {
    int pipefd[2];
    pid_t p;
    int st = 0, n;

    if (pipe(pipefd))
        return -1;
    p = fork();
    if (p == 0) { dup2(pipefd[1], 1); close(pipefd[0]); execv(argv[0], argv); _exit(127); }
    close(pipefd[1]);
    n = read(pipefd[0], out, max - 1);
    out[n > 0 ? n : 0] = 0;
    close(pipefd[0]);
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int run(char *const argv[]) {
    pid_t p = fork();
    int st = 0;

    if (p == 0) { execv(argv[0], argv); perror("exec"); _exit(127); }
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void dump_klog(const char *tag) {
    static char buf[1 << 18];
    int n = klogctl(3, buf, sizeof(buf) - 1);
    char *line;

    if (n <= 0) return;
    buf[n] = 0;
    printf("---- kernel log (%s): lines with problems ----\n", tag);
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        if (strstr(line, "WARNING") || strstr(line, "BUG") || strstr(line, "lockdep") || strstr(line, "possible") || strstr(line, "inconsistent") || strstr(line, "Oops") ||
            strstr(line, "encore_fas: ") || strstr(line, "debugobjects") || strstr(line, "sleeping function") || strstr(line, "rcu_") || strstr(line, "RCU: ") || strstr(line, "Call trace") || strstr(line, "Call Trace"))
            printf("%s\n", line);
    printf("---- end ----\n");
}

int main(void) {
    char *selftest[] = {"/fas_ctl", "selftest", NULL};
    char *version[] = {"/fas_ctl", "version", NULL};
    char *list[] = {"/fas_ctl", "list", NULL};
    int rc, i;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    klogctl(8, NULL, 7);

    printf("== load module\n");
    if (load_module("/encore_fas.ko")) goto out;
    run(version);

    printf("== selftest\n");
    rc = run(selftest);
    printf("== selftest exit code %d\n", rc);

    printf("== list after selftest\n");
    run(list);

    for (i = 0; i < 3; i++) {
        printf("== unload/reload cycle %d\n", i);
        if (unload_module("encore_fas")) break;
        if (load_module("/encore_fas.ko")) break;
    }
    printf("== selftest -q after reload\n");
    char *quick[] = {"/fas_ctl", "selftest", "-q", NULL};
    rc = run(quick);
    printf("== quick selftest exit code %d\n", rc);

    printf("== attach and detach while frames run (240 fps)\n");
    unload_module("encore_fas");
    if (load_module("/encore_fas.ko") == 0) {
        pid_t g = fork();
        int i;

        if (g == 0) { char *fr[] = {"/fas_ctl", "frames", "240", "6", NULL}; execv(fr[0], fr); _exit(127); }
        usleep(200000);
        char pidbuf[16];
        snprintf(pidbuf, sizeof(pidbuf), "%d", (int)g);
        for (i = 0; i < 25; i++) {
            char out[128], idbuf[16];
            int ctx = -1;
            char *attach[] = {"/fas_ctl", "attach", pidbuf, "-l", "/fas_ctl", "-s", "fas_probe_frame", "60", "120", NULL};
            run_capture(attach, out, sizeof(out));
            sscanf(out, "ctx_id: %d", &ctx);
            usleep(60000 + (i % 5) * 20000);
            snprintf(idbuf, sizeof(idbuf), "%d", ctx);
            char *detach[] = {"/fas_ctl", "detach", idbuf, NULL};
            if (i % 2) run(detach);
        }
        run(list);
        printf("== unload module while the game still draws\n");
        unload_module("encore_fas");
        waitpid(g, NULL, 0);
        printf("== game finished, module was unloaded under load\n");
    }
    printf("== unload with a live listener on an idle process\n");
    if (load_module("/encore_fas.ko") == 0) {
        pid_t p = fork();
        if (p == 0) { pause(); _exit(0); }
        usleep(100000);
        char pidbuf[16];
        snprintf(pidbuf, sizeof(pidbuf), "%d", (int)p);
        char *attach[] = {"/fas_ctl", "attach", pidbuf, "-l", "/fas_ctl", "-s", "fas_probe_frame", "60", NULL};
        run(attach);
        run(list);
        unload_module("encore_fas");
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
        printf("== module unloaded with a live listener\n");
    }
    dump_klog("final");
out:
    printf("== done\n");
    sync();
    reboot(RB_POWER_OFF);
    for (;;) sleep(1);
}
