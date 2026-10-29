package com.vasdk.android.ui

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.center
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.unit.dp
import kotlin.math.cos
import kotlin.math.sin

/**
 * Voice orb: 8 alpha-blended blobs (5 driven by individual VizAnim bands
 * bass→treble, 3 by weighted combinations - body/voice/air), each with its own
 * orbit speed/phase and size/brightness/colour tied to its drive energy, in
 * a four-hue blue/red/yellow/green scheme. [palette] tells the two sides
 * apart (blue/green mic, red/yellow TTS agent). Blending is normal SrcOver
 * rather than additive so the colours stay saturated on the light background
 * instead of summing to white.
 *
 * [level] is 0..1 overall loudness, [bars] the per-band scales from the
 * ViewModel. [thinking] swaps the audio drive for a shared pulse (the
 * agent-is-working idle). [onTap] turns the orb into a button (stop control
 * while busy).
 */
@Composable
internal fun VoiceOrb(
    level: Float,
    bars: List<Float>,
    modifier: Modifier = Modifier,
    palette: OrbPalette = MicOrbPalette,
    thinking: Boolean = false,
    onTap: (() -> Unit)? = null
) {
    val animatedLevel by animateFloatAsState(
        targetValue = level.coerceIn(0f, 1f),
        animationSpec = tween(durationMillis = 240),
        label = "orb-level"
    )

    val transition = rememberInfiniteTransition(label = "orb-motion")

    // Idle breathing so the orb never looks frozen.
    val breath by transition.animateFloat(
        initialValue = 0.94f,
        targetValue = 1.03f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 2100, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "orb-breath"
    )

    // Single master clock; every blob derives its own orbit angle from it so we
    // run one infinite animation instead of five.
    val masterDeg by transition.animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 24_000, easing = LinearEasing),
            repeatMode = RepeatMode.Restart
        ),
        label = "orb-clock"
    )

    // Thinking drive: a metronome swell that replaces the audio energies so the
    // orb visibly pulsates on repeat while the LLM works, then hands over to the
    // band-driven animation the moment speech starts.
    val thinkPulse by transition.animateFloat(
        initialValue = 0.08f,
        targetValue = 0.85f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 900, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "orb-think"
    )

    // Map band scales (0.15..1.5) back to 0..1 energies, smoothed to the mic's
    // ~250 ms feature cadence.
    fun rawBand(index: Int) =
        ((bars.getOrElse(index) { 0.15f } - 0.15f) / 1.35f).coerceIn(0f, 1f)

    val band0 by animateFloatAsState(rawBand(0), tween(240), label = "orb-b0")
    val band1 by animateFloatAsState(rawBand(1), tween(240), label = "orb-b1")
    val band2 by animateFloatAsState(rawBand(2), tween(240), label = "orb-b2")
    val band3 by animateFloatAsState(rawBand(3), tween(240), label = "orb-b3")
    val band4 by animateFloatAsState(rawBand(4), tween(240), label = "orb-b4")
    val bands = listOf(band0, band1, band2, band3, band4)

    val effectiveLevel = if (thinking) thinkPulse else animatedLevel

    val interaction = remember { MutableInteractionSource() }
    val tapModifier = if (onTap != null) {
        Modifier.clickable(
            interactionSource = interaction,
            indication = null,
            onClick = onTap
        )
    } else {
        Modifier
    }

    Canvas(modifier = modifier.size(160.dp).then(tapModifier)) {
        val c = size.center
        // Headroom so glow and blob excursions never clip.
        val orbR = size.minDimension / 2f * 0.60f * breath * (1f + 0.18f * effectiveLevel)

        // Ambient halo that widens and brightens with overall loudness.
        drawCircle(
            brush = Brush.radialGradient(
                colors = listOf(
                    palette.halo.copy(alpha = 0.20f + 0.30f * effectiveLevel),
                    palette.halo.copy(alpha = 0.05f),
                    Color.Transparent
                ),
                center = c,
                radius = orbR * 1.65f
            ),
            radius = orbR * 1.65f,
            center = c
        )

        // One blob per drive: five single bands (bass big/slow/deep → treble
        // small/quick/bright) plus three band combinations. Their soft radial
        // edges overlap into one living sphere.
        OrbLayers.forEachIndexed { index, layer ->
            // Drive energy = weighted mix of the band energies; while thinking
            // every blob follows the shared pulse instead.
            var energy = if (thinking) {
                thinkPulse
            } else {
                var mixed = 0f
                for (i in bands.indices) mixed += layer.weights[i] * bands[i]
                mixed
            }
            energy = energy.coerceIn(0f, 1f)

            val angleRad = Math.toRadians(
                (layer.phaseDeg + masterDeg * layer.speed).toDouble()
            )
            // Louder drive → the blob roams further off-centre.
            val orbit = orbR * layer.orbitFrac * (0.55f + 0.9f * energy)
            val blobCenter = c + Offset(
                x = cos(angleRad).toFloat(),
                y = sin(angleRad).toFloat()
            ) * orbit
            val blobR = orbR * layer.radiusFrac * (0.80f + 0.55f * energy)
            val alpha = 0.30f + 0.55f * energy
            // Colour comes alive with the drive: calm shade at rest, excited
            // shade at full energy.
            val colors = palette.layers[index]
            val color = lerp(colors.calm, colors.excited, energy)

            drawCircle(
                brush = Brush.radialGradient(
                    colors = listOf(
                        color.copy(alpha = alpha),
                        color.copy(alpha = alpha * 0.35f),
                        Color.Transparent
                    ),
                    center = blobCenter,
                    radius = blobR
                ),
                radius = blobR,
                center = blobCenter
            )
        }
    }
}

/** Calm (energy 0) and excited (energy 1) shades for one blob. */
internal class OrbLayerColors(
    val calm: Color,
    val excited: Color
)

/**
 * Colour scheme for the orb. [layers] must have one entry per [OrbLayers]
 * element, in the same order (bass, lowMid, mid, hiMid, treble, body, voice, air).
 */
internal class OrbPalette(
    /** Ambient glow behind the sphere. */
    val halo: Color,
    val layers: List<OrbLayerColors>
)

// Core orb hues plus lighter tints and deeper shades of each.
private val OrbBlue = Color(0xFF4285F4)
private val OrbRed = Color(0xFFEA4335)
private val OrbYellow = Color(0xFFFBBC04)
private val OrbGreen = Color(0xFF34A853)

/** User's mic: blue that shifts toward green/cyan as the voice rises. */
internal val MicOrbPalette = OrbPalette(
    halo = OrbBlue,
    layers = listOf(
        // Single bands (bass, lowMid, mid, hiMid, treble)
        OrbLayerColors(Color(0xFF1967D2), OrbBlue),
        OrbLayerColors(Color(0xFF185ABC), Color(0xFF12B5CB)),
        OrbLayerColors(OrbBlue, OrbGreen),
        OrbLayerColors(Color(0xFF8AB4F8), Color(0xFF81C995)),
        OrbLayerColors(Color(0xFFAECBFA), Color(0xFFA8DAB5)),
        // Combinations: body flushes purple, voice shifts to cyan, air sparks
        // yellow on sibilance.
        OrbLayerColors(Color(0xFF174EA6), Color(0xFFA142F4)),
        OrbLayerColors(Color(0xFF1A73E8), Color(0xFF12B5CB)),
        OrbLayerColors(Color(0xFF8AB4F8), OrbYellow)
    )
)

/** Agent's TTS: red that heats toward orange/yellow as the voice rises. */
internal val SynthOrbPalette = OrbPalette(
    halo = OrbRed,
    layers = listOf(
        // Single bands (bass, lowMid, mid, hiMid, treble)
        OrbLayerColors(Color(0xFFC5221F), OrbRed),
        OrbLayerColors(Color(0xFFD93025), Color(0xFFFA7B17)),
        OrbLayerColors(OrbRed, OrbYellow),
        OrbLayerColors(Color(0xFFF28B82), Color(0xFFFDD663)),
        OrbLayerColors(Color(0xFFFAD2CF), Color(0xFFFDE293)),
        // Combinations: body deepens to purple, voice glows yellow, air sparks
        // blue on sibilance - so all four core hues show while it speaks.
        OrbLayerColors(Color(0xFFA50E0E), Color(0xFFA142F4)),
        OrbLayerColors(Color(0xFFD93025), OrbYellow),
        OrbLayerColors(Color(0xFFF28B82), OrbBlue)
    )
)

private class OrbLayer(
    /** Orbit angular speed as a multiple of the master clock. */
    val speed: Float,
    val phaseDeg: Float,
    /** Blob radius as a fraction of the orb radius. */
    val radiusFrac: Float,
    /** How far the blob may wander from the centre, as a fraction of orb radius. */
    val orbitFrac: Float,
    /** Per-band weights (bass..treble) mixed into this blob's drive energy. */
    val weights: FloatArray
)

// Five single-band blobs - deep/slow/large at the bass end, bright/fast/small at
// the treble end - plus three combination blobs mixing several bands. Phases
// spread the blobs around the sphere so they never clump at rest. Colours come
// from the active OrbPalette, matched to this list by position.
private val OrbLayers = listOf(
    // Single bands (bass, lowMid, mid, hiMid, treble)
    OrbLayer(
        speed = 1.0f, phaseDeg = 0f,
        radiusFrac = 0.95f, orbitFrac = 0.16f, weights = floatArrayOf(1f, 0f, 0f, 0f, 0f)
    ),
    OrbLayer(
        speed = -1.6f, phaseDeg = 72f,
        radiusFrac = 0.80f, orbitFrac = 0.22f, weights = floatArrayOf(0f, 1f, 0f, 0f, 0f)
    ),
    OrbLayer(
        speed = 2.3f, phaseDeg = 144f,
        radiusFrac = 0.66f, orbitFrac = 0.28f, weights = floatArrayOf(0f, 0f, 1f, 0f, 0f)
    ),
    OrbLayer(
        speed = -3.1f, phaseDeg = 216f,
        radiusFrac = 0.52f, orbitFrac = 0.34f, weights = floatArrayOf(0f, 0f, 0f, 1f, 0f)
    ),
    OrbLayer(
        speed = 4.0f, phaseDeg = 288f,
        radiusFrac = 0.38f, orbitFrac = 0.40f, weights = floatArrayOf(0f, 0f, 0f, 0f, 1f)
    ),
    // Combinations
    // "Body" - low-end weight of the voice; big slow counter-rotating mass.
    OrbLayer(
        speed = -0.7f, phaseDeg = 36f,
        radiusFrac = 0.88f, orbitFrac = 0.14f, weights = floatArrayOf(0.55f, 0.45f, 0f, 0f, 0f)
    ),
    // "Voice" - vowel/formant region; mid-sized blob.
    OrbLayer(
        speed = 1.9f, phaseDeg = 180f,
        radiusFrac = 0.60f, orbitFrac = 0.30f, weights = floatArrayOf(0.10f, 0.25f, 0.40f, 0.20f, 0.05f)
    ),
    // "Air" - sibilance and sparkle; small fast blob.
    OrbLayer(
        speed = -4.6f, phaseDeg = 300f,
        radiusFrac = 0.34f, orbitFrac = 0.44f, weights = floatArrayOf(0f, 0f, 0f, 0.45f, 0.55f)
    )
)
