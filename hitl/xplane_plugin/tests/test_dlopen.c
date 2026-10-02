/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * Loads the built .xpl the way X-Plane does on Linux: the XPLM symbols are
 * already global in the process (here from the stub, loaded RTLD_GLOBAL), the
 * plugin is dlopen'ed with RTLD_NOW so any unresolved symbol fails the load,
 * and the five entry points are looked up by name and called.
 *
 * usage: camsim_truth_dlopen <libxplm_stub.so> <CamSimTruth.xpl> <work_dir>
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef int (*start_f)(char *, char *, char *);
typedef int (*enable_f)(void);
typedef void (*void_f)(void);
typedef void (*msg_f)(int, int, void *);

int main(int argc, char **argv)
{
    void *stub, *xpl;
    start_f start;
    enable_f enable;
    void_f disable, stop;
    msg_f receive;
    void (*set_path)(const char *);
    char name[256], sig[256], desc[256], path[1200];

    if (argc < 4)
        return 2;
    stub = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!stub) { fprintf(stderr, "FAIL stub: %s\n", dlerror()); return 1; }
    xpl = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!xpl) { fprintf(stderr, "FAIL plugin load: %s\n", dlerror()); return 1; }

    start = (start_f)dlsym(xpl, "XPluginStart");
    enable = (enable_f)dlsym(xpl, "XPluginEnable");
    disable = (void_f)dlsym(xpl, "XPluginDisable");
    stop = (void_f)dlsym(xpl, "XPluginStop");
    receive = (msg_f)dlsym(xpl, "XPluginReceiveMessage");
    if (!start || !enable || !disable || !stop || !receive) {
        fprintf(stderr, "FAIL missing entry point\n");
        return 1;
    }
    if (dlsym(xpl, "cst_test_set_clock") || dlsym(xpl, "cst_mono_ns")) {
        fprintf(stderr, "FAIL internal symbols exported\n");
        return 1;
    }

    /* No ini in this folder: defaults (127.0.0.1:49300); enable/disable only, no frames. */
    mkdir(argv[3], 0755);
    snprintf(path, sizeof path, "%s/CamSimTruth.xpl", argv[3]);
    set_path = (void (*)(const char *))dlsym(stub, "stub_set_plugin_path");
    if (set_path)
        set_path(path);

    if (start(name, sig, desc) != 1 || strcmp(sig, "camsim.hitl.truth") != 0) {
        fprintf(stderr, "FAIL XPluginStart\n");
        return 1;
    }
    if (enable() != 1) {
        fprintf(stderr, "FAIL XPluginEnable\n");
        return 1;
    }
    receive(0, 0, NULL);
    disable();
    stop();
    dlclose(xpl);
    printf("OK: %s loaded with RTLD_NOW, 5 entry points resolved, internals hidden\n", argv[2]);
    return 0;
}
