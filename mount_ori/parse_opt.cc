#include <cstdio>
#include <cstddef>
#include <cstring>

#include "logging.h"
#include "openedfilemgr.h"
#include "ori_fuse.h"

#define MOUNT_ORI_OPT(t, p, v) {t, offsetof(mount_ori_config, p), v}
static struct fuse_opt mount_ori_opts[] = {
    MOUNT_ORI_OPT("repo=%s", repo_path, 0),
    MOUNT_ORI_OPT("clone=%s", clone_path, 0),
    { NULL, 0, 0 }, // FUSE_OPT_END: Macro incompatible with C++
};

static int mount_ori_opt_proc(void *data, const char *arg, int key, struct
        fuse_args *outargs)
{
    return 1;
}

#undef FUSE_SINGLE_THREADED
#define FUSE_SINGLE_THREADED 0

void mount_ori_parse_opt(struct fuse_args *args, mount_ori_config *conf)
{
    memset(conf, sizeof(mount_ori_config), 0);
    fuse_opt_parse(args, conf, mount_ori_opts, mount_ori_opt_proc);

#if defined(FUSE_SINGLE_THREADED) && FUSE_SINGLE_THREADED == 1
    printf("FUSE forcing single threaded\n");
    fuse_opt_add_arg(args, "-s"); // Force single-threaded
#endif
}
