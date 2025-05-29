package io.github.xororz.localdream

import android.Manifest
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import io.github.xororz.localdream.navigation.Screen
import io.github.xororz.localdream.ui.screens.ModelListScreen
import io.github.xororz.localdream.ui.screens.ModelRunScreen
import io.github.xororz.localdream.ui.theme.LocalDreamTheme
import androidx.core.content.ContextCompat
import android.content.pm.PackageManager
import io.github.xororz.localdream.data.ModelRepository

class MainActivity : ComponentActivity() {
    private val requestStoragePermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted: Boolean ->
        if (!isGranted) {
//            Toast.makeText(
//                this,
//                "Storage permission is required for saving generated images",
//                Toast.LENGTH_LONG
//            ).show()
        }
    }

    private val requestNotificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted: Boolean ->
        if (!isGranted) {
//            Toast.makeText(
//                this,
//                "Notification permission is required for background image generation",
//                Toast.LENGTH_LONG
//            ).show()
        }
    }

    private fun checkStoragePermission() {
        // < Android 10
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            when {
                ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
                ) == PackageManager.PERMISSION_GRANTED -> {
                    // ok
                }

                shouldShowRequestPermissionRationale(Manifest.permission.WRITE_EXTERNAL_STORAGE) -> {
//                    Toast.makeText(
//                        this,
//                        "Storage permission is needed for saving generated images",
//                        Toast.LENGTH_LONG
//                    ).show()
                    requestStoragePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                }

                else -> {
                    requestStoragePermissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                }
            }
        }
    }

    private fun checkNotificationPermission() {
        // > Android 13
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            when {
                ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.POST_NOTIFICATIONS
                ) == PackageManager.PERMISSION_GRANTED -> {
                    // ok
                }

                shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS) -> {
//                    Toast.makeText(
//                        this,
//                        "Notification permission is needed for background image generation",
//                        Toast.LENGTH_LONG
//                    ).show()
                    requestNotificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                }

                else -> {
                    requestNotificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        checkStoragePermission()
        checkNotificationPermission()

        // 외부 앱에서 온 인텐트 처리
        val autoGenerate = intent.getBooleanExtra("AUTO_GENERATE", false)
        val externalPrompt = intent.getStringExtra("PROMPT")
        val sourceApp = intent.getStringExtra("SOURCE_APP")

        setContent {
            LocalDreamTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    val navController = rememberNavController()

                    // 시작 화면 결정
                    val startDestination = if (autoGenerate && !externalPrompt.isNullOrEmpty()) {
                        // 첫 번째 다운로드된 모델로 바로 이동
                        findFirstDownloadedModel()?.let { modelId ->
                            Screen.ModelRun.createRoute(modelId)
                        } ?: Screen.ModelList.route
                    } else {
                        Screen.ModelList.route
                    }

                    NavHost(
                        navController = navController,
                        startDestination = startDestination
                    ) {
                        composable(Screen.ModelList.route) {
                            ModelListScreen(navController)
                        }
                        composable(
                            route = Screen.ModelRun.route,
                            arguments = listOf(
                                navArgument("modelId") { type = NavType.StringType }
                            )
                        ) { backStackEntry ->
                            ModelRunScreen(
                                modelId = backStackEntry.arguments?.getString("modelId") ?: "",
                                navController = navController,
                                externalPrompt = if (autoGenerate) externalPrompt else null,
                                sourceApp = sourceApp
                            )
                        }
                    }
                }
            }
        }
    }

    private fun findFirstDownloadedModel(): String? {
        val modelRepository = ModelRepository(this)
        return modelRepository.models.find { it.isDownloaded }?.id
    }
}
