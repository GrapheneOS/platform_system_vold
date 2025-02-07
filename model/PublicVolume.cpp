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

#include "PublicVolume.h"

#include "AppFuseUtil.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "fs/Exfat.h"
#include "fs/Vfat.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>
#include <utils/Timers.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>

using android::base::GetBoolProperty;
using android::base::StringPrintf;

namespace android {
namespace vold {

static const char* kSdcardFsPath = "/system/bin/sdcard";

static const char* kAsecPath = "/mnt/secure/asec";

PublicVolume::PublicVolume(dev_t device) : VolumeBase(Type::kPublic), mDevice(device) {
    setId(StringPrintf("public:%u,%u", major(device), minor(device)));
    mDevPath = StringPrintf("/dev/block/vold/%s", getId().c_str());
    mFuseMounted = false;
    mUseSdcardFs = IsSdcardfsUsed();
}

PublicVolume::~PublicVolume() {}

status_t PublicVolume::readMetadata() {
    status_t res = ReadMetadataUntrusted(mDevPath, &mFsType, &mFsUuid, &mFsLabel);

    auto listener = getListener();
    if (listener) listener->onVolumeMetadataChanged(getId(), mFsType, mFsUuid, mFsLabel);

    return res;
}

status_t PublicVolume::initAsecStage() {
    std::string legacyPath(mRawPath + "/android_secure");
    std::string securePath(mRawPath + "/.android_secure");

    // Recover legacy secure path
    if (!access(legacyPath.c_str(), R_OK | X_OK) && access(securePath.c_str(), R_OK | X_OK)) {
        if (rename(legacyPath.c_str(), securePath.c_str())) {
            PLOG(WARNING) << getId() << " failed to rename legacy ASEC dir";
        }
    }

    if (TEMP_FAILURE_RETRY(mkdir(securePath.c_str(), 0700))) {
        if (errno != EEXIST) {
            PLOG(WARNING) << getId() << " creating ASEC stage failed";
            return -errno;
        }
    }

    BindMount(securePath, kAsecPath);

    return OK;
}

status_t PublicVolume::doCreate() {
    return CreateDeviceNode(mDevPath, mDevice);
}

status_t PublicVolume::doDestroy() {
    return DestroyDeviceNode(mDevPath);
}

status_t PublicVolume::doMount() {
    bool isVisible = isVisibleForWrite();
    readMetadata();

    if (mFsType == "vfat" && vfat::IsSupported()) {
        if (vfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else if (mFsType == "exfat" && exfat::IsSupported()) {
        if (exfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }

    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }

    mRawPath = StringPrintf("/mnt/media_rw/%s", stableName.c_str());

    mSdcardFsDefault = StringPrintf("/mnt/runtime/default/%s", stableName.c_str());
    mSdcardFsRead = StringPrintf("/mnt/runtime/read/%s", stableName.c_str());
    mSdcardFsWrite = StringPrintf("/mnt/runtime/write/%s", stableName.c_str());
    mSdcardFsFull = StringPrintf("/mnt/runtime/full/%s", stableName.c_str());

    setInternalPath(mRawPath);
    if (isVisible) {
        setPath(StringPrintf("/storage/%s", stableName.c_str()));
    } else {
        setPath(mRawPath);
    }

    if (fs_prepare_dir(mRawPath.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

    if (mFsType == "vfat") {
        if (vfat::Mount(mDevPath, mRawPath, false, false, false, AID_ROOT,
                        (isVisible ? AID_MEDIA_RW : AID_EXTERNAL_STORAGE), 0007, true)) {
            PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
            return -EIO;
        }
    } else if (mFsType == "exfat") {
        if (exfat::Mount(mDevPath, mRawPath, AID_ROOT,
                         (isVisible ? AID_MEDIA_RW : AID_EXTERNAL_STORAGE), 0007)) {
            PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
            return -EIO;
        }
    }

    if (getMountFlags() & MountFlags::kPrimary) {
        initAsecStage();
    }

    if (!isVisible) {
        // Not visible to apps, so no need to spin up sdcardfs or FUSE
        return OK;
    }

    if (mUseSdcardFs) {
        if (fs_prepare_dir(mSdcardFsDefault.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsRead.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsWrite.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsFull.c_str(), 0700, AID_ROOT, AID_ROOT)) {
            PLOG(ERROR) << getId() << " failed to create sdcardfs mount points";
            return -errno;
        }

        dev_t before = GetDevice(mSdcardFsFull);

        int sdcardFsPid;
        if (!(sdcardFsPid = fork())) {
            if (getMountFlags() & MountFlags::kPrimary) {
                // clang-format off
                if (execl(kSdcardFsPath, kSdcardFsPath,
                        "-u", "1023", // AID_MEDIA_RW
                        "-g", "1023", // AID_MEDIA_RW
                        "-U", std::to_string(getMountUserId()).c_str(),
                        "-w",
                        mRawPath.c_str(),
                        stableName.c_str(),
                        NULL)) {
                    // clang-format on
                    PLOG(ERROR) << "Failed to exec";
                }
            } else {
                // clang-format off
                if (execl(kSdcardFsPath, kSdcardFsPath,
                        "-u", "1023", // AID_MEDIA_RW
                        "-g", "1023", // AID_MEDIA_RW
                        "-U", std::to_string(getMountUserId()).c_str(),
                        mRawPath.c_str(),
                        stableName.c_str(),
                        NULL)) {
                    // clang-format on
                    PLOG(ERROR) << "Failed to exec";
                }
            }

            LOG(ERROR) << "sdcardfs exiting";
            _exit(1);
        }

        if (sdcardFsPid == -1) {
            PLOG(ERROR) << getId() << " failed to fork";
            return -errno;
        }

        nsecs_t start = systemTime(SYSTEM_TIME_BOOTTIME);
        while (before == GetDevice(mSdcardFsFull)) {
            LOG(DEBUG) << "Waiting for sdcardfs to spin up...";
            usleep(50000);  // 50ms

            nsecs_t now = systemTime(SYSTEM_TIME_BOOTTIME);
            if (nanoseconds_to_milliseconds(now - start) > 5000) {
                LOG(WARNING) << "Timed out while waiting for sdcardfs to spin up";
                return -ETIMEDOUT;
            }
        }
        /* sdcardfs will have exited already. The filesystem will still be running */
        TEMP_FAILURE_RETRY(waitpid(sdcardFsPid, nullptr, 0));
    }

    // We need to mount FUSE *after* sdcardfs, since the FUSE daemon may depend
    // on sdcardfs being up.
    LOG(INFO) << "Mounting public fuse volume";
    android::base::unique_fd fd;
    int user_id = getMountUserId();
    int result = MountUserFuse(user_id, getInternalPath(), stableName, &fd);

    if (result != 0) {
        LOG(ERROR) << "Failed to mount public fuse volume";
        doUnmount();
        return -result;
    }

    mFuseMounted = true;
    auto callback = getMountCallback();
    if (callback) {
        bool is_ready = false;
        callback->onVolumeChecking(std::move(fd), getPath(), getInternalPath(), &is_ready);
        if (!is_ready) {
            LOG(ERROR) << "Failed to complete public volume mount";
            doUnmount();
            return -EIO;
        }
    }

    ConfigureReadAheadForFuse(GetFuseMountPathForUser(user_id, stableName), 256u);

    // See comment in model/EmulatedVolume.cpp
    ConfigureMaxDirtyRatioForFuse(GetFuseMountPathForUser(user_id, stableName), 40u);

    auto vol_manager = VolumeManager::Instance();
    // Create bind mounts for all running users
    for (userid_t started_user : vol_manager->getStartedUsers()) {
        userid_t mountUserId = getMountUserId();
        if (started_user == mountUserId) {
            // No need to bind mount for the user that owns the mount
            continue;
        }
        if (mountUserId != VolumeManager::Instance()->getSharedStorageUser(started_user)) {
            // No need to bind if the user does not share storage with the mount owner
            continue;
        }
        // Create mount directory for the user as there is a chance that no other Volume is mounted
        // for the user (ex: if the user is just started), so /mnt/user/user_id  does not exist yet.
        auto mountDirStatus = PrepareMountDirForUser(started_user);
        if (mountDirStatus != OK) {
            LOG(ERROR) << "Failed to create Mount Directory for user " << started_user;
        }
        auto bindMountStatus = bindMountForUser(started_user);
        if (bindMountStatus != OK) {
            LOG(ERROR) << "Bind Mounting Public Volume: " << stableName
                       << " for user: " << started_user << "Failed. Error: " << bindMountStatus;
        }
    }
    return OK;
}

status_t PublicVolume::bindMountForUser(userid_t user_id) {
    userid_t mountUserId = getMountUserId();
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }

    LOG(INFO) << "Bind Mounting Public Volume for user: " << user_id
              << ".Mount owner: " << mountUserId;
    auto sourcePath = GetFuseMountPathForUser(mountUserId, stableName);
    auto destPath = GetFuseMountPathForUser(user_id, stableName);
    PrepareDir(destPath, 0770, AID_ROOT, AID_MEDIA_RW);
    auto mountRes = BindMount(sourcePath, destPath);
    LOG(INFO) << "Mount status: " << mountRes;

    return mountRes;
}

status_t PublicVolume::doUnmount() {
    // Unmount the storage before we kill the FUSE process. If we kill
    // the FUSE process first, most file system operations will return
    // ENOTCONN until the unmount completes. This is an exotic and unusual
    // error code and might cause broken behaviour in applications.
    KillProcessesUsingPath(getPath());

    if (mFuseMounted) {
        // Use UUID as stable name, if available
        std::string stableName = getId();
        if (!mFsUuid.empty()) {
            stableName = mFsUuid;
        }

        // Unmount bind mounts for running users
        auto vol_manager = VolumeManager::Instance();
        int user_id = getMountUserId();
        for (int started_user : vol_manager->getStartedUsers()) {
            if (started_user == user_id) {
                // No need to remove bind mount for the user that owns the mount
                continue;
            }
            LOG(INFO) << "Removing Public Volume Bind Mount for: " << started_user;
            auto mountPath = GetFuseMountPathForUser(started_user, stableName);
            ForceUnmount(mountPath);
            rmdir(mountPath.c_str());
        }

        if (UnmountUserFuse(getMountUserId(), getInternalPath(), stableName) != OK) {
            PLOG(INFO) << "UnmountUserFuse failed on public fuse volume";
            return -errno;
        }

        mFuseMounted = false;
    }

    ForceUnmount(kAsecPath);

    if (mUseSdcardFs) {
        ForceUnmount(mSdcardFsDefault);
        ForceUnmount(mSdcardFsRead);
        ForceUnmount(mSdcardFsWrite);
        ForceUnmount(mSdcardFsFull);

        rmdir(mSdcardFsDefault.c_str());
        rmdir(mSdcardFsRead.c_str());
        rmdir(mSdcardFsWrite.c_str());
        rmdir(mSdcardFsFull.c_str());

        mSdcardFsDefault.clear();
        mSdcardFsRead.clear();
        mSdcardFsWrite.clear();
        mSdcardFsFull.clear();
    }

    if (ForceUnmount(mRawPath) != 0){
        umount2(mRawPath.c_str(),MNT_DETACH);
        PLOG(INFO) << "use umount lazy if force unmount fail";
    }
    if(rmdir(mRawPath.c_str()) != 0) {
        PLOG(INFO) << "rmdir mRawPath=" << mRawPath << " fail";
        KillProcessesUsingPath(getPath());
    }
    mRawPath.clear();

    return OK;
}

status_t PublicVolume::doFormat(const std::string& fsType) {
    bool isVfatSup = vfat::IsSupported();
    bool isExfatSup = exfat::IsSupported();
    status_t res = OK;

    enum { NONE, VFAT, EXFAT } fsPick = NONE;

    // Resolve auto requests
    if (fsType == "auto" && isVfatSup && isExfatSup) {
        uint64_t size = 0;

        res = GetBlockDevSize(mDevPath, &size);
        if (res != OK) {
            LOG(ERROR) << "Couldn't get device size " << mDevPath;
            return res;
        }

        // If both vfat & exfat are supported use exfat for SDXC (>~32GiB) cards
        if (size > 32896LL * 1024 * 1024) {
            fsPick = EXFAT;
        } else {
            fsPick = VFAT;
        }
    } else if (fsType == "auto" && isExfatSup) {
        fsPick = EXFAT;
    } else if (fsType == "auto" && isVfatSup) {
        fsPick = VFAT;
    }

    // Resolve explicit requests
    if (fsType == "vfat" && isVfatSup) {
        fsPick = VFAT;
    } else if (fsType == "exfat" && isExfatSup) {
        fsPick = EXFAT;
    }

    if (WipeBlockDevice(mDevPath) != OK) {
        LOG(WARNING) << getId() << " failed to wipe";
    }

    if (fsPick == VFAT) {
        res = vfat::Format(mDevPath, 0);
    } else if (fsPick == EXFAT) {
        res = exfat::Format(mDevPath);
    } else {
        LOG(ERROR) << "Unsupported filesystem " << fsType;
        return -EINVAL;
    }

    if (res != OK) {
        LOG(ERROR) << getId() << " failed to format";
        res = -errno;
    }

    return res;
}

}  // namespace vold
}  // namespace android
