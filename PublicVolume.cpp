/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "fs/Vfat.h"
#include "fs/Ntfs.h"
#include "fs/Exfat.h"
#include "fs/Hfsplus.h"
#include "fs/Iso9660.h"
#include "fs/Ext4.h"
#include "PublicVolume.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "ResponseCode.h"

#include <android-base/stringprintf.h>
#include <android-base/logging.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

using android::base::StringPrintf;

namespace android {
namespace droidvold {

static const char* kChownPath = "/system/bin/chown";

PublicVolume::PublicVolume(const std::string& physicalDevName, const bool isPhysical) :
        VolumeBase(Type::kPublic), mFusePid(0), mJustPhysicalDev(isPhysical) {
    setId(physicalDevName);
    mDevPath = StringPrintf("/dev/block/%s", getId().c_str());
}

PublicVolume::~PublicVolume() {
}

status_t PublicVolume::readMetadata() {
    status_t res = ReadPartMetadata(mDevPath, mFsType, mFsUuid, mFsLabel);

    if (VolumeManager::Instance()->getDebug())
        LOG(DEBUG) << "blkid get devPath=" << mDevPath << " fsType= " << mFsType;

    notifyEvent(ResponseCode::VolumeFsTypeChanged, mFsType);

    // TODO: find the Uuid of srdisk
    // If mFsUuid of publicVolume is empty,
    // it will cause systemUi crash when it is mounted
    if (mFsUuid.empty()) {
        if (major(mDevice) == 11)
            mFsUuid = "sr0";
        else
            mFsUuid = "fakeUuid";
    }

    notifyEvent(ResponseCode::VolumeFsUuidChanged, mFsUuid);
    notifyEvent(ResponseCode::VolumeFsLabelChanged, mFsLabel);

    return OK;
}

status_t PublicVolume::doCreate() {
    return 0;
}

status_t PublicVolume::doDestroy() {
    return 0;
}

status_t PublicVolume::doMount() {
    // TODO: expand to support mounting other filesystems
    readMetadata();

    if (mFsType != "vfat" &&
        mFsType != "ntfs" &&
        mFsType != "exfat" &&
        strncmp(mFsType.c_str(), "ext", 3) &&
        mFsType != "hfs" &&
        mFsType != "iso9660" &&
        mFsType != "udf") {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }

    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }
    mRawPath = StringPrintf("/mnt/media_rw/%s", stableName.c_str());

    VolumeManager *vm = VolumeManager::Instance();
    //if (!mJustPhysicalDev && mFsType == "vfat") {
    if (mFsType == "vfat") {
        sleep(2);
        if (vm->isMountpointMounted(mRawPath.c_str())) {
            LOG(DEBUG) << getId() << " vfat will handle by vold";
            return 0;
        }
    }

    if (vm->isMountpointMounted(mRawPath.c_str())) {
        LOG(ERROR) << " path:" << mRawPath << " is already mounted";
        return -EIO;
    }

    setInternalPath(mRawPath);
    setPath(mRawPath);

    if (prepareDir(mRawPath, 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

    // Mount device
    status_t mountStatus = -1;

    if (mFsType == "vfat") {
        mountStatus = vfat::Mount(mDevPath, mRawPath, false, false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (mFsType == "ntfs") {
        mountStatus = ntfs::Mount(mDevPath.c_str(), mRawPath.c_str(), false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (mFsType == "exfat") {
        mountStatus = exfat::Mount(mDevPath.c_str(), mRawPath.c_str(), false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (!strncmp(mFsType.c_str(), "ext", 3)) {
        mountStatus = ext4::Mount(mDevPath, mRawPath, false, false, true, mFsType);
    } else if (mFsType == "hfs") {
        mountStatus = hfsplus::Mount(mDevPath.c_str(), mRawPath.c_str(), false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (mFsType == "iso9660" || mFsType == "udf") {
        if ((mountStatus = iso9660::Mount(mDevPath.c_str(), mRawPath.c_str(), false, false,
                        AID_MEDIA_RW, AID_MEDIA_RW, 0007, true)) == 0)
            mSrMounted = true;
    }

    if (mountStatus) {
        PLOG(ERROR) << " failed to mount " << mDevPath << " as " << mFsType;
        return -EIO;
    } else {
        LOG(INFO) << "successfully mount " << mDevPath << " as " << mFsType;
    }

    if (!strncmp(mFsType.c_str(), "ext", 3)) {
        std::vector<std::string> cmd;
        cmd.push_back(kChownPath);
        cmd.push_back("-R");
        cmd.push_back("media_rw:media_rw");
        cmd.push_back(mRawPath);

        std::vector<std::string> output;
        status_t res = ForkExecvp(cmd, output);
        if (res != OK) {
            LOG(WARNING) << "chown failed " << mRawPath;
            return res;
        }

        RestoreconRecursive(mRawPath);

        LOG(VERBOSE) << "Finished restorecon of " << mRawPath;
    }

    return OK;
}

status_t PublicVolume::doUnmount() {
    // Unmount the storage before we kill the FUSE process. If we kill
    // the FUSE process first, most file system operations will return
    // ENOTCONN until the unmount completes. This is an exotic and unusual
    // error code and might cause broken behaviour in applications.
    KillProcessesUsingPath(getPath());

#ifdef HAS_VIRTUAL_CDROM
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }

    VolumeManager *vm = VolumeManager::Instance();
    vm->unmountLoopIfNeed(stableName.c_str());
#endif

    ForceUnmount(mRawPath);

    if (mFusePid > 0) {
        kill(mFusePid, SIGTERM);
        TEMP_FAILURE_RETRY(waitpid(mFusePid, nullptr, 0));
        mFusePid = 0;
    }

    rmdir(mRawPath.c_str());

    mRawPath.clear();

    return OK;
}

status_t PublicVolume::doFormat(const std::string& fsType) {
    if (fsType == "vfat" || fsType == "auto") {
        if (WipeBlockDevice(mDevPath) != OK) {
            LOG(WARNING) << getId() << " failed to wipe";
        }
        if (vfat::Format(mDevPath, 0)) {
            LOG(ERROR) << getId() << " failed to format";
            return -errno;
        }
    } else {
        LOG(ERROR) << "Unsupported filesystem " << fsType;
        return -EINVAL;
    }

    return OK;
}

status_t PublicVolume::prepareDir(const std::string& path,
        mode_t mode, uid_t uid, gid_t gid) {
    if (fs_prepare_dir(path.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        if (errno == ENOTCONN) { // Transport endpoint is not connected
            LOG(ERROR) << getId() << " failed to create mount point";
            LOG(INFO) << "umount " << path << " and try again";
            // lazy umount
            if (!umount2(path.c_str(), MNT_DETACH) || errno == EINVAL || errno == ENOENT) {
                if (fs_prepare_dir(path.c_str(), 0700, AID_ROOT, AID_ROOT)) {
                    return -1;
                }
                return OK;
            }
            PLOG(ERROR) << " failed to umount " << path;
            return -1;
        }
        return -1;
    }

    return OK;
}

}  // namespace vold
}  // namespace android
