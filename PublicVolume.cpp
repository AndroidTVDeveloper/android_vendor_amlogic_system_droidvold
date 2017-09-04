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

static const char* kFusePath = "/system/bin/sdcard";

static const char* kChownPath = "/system/bin/chown";

PublicVolume::PublicVolume(dev_t device) :
        VolumeBase(Type::kPublic), mDevice(device), mFusePid(0), mJustPhysicalDev(false) {
    setId(StringPrintf("public:%u,%u", major(device), minor(device)));
    mDevPath = StringPrintf("/dev/block/droidvold/%s", getId().c_str());
    mSrMounted = false;
}

PublicVolume::PublicVolume(const std::string& physicalDevName) :
        VolumeBase(Type::kPublic), mFusePid(0), mJustPhysicalDev(true) {
    setId(physicalDevName);
    mDevPath = StringPrintf("/dev/block/%s", getId().c_str());
}

PublicVolume::~PublicVolume() {
}

status_t PublicVolume::readMetadata() {
    status_t res = ReadMetadataUntrusted(mDevPath, mFsType, mFsUuid, mFsLabel);
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
    return res;
}

status_t PublicVolume::doCreate() {
    if (mJustPhysicalDev) return 0;
    return CreateDeviceNode(mDevPath, mDevice);
}

status_t PublicVolume::doDestroy() {
    if (mJustPhysicalDev) return 0;
    return DestroyDeviceNode(mDevPath);
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

    if (!mJustPhysicalDev && mFsType == "vfat") {
        LOG(DEBUG) << getId() << " vfat will handle by vold";
        return 0;
    }

    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }
    mRawPath = StringPrintf("/mnt/media_rw/%s", stableName.c_str());

    VolumeManager *vm = VolumeManager::Instance();
    if (vm->isMountpointMounted(mRawPath.c_str())) {
        LOG(ERROR) << " path:" << mRawPath << " is already mounted";
        return -EIO;
    }

    // Check filesystems
    status_t checkStatus = -1;
    if (mFsType == "vfat") {
        checkStatus = vfat::Check(mDevPath);
    } else if (mFsType == "ntfs") {
        checkStatus = ntfs::Check(mDevPath.c_str());
    } else if (mFsType == "exfat") {
        checkStatus = exfat::Check(mDevPath.c_str());
    } else if (!strncmp(mFsType.c_str(), "ext", 3)) {
        // ext2/3/4 check later
        checkStatus = 0;
    } else if (mFsType == "hfs") {
        checkStatus = hfsplus::Check(mDevPath.c_str());
    } else if (mFsType == "iso9660" || mFsType == "udf") {
        // iso needn't check
        checkStatus = iso9660::Check(mDevPath.c_str());
    }


    if (checkStatus) {
        LOG(ERROR) << getId() << " failed to check filesystem " << mFsType;
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
    std::string logicPartDevPath = mDevPath;
    if (!mJustPhysicalDev &&
        (mFsType == "ntfs" || mFsType == "exfat")) {
        if (GetLogicalPartitionDevice(mDevice, getSysPath(), logicPartDevPath) != OK) {
            LOG(ERROR) << "failed to get logical partition device for fstype " << mFsType;
            return -errno;
        }
    }

    if (mFsType == "vfat") {
        mountStatus = vfat::Mount(mDevPath, mRawPath, false, false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (mFsType == "ntfs") {
        mountStatus = ntfs::Mount(logicPartDevPath.c_str(), mRawPath.c_str(), false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (mFsType == "exfat") {
        mountStatus = exfat::Mount(logicPartDevPath.c_str(), mRawPath.c_str(), false, false,
                            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true);
    } else if (!strncmp(mFsType.c_str(), "ext", 3)) {
        int res = ext4::Check(logicPartDevPath, mRawPath);
        if (res == 0 || res == 1) {
            LOG(DEBUG) << getId() << " passed filesystem check";
        } else {
            PLOG(ERROR) << getId() << " failed filesystem check";
        //    return -EIO;
        }

        mountStatus = ext4::Mount(logicPartDevPath, mRawPath, false, false, true, mFsType);
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
