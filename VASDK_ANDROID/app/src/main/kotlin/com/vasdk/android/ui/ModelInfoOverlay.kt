package com.vasdk.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.ModelInfo
import com.vasdk.android.ToolInfo
import com.vasdk.android.ToolParamInfo

@Composable
internal fun ModelInfoOverlay(info: ModelInfo?, onCloseClick: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgDark)
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(SurfaceColor)
                    .padding(horizontal = 20.dp, vertical = 16.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "Model Info",
                    color = TextPrimary,
                    fontWeight = FontWeight.Bold,
                    fontSize = 17.sp,
                    modifier = Modifier.weight(1f)
                )
                IconButton(onClick = onCloseClick) {
                    Icon(imageVector = Icons.Default.Close, contentDescription = "Close", tint = Color.White)
                }
            }

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(20.dp)
            ) {
                if (info == null) {
                    Text(text = "Model info isn't available yet.", color = TextSecondary, fontSize = 14.sp)
                } else {
                    OverviewSection(info)
                    SystemPromptSection(info.systemPrompt)
                    ToolsSection(info.tools)
                    RawPromptSection(info.isLocal, info.rawPrompt)
                }
            }
        }
    }
}

@Composable
private fun SectionHeader(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text.uppercase(),
        color = Accent,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 0.8.sp,
        modifier = modifier.padding(bottom = 10.dp)
    )
}

@Composable
private fun OverviewSection(info: ModelInfo) {
    Column(modifier = Modifier.padding(bottom = 28.dp)) {
        SectionHeader("Overview")
        Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
            Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                InfoRow(label = "Mode", value = if (info.isLocal) "On-device" else "Cloud")
                InfoRow(label = "Model", value = info.modelName)
                if (info.isLocal) {
                    InfoRow(label = "Chat format", value = info.chatFormat)
                }
            }
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 10.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(text = label, color = TextSecondary, fontSize = 13.sp)
        Text(text = value, color = TextPrimary, fontSize = 13.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun SystemPromptSection(systemPrompt: String) {
    Column(modifier = Modifier.padding(bottom = 28.dp)) {
        SectionHeader("System prompt")
        Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
            Text(
                text = systemPrompt.ifEmpty { "(empty)" },
                color = TextPrimary,
                fontSize = 13.sp,
                lineHeight = 19.sp,
                modifier = Modifier.padding(16.dp)
            )
        }
    }
}

@Composable
private fun ToolsSection(tools: List<ToolInfo>) {
    Column(modifier = Modifier.padding(bottom = 28.dp)) {
        SectionHeader("Tools (${tools.size})")
        if (tools.isEmpty()) {
            Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
                Text(
                    text = "No tools registered.",
                    color = TextSecondary,
                    fontSize = 13.sp,
                    modifier = Modifier.padding(16.dp)
                )
            }
        } else {
            tools.forEachIndexed { index, tool ->
                ToolCard(tool, modifier = Modifier.padding(top = if (index == 0) 0.dp else 10.dp))
            }
        }
    }
}

@Composable
private fun ToolCard(tool: ToolInfo, modifier: Modifier = Modifier) {
    Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp), modifier = modifier) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = tool.name,
                color = Accent,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace
            )
            if (tool.description.isNotEmpty()) {
                Text(
                    text = tool.description,
                    color = TextSecondary,
                    fontSize = 12.sp,
                    lineHeight = 17.sp,
                    modifier = Modifier.padding(top = 4.dp)
                )
            }
            if (tool.parameters.isNotEmpty()) {
                Column(modifier = Modifier.padding(top = 12.dp)) {
                    tool.parameters.forEach { param ->
                        ParamRow(param, modifier = Modifier.padding(top = 6.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun ParamRow(param: ToolParamInfo, modifier: Modifier = Modifier) {
    Column(modifier = modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = param.name,
                color = TextPrimary,
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium,
                fontFamily = FontFamily.Monospace
            )
            Text(
                text = " ${param.type}",
                color = TextSecondary,
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace
            )
            Text(
                text = if (param.required) "required" else "optional",
                color = if (param.required) MicActive else TextSecondary.copy(alpha = 0.7f),
                fontSize = 10.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier
                    .padding(start = 8.dp)
                    .background(SurfaceColor, RoundedCornerShape(999.dp))
                    .padding(horizontal = 7.dp, vertical = 2.dp)
            )
        }
        if (param.description.isNotEmpty()) {
            Text(
                text = param.description,
                color = TextSecondary,
                fontSize = 11.sp,
                lineHeight = 15.sp,
                modifier = Modifier.padding(top = 2.dp)
            )
        }
    }
}

@Composable
private fun RawPromptSection(isLocal: Boolean, rawPrompt: String) {
    Column {
        SectionHeader(if (isLocal) "Rendered prompt" else "Request payload")
        Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
            Text(
                text = rawPrompt.ifEmpty { "(empty)" },
                color = TextPrimary,
                fontSize = 12.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.padding(14.dp)
            )
        }
    }
}
