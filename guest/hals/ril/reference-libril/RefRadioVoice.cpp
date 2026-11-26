/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "RefRadioVoice.h"
#include "ril_service.h"

#include <atomic>
#include <log/log.h>
#include <telephony/ril.h>

namespace cf::ril {

using ::ndk::ScopedAStatus;
using namespace ::aidl::android::hardware::radio;
constexpr auto ok = &ScopedAStatus::ok;

namespace {

constexpr int kMaxSlots = 4;
std::atomic<bool> gVoNrEnabled[kMaxSlots] = {true, true, true, true};

RadioResponseInfo responseInfo(int32_t serial, RadioError error = RadioError::NONE) {
    return {
            .type = RadioResponseType::SOLICITED,
            .serial = serial,
            .error = error,
    };
}

}  // namespace

bool isVoNrEnabledForSlot(int slotId) {
    if (slotId < 0 || slotId >= kMaxSlots) {
        return true;
    }
    return gVoNrEnabled[slotId].load(std::memory_order_relaxed);
}

void setVoNrEnabledForSlot(int slotId, bool enabled) {
    if (slotId < 0 || slotId >= kMaxSlots) {
        return;
    }
    gVoNrEnabled[slotId].store(enabled, std::memory_order_relaxed);
}

ScopedAStatus RefRadioVoice::isVoNrEnabled(int32_t serial) {
    bool enabled = isVoNrEnabledForSlot(mSlotId);
    RLOGD("RefRadioVoice[%d]::isVoNrEnabled: serial=%d enabled=%d", mSlotId, serial, enabled);
    respond()->isVoNrEnabledResponse(responseInfo(serial), enabled);
    return ok();
}

ScopedAStatus RefRadioVoice::setVoNrEnabled(int32_t serial, bool enable) {
    bool prev = isVoNrEnabledForSlot(mSlotId);
    setVoNrEnabledForSlot(mSlotId, enable);
    RLOGI("RefRadioVoice[%d]::setVoNrEnabled: serial=%d enable=%d (prev=%d)",
          mSlotId, serial, enable, prev);
    respond()->setVoNrEnabledResponse(responseInfo(serial));
    if (prev != enable) {
        radio_1_6::networkStateChangedInd(mSlotId, RESPONSE_UNSOLICITED, 0,
                                          RIL_E_SUCCESS, nullptr, 0);
    }
    return ok();
}

}  // namespace cf::ril
