// The model store from Kotlin, through the C API: a manifest of its own with file:// sources (so it
// runs offline), pulled with progress, stopped and resumed, verified, gated by its license, and the
// models an engine config's providers load. The port of bindings/python/tests/test_models.py.
package com.vasdk

import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class ModelsTest {
    private val abcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    private val millionSha256 = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"   // 1,000,000 x 'a'

    @Test
    fun modelStore() {
        assertTrue(Vasdk.apiVersion() >= 1)
        val root = Files.createTempDirectory("vasdk_models_test").toFile()
        try {
            val abc = File(root, "abc.txt").apply { writeBytes("abc".toByteArray()) }
            val million = File(root, "million.bin").apply { writeBytes(ByteArray(1000000) { 'a'.code.toByte() }) }
            File(root, "manifest.json").writeText(
                """{"schema": 1, "models": [
                  {"id": "small", "providers": ["vad/test"], "license": "MIT",
                   "files": [{"path": "small/abc.txt", "url": "${abc.toURI()}", "sha256": "$abcSha256", "size": 3}]},
                  {"id": "big", "providers": ["tts/test"], "license": "MIT",
                   "files": [{"path": "big.bin", "url": "${million.toURI()}", "sha256": "$millionSha256", "size": 1000000}]},
                  {"id": "terms", "providers": [], "license": "CC-BY-NC-4.0", "requires_acceptance": true,
                   "files": [{"path": "terms.txt", "url": "${abc.toURI()}", "sha256": "$abcSha256", "size": 3}]}]}""".replace("file:/", "file:///")
            )
            val store = File(root, "store")
            Models(directory = store.path, manifestPath = File(root, "manifest.json").path).use { models ->
                assertEquals(listOf("small", "big", "terms"), models.list().map { it.id })
                assertEquals("missing", models.status("small"))
                assertEquals("unknown", models.status("nothing"))

                val seen = mutableListOf<Long>()
                models.pull("small") { _, _, done, _ -> seen += done; true }
                assertEquals("present", models.status("small"))
                assertEquals("abc", File(store, "small/abc.txt").readText())
                assertEquals(3L, seen.last())
                models.verify("small")

                val stopped = assertFailsWith<VasdkException> { models.pull("big") { _, _, done, _ -> done < 500000 } }
                assertTrue("cancelled" in stopped.message!!, stopped.message)
                assertEquals("partial", models.status("big"))
                models.pull("big")
                models.verify("big")

                val gated = assertFailsWith<VasdkException> { models.pull("terms") }
                assertTrue("CC-BY-NC-4.0" in gated.message!!, gated.message)
                models.pull("terms", acceptLicense = true)

                val config = File(root, "engine.txtpb").apply { writeText("providers { vad { id: \"test\" } tts { id: \"test\" } }\n") }
                assertEquals(listOf("small", "big"), models.forConfig(config.path))

                models.remove("small")
                assertEquals("missing", models.status("small"))
            }
            Models(directory = store.path, manifestPath = File(root, "manifest.json").path, offline = true).use { offline ->
                val error = assertFailsWith<VasdkException> { offline.pull("small") }
                assertTrue("offline" in error.message!!, error.message)
            }
        } finally {
            root.deleteRecursively()
        }
        Models(directory = System.getProperty("java.io.tmpdir")).use { builtIn ->
            val ids = builtIn.list().map { it.id }
            assertTrue("silero_vad" in ids && "pocket_tts" in ids, ids.toString())
        }
    }
}
