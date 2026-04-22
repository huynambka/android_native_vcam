#include <stdio.h>
#include <stdlib.h>

extern int do_inject(const char *target_name, const char *loader_path);

int main(int argc, char **argv) {
    const char *target = "cameraserver";
    const char *loader = "/data/camera/libproxy.so";

    if (argc > 1) target = argv[1];
    if (argc > 2) loader = argv[2];

    return do_inject(target, loader);
}
