/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ANDROID_VOLD_KEYUTIL_H
#define ANDROID_VOLD_KEYUTIL_H

#include "KeyBuffer.h"
#include "KeyStorage.h"

#include <fscrypt/fscrypt.h>

#include <memory>
#include <string>

namespace android {
namespace vold {

// Description of how to generate a key when needed.
struct KeyGeneration {
    size_t keysize;
    bool allow_gen;
    bool use_hw_wrapped_key;
};

// Generate a key as specified in KeyGeneration
bool generateStorageKey(const KeyGeneration& gen, KeyBuffer* key);

// Returns a key with allow_gen false so generateStorageKey returns false;
// this is used to indicate to retrieveOrGenerateKey that a key should not
// be generated.
const KeyGeneration neverGen();

// Install a file-based encryption key to the kernel, for use by encrypted files
// on the specified filesystem using the specified encryption policy version.
//
// Returns %true on success, %false on failure.  On success also sets *policy
// to the EncryptionPolicy used to refer to this key.
bool installKey(const std::string& mountpoint, const android::fscrypt::EncryptionOptions& options,
                const KeyBuffer& key, android::fscrypt::EncryptionPolicy* policy);

// Evict a file-based encryption key from the kernel.
bool evictKey(const std::string& mountpoint, const android::fscrypt::EncryptionPolicy& policy);

// Retrieves the key from the named directory, or generates it if it doesn't
// exist.
bool retrieveOrGenerateKey(const std::string& key_path, const std::string& tmp_path,
                           const KeyAuthentication& key_authentication, const KeyGeneration& gen,
                           KeyBuffer* key);

}  // namespace vold
}  // namespace android

#endif
