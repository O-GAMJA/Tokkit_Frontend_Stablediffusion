package io.github.xororz.localdream.data

import android.content.Context
import android.os.Build
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import io.github.xororz.localdream.BuildConfig
import io.github.xororz.localdream.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import java.io.File

private fun getDeviceSoc(): String {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        Build.SOC_MODEL
    } else {
        "CPU"
    }
}

data class ModelFile(
    val name: String,
    val displayName: String,
    val uri: String
)

data class DownloadProgress(
    val displayName: String,
    val currentFileIndex: Int,
    val totalFiles: Int,
    val progress: Float,
    val downloadedBytes: Long,
    val totalBytes: Long
)

val chipsetModelSuffixes = mapOf(
    "SM8475" to "8gen1",
    "SM8450" to "8gen1",
    "SM8550" to "8gen2",
    "QCS8550" to "8gen2",
    "QCM8550" to "8gen2",
    "SM8650" to "8gen3",
    "SM8750" to "8gen4",
)

sealed class DownloadResult {
    data object Success : DownloadResult()
    data class Error(val message: String) : DownloadResult()
    data class Progress(val progress: DownloadProgress) : DownloadResult()
}

data class Model(
    val id: String,
    val name: String,
    val description: String,
    val baseUrl: String,
    val files: List<ModelFile> = emptyList(),
    val generationSize: Int = 512,
    val textEmbeddingSize: Int = 768,
    val approximateSize: String = "1GB",
    val isDownloaded: Boolean = false,
    val defaultPrompt: String = "",
    val defaultNegativePrompt: String = "",
    val runOnCpu: Boolean = false,
    val useCpuClip: Boolean = false
) {
    fun download(context: Context): Flow<DownloadResult> = flow {
        val modelsDir = getModelsDir(context)
        val modelDir = File(modelsDir, id).apply {
            if (!exists()) mkdirs()
        }

        val downloadManager = DownloadManager(context)
        val fileVerification = FileVerification(context)

        try {
            downloadManager.downloadWithResume(
                modelId = id,
                files = files,
                baseUrl = baseUrl,
                modelDir = modelDir
            ).collect { result ->
                emit(result)
            }
        } catch (e: Exception) {
            fileVerification.clearVerification(id)
            modelDir.deleteRecursively()
            emit(DownloadResult.Error(e.message ?: "Download failed"))
        }
    }.flowOn(Dispatchers.IO)

    fun deleteModel(context: Context): Boolean {
        return try {
            val modelDir = File(getModelsDir(context), id)
            val fileVerification = FileVerification(context)

            runBlocking {
                fileVerification.clearVerification(id)
            }

            if (modelDir.exists() && modelDir.isDirectory) {
                val deleted = modelDir.deleteRecursively()
                android.util.Log.d("Model", "Delete model $id: $deleted")
                deleted
            } else {
                android.util.Log.d("Model", "Model does not exist: $id")
                false
            }
        } catch (e: Exception) {
            android.util.Log.e("Model", "error: ${e.message}")
            false
        }
    }

    companion object {
        private const val MODELS_DIR = "models"

        fun isDeviceSupported(): Boolean {
            return getDeviceSoc() in chipsetModelSuffixes
        }

        fun getModelsDir(context: Context): File {
            return File(context.filesDir, MODELS_DIR).apply {
                if (!exists()) mkdirs()
            }
        }

        fun checkModelExists(context: Context, modelId: String, files: List<ModelFile>): Boolean {
            val modelDir = File(getModelsDir(context), modelId)
            val fileVerification = FileVerification(context)

            return runBlocking {
                files.all { modelFile ->
                    val file = File(modelDir, modelFile.name)
                    if (!file.exists()) return@all false

                    val savedSize = fileVerification.getFileSize(modelId, modelFile.name)
                    savedSize != null && file.length() == savedSize
                }
            }
        }
    }
}

class ModelRepository(private val context: Context) {
    private val generationPreferences = GenerationPreferences(context)

    private var _baseUrl = mutableStateOf("https://huggingface.co/")
    var baseUrl: String
        get() = _baseUrl.value
        private set(value) {
            _baseUrl.value = value
        }

    var models by mutableStateOf(initializeModels())
        private set

    init {
        CoroutineScope(Dispatchers.Main).launch {
            generationPreferences.getBaseUrl().collect { url ->
                baseUrl = url
                models = initializeModels()
            }
        }
    }

    fun updateBaseUrl(newUrl: String) {
        baseUrl = newUrl
        models = initializeModels()
    }

    private fun initializeModels(): List<Model> {
        // Only NPU Stable Diffusion 2.1 model
        return listOf(
            createSD21Model()
        )
    }

    private fun createSD21Model(): Model {
        val id = "sd21"
        val soc = getDeviceSoc()
        return Model(
            id = id,
            name = "Stable Diffusion 2.1",
            description = context.getString(R.string.sd21_description),
            baseUrl = baseUrl,
            files = listOf(
                ModelFile("tokenizer.json", "tokenizer", "xororz/SD21/resolve/main/tokenizer.json"),
                ModelFile(
                    "clip.bin",
                    "clip",
                    "xororz/SD21/resolve/main/clip_${chipsetModelSuffixes[soc]}.bin"
                ),
                ModelFile(
                    "vae_encoder.bin",
                    "vae_encoder",
                    "xororz/AnythingV5/resolve/main/vae_encoder_${chipsetModelSuffixes[soc]}.bin"
                ),
                ModelFile(
                    "vae_decoder.bin",
                    "vae_decoder",
                    "xororz/SD21/resolve/main/vae_decoder_${chipsetModelSuffixes[soc]}.bin"
                ),
                ModelFile(
                    "unet.bin",
                    "unet",
                    "xororz/SD21/resolve/main/unet_${chipsetModelSuffixes[soc]}.bin"
                )
            ),
            textEmbeddingSize = 1024,
            approximateSize = "1.3GB",
            isDownloaded = Model.checkModelExists(
                context,
                id,
                listOf(
                    ModelFile("tokenizer.json", "", ""),
                    ModelFile("clip.bin", "", ""),
                    ModelFile("vae_encoder.bin", "", ""),
                    ModelFile("vae_decoder.bin", "", ""),
                    ModelFile("unet.bin", "", "")
                )
            ),
            defaultPrompt = "a rabbit on grass,",
            defaultNegativePrompt = "lowres, bad anatomy, bad hands, missing fingers, extra digit, fewer fingers, cropped, worst quality, low quality, blur, simple background, mutation, deformed, ugly, duplicate, error, jpeg artifacts, watermark, username, blurry"
        )
    }

    fun refreshModelState(modelId: String) {
        models = models.map { model ->
            if (model.id == modelId) {
                model.copy(
                    isDownloaded = Model.checkModelExists(context, modelId, model.files)
                )
            } else {
                model
            }
        }
    }

    fun refreshAllModels() {
        models = models.map { model ->
            model.copy(
                isDownloaded = Model.checkModelExists(context, model.id, model.files)
            )
        }
    }
}