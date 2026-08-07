/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.xiaomi.hardware.keyboardnanoapp_aidl;

@VintfStability
oneway interface INanoappCallback_aidl {
    void dataReceive_aidl(in byte[] buf);
    void errorReceive_aidl(int errorCode);
}
