package ownapp.esp

class EspTarget(
    val x: Float,
    val z: Float,
    val name: String,
    val meters: Int,
    val hp: Int,
    val hpMax: Int,
    val monster: Boolean
)

object EspFeatures {
    @Volatile var box = true
    @Volatile var line = true
    @Volatile var health = true
    @Volatile var name = true
    @Volatile var distance = true
}

object EspState {
    @Volatile var ok = false
    @Volatile var text = ""
    @Volatile var resolveLog = ""
    @Volatile var lpX = 0f
    @Volatile var lpZ = 0f
    @Volatile var hasLp = false
    @Volatile var enemies: Array<EspTarget> = emptyArray()
    @Volatile var updatedNanos = 0L

    fun update(okIn: Boolean, textIn: String, enemiesIn: Array<EspTarget>, lx: Float, lz: Float, lp: Boolean) {
        ok = okIn
        text = textIn
        enemies = enemiesIn
        lpX = lx
        lpZ = lz
        hasLp = lp
        updatedNanos = System.nanoTime()
    }
}
