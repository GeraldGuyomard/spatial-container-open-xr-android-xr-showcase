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

import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity

/**
 * Owns the OpenXR session and the spatial container. It is hosted by [MainActivity] in a
 * SpatialActivityPanel, so the system only treats this activity as OpenXR-rendering and leaves the
 * Compose activity's panels alone.
 */
class ContainerActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(LogTag, "ContainerActivity created")
        ContainerRenderer.start(this)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        Log.i(
            LogTag,
            "ContainerActivity config changed: ${newConfig.screenWidthDp}x${newConfig.screenHeightDp} dp, " +
                "smallest ${newConfig.smallestScreenWidthDp} dp, density ${newConfig.densityDpi}",
        )
    }

    override fun onDestroy() {
        Log.i(LogTag, "ContainerActivity destroyed (changing configurations: $isChangingConfigurations)")
        ContainerRenderer.stop()
        super.onDestroy()
    }
}
