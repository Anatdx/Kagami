package com.anatdx.kagami

internal object KasumiNative {
    init {
        System.loadLibrary("kagami_client")
    }

    external fun snapshotJson(ignoreProtocolMismatch: Boolean): String
    external fun setEnabled(enabled: Boolean): String
    external fun setDebug(enabled: Boolean): String
    external fun setMountHide(enabled: Boolean): String
    external fun setMapsSpoof(enabled: Boolean): String
    external fun setStatfsSpoof(enabled: Boolean): String
    external fun setSelinuxGuard(enabled: Boolean): String
    external fun hidePath(path: String): String
    external fun deleteRule(path: String): String
    external fun addMapsRule(targetIno: String, targetDev: String, spoofedIno: String, spoofedDev: String, spoofedPath: String): String
    external fun clearMapsRules(): String
    external fun setPolicy(owner: String, flags: Int): String
    external fun setPolicyUids(list: String, uids: IntArray): String
    external fun clearPolicyUids(list: String): String
    external fun statPath(path: String): String
}
