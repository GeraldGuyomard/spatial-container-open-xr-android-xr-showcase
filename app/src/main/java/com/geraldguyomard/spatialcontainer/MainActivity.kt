/*
 * Copyright 2026 Gerald Guyomard
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.geraldguyomard.spatialcontainer

import android.content.Intent
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.xr.compose.spatial.Subspace
import androidx.xr.compose.subspace.SpatialActivityPanel
import androidx.xr.compose.subspace.SpatialPanel
import androidx.xr.compose.subspace.rememberSpatialActivityPanelController
import androidx.xr.compose.subspace.layout.SubspaceModifier
import androidx.xr.compose.subspace.layout.height
import androidx.xr.compose.subspace.layout.movable
import androidx.xr.compose.subspace.layout.resizable
import androidx.xr.compose.subspace.layout.width
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(LogTag, "MainActivity created")
        setContent {
            MaterialTheme {
                Subspace {
                    SpatialPanel(
                        SubspaceModifier.width(480.dp).height(400.dp).movable().resizable()
                    ) {
                        ControlPanel()
                    }
                    // The OpenXR container runs in its own activity so this one stays a plain
                    // Compose XR activity (an activity that renders with OpenXR loses its panels).
                    // The hosted activity's own 2D window is only a gray placeholder: the container
                    // is drawn separately by OpenXR. Keep its panel tiny so it is effectively hidden.
                    val controller = rememberSpatialActivityPanelController(
                        Intent(this@MainActivity, ContainerActivity::class.java)
                    )
                    SpatialActivityPanel(controller, SubspaceModifier.width(1.dp).height(1.dp))
                }
            }
        }
    }
}

@Composable
private fun ControlPanel() {
    var immersive by remember { mutableStateOf(false) }
    var backend by remember { mutableStateOf(ContainerRenderer.Backend.WEBGPU) }
    var switching by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    // The mode is owned by the native container; poll it so the panel follows system changes too.
    LaunchedEffect(Unit) {
        while (true) {
            immersive = ContainerRenderer.isImmersive()
            backend = ContainerRenderer.backend()
            delay(200)
        }
    }
    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier.padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("Spatial Container demo", style = MaterialTheme.typography.headlineSmall)
            Text("Container mode: ${if (immersive) "immersive" else "bounded"}")
            Button(onClick = { ContainerRenderer.toggleBoundsMode() }) {
                Text(if (immersive) "Switch to bounded" else "Switch to immersive")
            }
            Text(
                "Renderer: ${backend.label}" +
                    if (backend == ContainerRenderer.Backend.VULKAN) " (with layer depth)" else "",
            )
            val other =
                if (backend == ContainerRenderer.Backend.WEBGPU) ContainerRenderer.Backend.VULKAN
                else ContainerRenderer.Backend.WEBGPU
            Button(
                enabled = !switching,
                onClick = {
                    switching = true
                    scope.launch {
                        // Recreates the OpenXR session: keep it off the main thread.
                        withContext(Dispatchers.Default) { ContainerRenderer.setBackend(other) }
                        backend = ContainerRenderer.backend()
                        switching = false
                    }
                },
            ) {
                Text(if (switching) "Switching…" else "Switch to ${other.label}")
            }
        }
    }
}
