/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Disk.h"
#include "VolumeManager.h"
#include "CommandListener.h"
#include "NetlinkManager.h"
#include "sehandle.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/klog.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>
#include <fs_mgr.h>

#define LOG_TAG "droidVold"

#include "cutils/klog.h"
#include "cutils/log.h"
#include "cutils/properties.h"

static int process_config(VolumeManager *vm, bool* has_adoptable);

static void coldboot(const char *path);
static void parse_args(int argc, char** argv);
static void set_media_poll_time(void);

struct fstab *fstab;

struct selabel_handle *sehandle;

using android::base::StringPrintf;

int main(int argc, char** argv) {
    setenv("ANDROID_LOG_TAGS", "*:v", 1);
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));

    LOG(INFO) << "doildVold 1.0 firing up";

    VolumeManager *vm;
    NetlinkManager *nm;
    CommandListener *cl;

    parse_args(argc, argv);

    sehandle = selinux_android_file_context_handle();
    if (sehandle) {
        selinux_android_set_sehandle(sehandle);
    }

    mkdir("/dev/block/droidvold", 0755);

    /* Create our singleton managers */
    if (!(vm = VolumeManager::Instance())) {
        LOG(ERROR) << "Unable to create VolumeManager";
        exit(1);
    }

    if (!(nm = NetlinkManager::Instance())) {
        LOG(ERROR) << "Unable to create NetlinkManager";
        exit(1);
    }

    if (property_get_bool("droidvold.debug", false)) {
        vm->setDebug(true);
    }

    cl = new CommandListener();
    vm->setBroadcaster((SocketListener *) cl);
    nm->setBroadcaster((SocketListener *) cl);

    if (vm->start()) {
        PLOG(ERROR) << "Unable to start VolumeManager";
        exit(1);
    }

    bool has_adoptable;

    if (process_config(vm, &has_adoptable)) {
        PLOG(ERROR) << "Error reading configuration... continuing anyways";
    }

    if (nm->start()) {
        PLOG(ERROR) << "Unable to start NetlinkManager";
        exit(1);
    }

    set_media_poll_time();
    coldboot("/sys/block");

    /*
     * Now that we're up, we can respond to commands
     */
    if (cl->startListener()) {
        PLOG(ERROR) << "Unable to start CommandListener";
        exit(1);
    }

    // Eventually we'll become the monitoring thread
    while (1) {
        sleep(1000);
    }

    LOG(ERROR) << "droidVold exiting";
    exit(0);
}

static void set_media_poll_time(void) {
    int fd;

    fd = open ("/sys/module/block/parameters/events_dfl_poll_msecs", O_WRONLY);
    if (fd >= 0) {
        write(fd, "2000", 4);
        close (fd);
    } else {
        LOG(ERROR) << "kernel not support media poll uevent!";
    }
}

static void parse_args(int argc, char** argv) {
    static struct option opts[] = {
        {"blkid_context", required_argument, 0, 'b' },
        {"blkid_untrusted_context", required_argument, 0, 'B' },
        {"fsck_context", required_argument, 0, 'f' },
        {"fsck_untrusted_context", required_argument, 0, 'F' },
    };

    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'b': android::droidvold::sBlkidContext = optarg; break;
        case 'B': android::droidvold::sBlkidUntrustedContext = optarg; break;
        case 'f': android::droidvold::sFsckContext = optarg; break;
        case 'F': android::droidvold::sFsckUntrustedContext = optarg; break;
        }
    }

    CHECK(android::droidvold::sBlkidContext != nullptr);
    CHECK(android::droidvold::sBlkidUntrustedContext != nullptr);
    CHECK(android::droidvold::sFsckContext != nullptr);
    CHECK(android::droidvold::sFsckUntrustedContext != nullptr);
}

static void do_coldboot(DIR *d, int lvl) {
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while ((de = readdir(d))) {
        DIR *d2;

        if (de->d_name[0] == '.')
            continue;

        if (de->d_type != DT_DIR && lvl > 0)
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (fd < 0)
            continue;

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char *path) {
    DIR *d = opendir(path);

    if (d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}

static int process_config(VolumeManager *vm, bool* has_adoptable) {
    std::string path(android::droidvold::DefaultFstabPath());
    fstab = fs_mgr_read_fstab(path.c_str());
    if (!fstab) {
        PLOG(ERROR) << "Failed to open default fstab " << path;
        return -1;
    }

    /* Loop through entries looking for ones that vold manages */
    *has_adoptable = false;
    for (int i = 0; i < fstab->num_entries; i++) {
        if (fs_mgr_is_voldmanaged(&fstab->recs[i])) {
            if (fs_mgr_is_nonremovable(&fstab->recs[i])) {
                LOG(WARNING) << "nonremovable no longer supported; ignoring volume";
                continue;
            }

            std::string sysPattern(fstab->recs[i].blk_device);
            std::string nickname(fstab->recs[i].label);
            int flags = 0;

            if (fs_mgr_is_encryptable(&fstab->recs[i])) {
                flags |= android::droidvold::Disk::Flags::kAdoptable;
                *has_adoptable = true;
            }
            if (fs_mgr_is_noemulatedsd(&fstab->recs[i])
                    || property_get_bool("vold.debug.default_primary", false)) {
                flags |= android::droidvold::Disk::Flags::kDefaultPrimary;
            }

            vm->addDiskSource(std::shared_ptr<VolumeManager::DiskSource>(
                    new VolumeManager::DiskSource(sysPattern, nickname, flags)));
        }
    }
    return 0;
}
