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

#define ATRACE_TAG ATRACE_TAG_PACKAGE_MANAGER

#include "FsCrypt.h"
#include "MetadataCrypt.h"
#include "NetlinkManager.h"
#include "VoldNativeService.h"
#include "VoldUtil.h"
#include "VolumeManager.h"
#include "model/Disk.h"
#include "sehandle.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/klog.h>
#include <hidl/HidlTransportSupport.h>
#include <utils/Trace.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct vold_configs {
    bool has_adoptable : 1;
    bool has_quota : 1;
    bool has_reserved : 1;
    bool has_compress : 1;
} VoldConfigs;

static int process_config(VolumeManager* vm, VoldConfigs* configs);
static void coldboot(const char* path);
static void parse_args(int argc, char** argv);
static void VoldLogger(android::base::LogId log_buffer_id, android::base::LogSeverity severity,
                       const char* tag, const char* file, unsigned int line, const char* message);

struct selabel_handle* sehandle;
android::base::LogdLogger logd_logger(android::base::SYSTEM);

using android::base::StringPrintf;
using android::fs_mgr::ReadDefaultFstab;

int main(int argc, char** argv) {
    atrace_set_tracing_enabled(false);
    setenv("ANDROID_LOG_TAGS", "*:d", 1);  // Do not submit with verbose logs enabled
    android::base::InitLogging(argv, &VoldLogger);

    LOG(INFO) << "Vold 3.0 (the awakening) firing up";

    ATRACE_BEGIN("main");

    LOG(DEBUG) << "Detected support for:"
               << (android::vold::IsFilesystemSupported("ext4") ? " ext4" : "")
               << (android::vold::IsFilesystemSupported("f2fs") ? " f2fs" : "")
               << (android::vold::IsFilesystemSupported("vfat") ? " vfat" : "");

    VolumeManager* vm;
    NetlinkManager* nm;

    parse_args(argc, argv);

    sehandle = selinux_android_file_context_handle();
    if (!sehandle) {
        LOG(ERROR) << "Failed to get SELinux file contexts handle";
        exit(1);
    }
    selinux_android_set_sehandle(sehandle);

    mkdir("/dev/block/vold", 0755);

    /* For when cryptfs checks and mounts an encrypted filesystem */
    klog_set_level(6);

    /* Create our singleton managers */
    if (!(vm = VolumeManager::Instance())) {
        LOG(ERROR) << "Unable to create VolumeManager";
        exit(1);
    }

    if (!(nm = NetlinkManager::Instance())) {
        LOG(ERROR) << "Unable to create NetlinkManager";
        exit(1);
    }

    if (android::base::GetBoolProperty("vold.debug", false)) {
        vm->setDebug(true);
    }

    if (vm->start()) {
        PLOG(ERROR) << "Unable to start VolumeManager";
        exit(1);
    }

    VoldConfigs configs = {};
    if (process_config(vm, &configs)) {
        PLOG(ERROR) << "Error reading configuration... continuing anyways";
    }

    android::hardware::configureRpcThreadpool(1, false /* callerWillJoin */);

    ATRACE_BEGIN("VoldNativeService::start");
    if (android::vold::VoldNativeService::start() != android::OK) {
        LOG(ERROR) << "Unable to start VoldNativeService";
        exit(1);
    }
    ATRACE_END();

    LOG(DEBUG) << "VoldNativeService::start() completed OK";

    ATRACE_BEGIN("NetlinkManager::start");
    if (nm->start()) {
        PLOG(ERROR) << "Unable to start NetlinkManager";
        exit(1);
    }
    ATRACE_END();

    // This call should go after listeners are started to avoid
    // a deadlock between vold and init (see b/34278978 for details)
    android::base::SetProperty("vold.has_adoptable", configs.has_adoptable ? "1" : "0");
    android::base::SetProperty("vold.has_quota", configs.has_quota ? "1" : "0");
    android::base::SetProperty("vold.has_reserved", configs.has_reserved ? "1" : "0");
    android::base::SetProperty("vold.has_compress", configs.has_compress ? "1" : "0");

    // Do coldboot here so it won't block booting,
    // also the cold boot is needed in case we have flash drive
    // connected before Vold launched
    coldboot("/sys/block");

    ATRACE_END();

    android::IPCThreadState::self()->joinThreadPool();
    LOG(INFO) << "vold shutting down";

    exit(0);
}

static void parse_args(int argc, char** argv) {
    static struct option opts[] = {
        {"blkid_context", required_argument, 0, 'b'},
        {"blkid_untrusted_context", required_argument, 0, 'B'},
        {"fsck_context", required_argument, 0, 'f'},
        {"fsck_untrusted_context", required_argument, 0, 'F'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
            // clang-format off
        case 'b': android::vold::sBlkidContext = optarg; break;
        case 'B': android::vold::sBlkidUntrustedContext = optarg; break;
        case 'f': android::vold::sFsckContext = optarg; break;
        case 'F': android::vold::sFsckUntrustedContext = optarg; break;
                // clang-format on
        }
    }

    CHECK(android::vold::sBlkidContext != nullptr);
    CHECK(android::vold::sBlkidUntrustedContext != nullptr);
    CHECK(android::vold::sFsckContext != nullptr);
    CHECK(android::vold::sFsckUntrustedContext != nullptr);
}

static void do_coldboot(DIR* d, int lvl) {
    struct dirent* de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while ((de = readdir(d))) {
        DIR* d2;

        if (de->d_name[0] == '.') continue;

        if (de->d_type != DT_DIR && lvl > 0) continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char* path) {
    ATRACE_NAME("coldboot");
    DIR* d = opendir(path);
    if (d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}

static int process_config(VolumeManager* vm, VoldConfigs* configs) {
    ATRACE_NAME("process_config");

    if (!ReadDefaultFstab(&fstab_default)) {
        PLOG(ERROR) << "Failed to open default fstab";
        return -1;
    }

    /* Loop through entries looking for ones that vold manages */
    configs->has_adoptable = false;
    configs->has_quota = false;
    configs->has_reserved = false;
    configs->has_compress = false;
    for (auto& entry : fstab_default) {
        if (entry.fs_mgr_flags.quota) {
            configs->has_quota = true;
        }
        if (entry.reserved_size > 0) {
            configs->has_reserved = true;
        }
        if (entry.fs_mgr_flags.fs_compress) {
            configs->has_compress = true;
        }

        /* Make sure logical partitions have an updated blk_device. */
        if (entry.fs_mgr_flags.logical && !fs_mgr_update_logical_partition(&entry) &&
            !entry.fs_mgr_flags.no_fail) {
            PLOG(FATAL) << "could not find logical partition " << entry.blk_device;
        }

        if (entry.mount_point == "/data" && !entry.metadata_key_dir.empty()) {
            // Pre-populate userdata dm-devices since the uevents are asynchronous (b/198405417).
            android::vold::defaultkey_precreate_dm_device();
        }

        if (entry.fs_mgr_flags.vold_managed) {
            if (entry.fs_mgr_flags.nonremovable) {
                LOG(WARNING) << "nonremovable no longer supported; ignoring volume";
                continue;
            }

            std::string sysPattern(entry.blk_device);
            std::string nickname(entry.label);
            int flags = 0;

            if (entry.is_encryptable()) {
            }
            if (entry.fs_mgr_flags.no_emulated_sd ||
                android::base::GetBoolProperty("vold.debug.default_primary", false)) {
                flags |= android::vold::Disk::Flags::kDefaultPrimary;
            }

            vm->addDiskSource(std::shared_ptr<VolumeManager::DiskSource>(
                new VolumeManager::DiskSource(sysPattern, nickname, flags)));
        }
    }
    return 0;
}

static void VoldLogger(android::base::LogId log_buffer_id, android::base::LogSeverity severity,
                       const char* tag, const char* file, unsigned int line, const char* message) {
    logd_logger(log_buffer_id, severity, tag, file, line, message);

    if (severity >= android::base::WARNING) {
        static bool early_boot_done = false;

        // If metadata encryption setup (fscrypt_mount_metadata_encrypted) or
        // basic FBE setup (fscrypt_init_user0) fails, then the boot will fail
        // before adb can be started, so logcat won't be available.  To allow
        // debugging these early boot failures, log early errors and warnings to
        // the kernel log.  This allows diagnosing failures via the serial log,
        // or via last dmesg/"fastboot oem dmesg" on devices that support it.
        //
        // As a very quick-and-dirty test for whether /data has been mounted,
        // check whether /data/misc/vold exists.
        if (!early_boot_done) {
            if (access("/data/misc/vold", F_OK) == 0 && fscrypt_init_user0_done) {
                early_boot_done = true;
                return;
            }
            android::base::KernelLogger(log_buffer_id, severity, tag, file, line, message);
        }
    }
}
