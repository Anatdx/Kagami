package com.anatdx.kagami

import android.content.Intent
import android.os.IBinder
import com.topjohnwu.superuser.ipc.RootService
import java.io.BufferedReader
import java.io.InputStreamReader

class KasumiRootService : RootService() {
    private val binder = object : IKasumiRootService.Stub() {
        override fun snapshotJson(ignoreProtocolMismatch: Boolean): String =
            KasumiNative.snapshotJson(ignoreProtocolMismatch)

        override fun setEnabled(enabled: Boolean): String =
            KasumiNative.setEnabled(enabled)

        override fun setDebug(enabled: Boolean): String =
            KasumiNative.setDebug(enabled)

        override fun setMountHide(enabled: Boolean): String =
            KasumiNative.setMountHide(enabled)

        override fun setMapsSpoof(enabled: Boolean): String =
            KasumiNative.setMapsSpoof(enabled)

        override fun setStatfsSpoof(enabled: Boolean): String =
            KasumiNative.setStatfsSpoof(enabled)

        override fun setSelinuxGuard(enabled: Boolean): String =
            KasumiNative.setSelinuxGuard(enabled)

        override fun hidePath(path: String): String =
            KasumiNative.hidePath(path)

        override fun deleteRule(path: String): String =
            KasumiNative.deleteRule(path)

        override fun addMapsRule(targetIno: String, targetDev: String, spoofedIno: String, spoofedDev: String, spoofedPath: String): String =
            KasumiNative.addMapsRule(targetIno, targetDev, spoofedIno, spoofedDev, spoofedPath)

        override fun clearMapsRules(): String =
            KasumiNative.clearMapsRules()

        override fun setPolicy(owner: String, flags: Int): String =
            KasumiNative.setPolicy(owner, flags)

        override fun setPolicyUids(list: String, uids: IntArray): String =
            KasumiNative.setPolicyUids(list, uids)

        override fun clearPolicyUids(list: String): String =
            KasumiNative.clearPolicyUids(list)

        override fun kernelLog(): String =
            runCommand("dmesg | tail -n 240")

        override fun statPath(path: String): String =
            KasumiNative.statPath(path)
    }

    override fun onBind(intent: Intent): IBinder = binder

    private fun runCommand(command: String): String {
        return runCatching {
            val process = ProcessBuilder("sh", "-c", command)
                .redirectErrorStream(true)
                .start()
            val output = BufferedReader(InputStreamReader(process.inputStream)).use { it.readText() }
            val code = process.waitFor()
            if (code == 0) output.trimEnd() else "exit=$code\n${output.trimEnd()}"
        }.getOrElse { it.message ?: it::class.java.simpleName }
    }
}
