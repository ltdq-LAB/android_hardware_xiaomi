/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.xiaomi.hardware.keyboardnanoapp_aidl;

import vendor.xiaomi.hardware.keyboardnanoapp_aidl.INanoappCallback_aidl;

@VintfStability
interface IKeyboardNanoapp_aidl {
    int sendCmd_aidl(in byte[] buf);
    void setCallback_aidl(INanoappCallback_aidl mNanoappCallback);
}
