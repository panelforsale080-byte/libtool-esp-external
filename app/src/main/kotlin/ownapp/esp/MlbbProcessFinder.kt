package ownapp.esp

import java.io.File

object MlbbProcessFinder {

    data class Target(val pid: Int, val cmdline: String)

    fun findUnityKills(): Target? {
        var best: Target? = null
        var bestScore = Int.MIN_VALUE
        val proc = File("/proc")
        for (d in proc.listFiles() ?: return null) {
            if (!d.isDirectory) continue
            val name = d.name
            if (name.isEmpty() || !name.all { it.isDigit() }) continue
            try {
                val cmd = String(d.resolve("cmdline").readBytes(), Charsets.UTF_8)
                    .trim('\u0000')
                if (!cmd.contains("UnityKills", ignoreCase = true)) continue
                if (!cmd.contains("mobile.legends", ignoreCase = true) &&
                    !cmd.contains("com.mobile.legends", ignoreCase = true)
                ) continue
                var score = 100
                if (cmd.contains("UnityKillsMe", ignoreCase = true)) score += 50
                if (cmd.contains(":")) score += 10
                if (score > bestScore) {
                    bestScore = score
                    best = Target(name.toInt(), cmd)
                }
            } catch (_: Exception) {
            }
        }
        return best
    }

    fun listCandidates(): List<Target> {
        val out = mutableListOf<Target>()
        val proc = File("/proc")
        for (d in proc.listFiles() ?: return emptyList()) {
            if (!d.isDirectory) continue
            val name = d.name
            if (name.isEmpty() || !name.all { it.isDigit() }) continue
            try {
                val cmd = String(d.resolve("cmdline").readBytes(), Charsets.UTF_8).trim('\u0000')
                if (cmd.contains("mobile.legends", ignoreCase = true) ||
                    cmd.contains("UnityKills", ignoreCase = true)
                ) {
                    out += Target(name.toInt(), cmd.ifBlank { name })
                }
            } catch (_: Exception) {
            }
        }
        return out.sortedByDescending { it.cmdline.contains("UnityKillsMe", true) }
    }
}
