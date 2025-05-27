#include <iostream>
#include <memory>
#include <string>

#include "BuildId.hpp"
#include "DynamicLoadUtil.hpp"
#include "Logger.hpp"
#include "PAL/DynamicLoading.hpp"
#include "PAL/GetOpt.hpp"
#include "QnnModel.hpp"
#include "QnnSampleAppUtils.hpp"

#include <fstream>
#include "tokenizers_cpp.h"
#include "httplib.h"
#include "json.hpp"
#include "DPMSolverMultistepScheduler.hpp"
#include "Config.hpp"
#include "SDUtils.hpp"
#include "QnnModel.hpp"

#include <MNN/Interpreter.hpp>
#include <MNN/MNNDefine.h>

int port = 8081;
std::string listen_address = "127.0.0.1";

static void *sg_backendHandle_clip{nullptr};
static void *sg_backendHandle_unet{nullptr};
static void *sg_backendHandle_vae_decoder{nullptr};
static void *sg_backendHandle_vae_encoder{nullptr};
static void *sg_modelHandle{nullptr};

std::shared_ptr<tokenizers::Tokenizer> g_tokenizer;
std::unordered_map<std::string, int> g_token2id;
std::unordered_map<int, std::string> g_id2token;

MNN::Session *safetyCheckerSession;
bool use_safety_checker = false;
bool img2img = false;
float nsfw_threshold = 0.5f;

namespace qnn
{
    namespace tools
    {
        namespace sample_app
        {
            template <typename AppType>
            int initializeQnnApp(const std::string &modelPath,
                                 std::unique_ptr<AppType> &app,
                                 bool loadFromCachedBinary,
                                 const std::string &appType)
            {
                if (modelPath.empty() || nullptr == app)
                {
                    return EXIT_FAILURE;
                }

                QNN_INFO("qnn-%s-app build version: %s", appType.c_str(), qnn::tools::getBuildId().c_str());
                QNN_INFO("Backend        build version: %s", app->getBackendBuildId().c_str());

                if (sample_app::StatusCode::SUCCESS != app->initialize())
                {
                    return app->reportError(appType + " Initialization failure");
                }
                if (sample_app::StatusCode::SUCCESS != app->initializeBackend())
                {
                    return app->reportError(appType + " Backend Initialization failure");
                }
                auto devicePropertySupportStatus = app->isDevicePropertySupported();
                if (sample_app::StatusCode::FAILURE != devicePropertySupportStatus)
                {
                    auto createDeviceStatus = app->createDevice();
                    if (sample_app::StatusCode::SUCCESS != createDeviceStatus)
                    {
                        return app->reportError(appType + " Device Creation failure");
                    }
                }
                if (sample_app::StatusCode::SUCCESS != app->initializeProfiling())
                {
                    return app->reportError(appType + " Profiling Initialization failure");
                }
                if (sample_app::StatusCode::SUCCESS != app->registerOpPackages())
                {
                    return app->reportError(appType + " Register Op Packages failure");
                }
                if (!loadFromCachedBinary)
                {
                    if (sample_app::StatusCode::SUCCESS != app->createContext())
                    {
                        return app->reportError(appType + " Context Creation failure");
                    }
                    if (sample_app::StatusCode::SUCCESS != app->composeGraphs())
                    {
                        return app->reportError(appType + " Graph Prepare failure");
                    }
                    if (sample_app::StatusCode::SUCCESS != app->finalizeGraphs())
                    {
                        return app->reportError(appType + " Graph Finalize failure");
                    }
                }
                else
                {
                    if (sample_app::StatusCode::SUCCESS != app->createFromBinary())
                    {
                        return app->reportError(appType + " Create From Binary failure");
                    }
                }
                return EXIT_SUCCESS;
            }

            void showHelp()
            {
            }

            void showHelpAndExit(std::string &&error)
            {
                std::cerr << "ERROR: " << error << "\n";
                std::cerr << "Please check help below:\n";
                showHelp();
                std::exit(EXIT_FAILURE);
            }

            struct ModelApps
            {
                std::unique_ptr<QnnModel> clip;
                std::unique_ptr<QnnModel> unet;
                std::unique_ptr<QnnModel> vae_decoder;
                std::unique_ptr<QnnModel> vae_encoder;
                MNN::Interpreter *safety_checker_mnn;
            };

            ModelApps processCommandLine(int argc,
                                         char **argv,
                                         bool &loadFromCachedBinary,
                                         std::string &clipPath,
                                         std::string &unetPath,
                                         std::string &vaeEncoderPath,
                                         std::string &vaeDecoderPath,
                                         std::string &safetyCheckerPath,
                                         std::string &tokenizerPath)
            {
                {
                    enum OPTIONS
                    {
                        OPT_HELP = 0,
                        OPT_CLIP = 21,
                        OPT_UNET = 22,
                        OPT_VAE_DECODER = 23,
                        OPT_TEXT_EMBEDDING_SIZE = 24,
                        OPT_SAFETY_CHECKER = 27,
                        OPT_IMG2IMG = 29,
                        OPT_BACKEND = 3,
                        OPT_INPUT_LIST = 4,
                        OPT_OUTPUT_DIR = 5,
                        OPT_OP_PACKAGES = 6,
                        OPT_DEBUG_OUTPUTS = 7,
                        OPT_OUTPUT_DATA_TYPE = 8,
                        OPT_INPUT_DATA_TYPE = 9,
                        OPT_LOG_LEVEL = 10,
                        OPT_PROFILING_LEVEL = 11,
                        OPT_SAVE_CONTEXT = 12,
                        OPT_VERSION = 13,
                        OPT_SYSTEM_LIBRARY = 14,
                        OPT_PORT = 15,
                        OPT_TOKENIZER = 16
                    };

                    static struct pal::Option s_longOptions[] = {
                            {"help", pal::no_argument, NULL, OPT_HELP},
                            {"port", pal::required_argument, NULL, OPT_PORT},
                            {"text_embedding_size", pal::required_argument, NULL, OPT_TEXT_EMBEDDING_SIZE},
                            {"safety_checker", pal::required_argument, NULL, OPT_SAFETY_CHECKER},
                            {"vae_encoder", pal::required_argument, NULL, OPT_IMG2IMG},
                            {"tokenizer", pal::required_argument, NULL, OPT_TOKENIZER},
                            {"clip", pal::required_argument, NULL, OPT_CLIP},
                            {"unet", pal::required_argument, NULL, OPT_UNET},
                            {"vae_decoder", pal::required_argument, NULL, OPT_VAE_DECODER},
                            {"backend", pal::required_argument, NULL, OPT_BACKEND},
                            {"input_list", pal::required_argument, NULL, OPT_INPUT_LIST},
                            {"output_dir", pal::required_argument, NULL, OPT_OUTPUT_DIR},
                            {"op_packages", pal::required_argument, NULL, OPT_OP_PACKAGES},
                            {"debug", pal::no_argument, NULL, OPT_DEBUG_OUTPUTS},
                            {"output_data_type", pal::required_argument, NULL, OPT_OUTPUT_DATA_TYPE},
                            {"input_data_type", pal::required_argument, NULL, OPT_INPUT_DATA_TYPE},
                            {"profiling_level", pal::required_argument, NULL, OPT_PROFILING_LEVEL},
                            {"log_level", pal::required_argument, NULL, OPT_LOG_LEVEL},
                            {"save_context", pal::required_argument, NULL, OPT_SAVE_CONTEXT},
                            {"system_library", pal::required_argument, NULL, OPT_SYSTEM_LIBRARY},
                            {"version", pal::no_argument, NULL, OPT_VERSION},
                            {NULL, 0, NULL, 0}};

                    bool debug = false;
                    std::string backEndPath;
                    std::string inputListPaths;
                    std::string outputPath;
                    std::string opPackagePaths;
                    std::string saveBinaryName;
                    std::string systemLibraryPath;
                    iotensor::OutputDataType parsedOutputDataType = iotensor::OutputDataType::FLOAT_ONLY;
                    iotensor::InputDataType parsedInputDataType = iotensor::InputDataType::FLOAT;
                    sample_app::ProfilingLevel parsedProfilingLevel = ProfilingLevel::OFF;
                    QnnLog_Level_t logLevel = QNN_LOG_LEVEL_ERROR;

                    int longIndex = 0;
                    int opt = 0;
                    while ((opt = pal::getOptLongOnly(argc, argv, "", s_longOptions, &longIndex)) != -1)
                    {
                        switch (opt)
                        {
                            case OPT_HELP:
                                showHelp();
                                std::exit(EXIT_SUCCESS);
                                break;
                            case OPT_VERSION:
                                std::cout << "QNN SDK " << qnn::tools::getBuildId() << "\n";
                                std::exit(EXIT_SUCCESS);
                                break;
                            case OPT_CLIP:
                                clipPath = pal::g_optArg;
                                break;
                            case OPT_UNET:
                                unetPath = pal::g_optArg;
                                break;
                            case OPT_VAE_DECODER:
                                vaeDecoderPath = pal::g_optArg;
                                break;
                            case OPT_BACKEND:
                                backEndPath = pal::g_optArg;
                                break;
                            case OPT_INPUT_LIST:
                                inputListPaths = pal::g_optArg;
                                break;
                            case OPT_TEXT_EMBEDDING_SIZE:
                                text_embedding_size = std::stoi(pal::g_optArg);
                                break;
                            case OPT_SAFETY_CHECKER:
                                use_safety_checker = true;
                                safetyCheckerPath = pal::g_optArg;
                                break;
                            case OPT_IMG2IMG:
                                img2img = true;
                                vaeEncoderPath = pal::g_optArg;
                                break;
                            case OPT_DEBUG_OUTPUTS:
                                debug = true;
                                break;
                            case OPT_OUTPUT_DIR:
                                outputPath = pal::g_optArg;
                                break;
                            case OPT_OP_PACKAGES:
                                opPackagePaths = pal::g_optArg;
                                break;
                            case OPT_OUTPUT_DATA_TYPE:
                                parsedOutputDataType = iotensor::parseOutputDataType(pal::g_optArg);
                                if (parsedOutputDataType == iotensor::OutputDataType::INVALID)
                                {
                                    showHelpAndExit("Invalid output data type string.");
                                }
                                break;
                            case OPT_INPUT_DATA_TYPE:
                                parsedInputDataType = iotensor::parseInputDataType(pal::g_optArg);
                                if (parsedInputDataType == iotensor::InputDataType::INVALID)
                                {
                                    showHelpAndExit("Invalid input data type string.");
                                }
                                break;
                            case OPT_PROFILING_LEVEL:
                                parsedProfilingLevel = sample_app::parseProfilingLevel(pal::g_optArg);
                                if (parsedProfilingLevel == sample_app::ProfilingLevel::INVALID)
                                {
                                    showHelpAndExit("Invalid profiling level.");
                                }
                                break;
                            case OPT_LOG_LEVEL:
                                logLevel = sample_app::parseLogLevel(pal::g_optArg);
                                if (logLevel != QNN_LOG_LEVEL_MAX)
                                {
                                    if (!log::setLogLevel(logLevel))
                                    {
                                        showHelpAndExit("Unable to set log level.");
                                    }
                                }
                                break;
                            case OPT_SAVE_CONTEXT:
                                saveBinaryName = pal::g_optArg;
                                if (saveBinaryName.empty())
                                {
                                    showHelpAndExit("Save context needs a file name.");
                                }
                                break;
                            case OPT_SYSTEM_LIBRARY:
                                systemLibraryPath = pal::g_optArg;
                                break;
                            case OPT_PORT:
                                port = std::stoi(pal::g_optArg);
                                break;
                            case OPT_TOKENIZER:
                                tokenizerPath = pal::g_optArg;
                                break;
                            default:
                                showHelpAndExit("Invalid argument passed.");
                        }
                    }

                    if (clipPath.empty() || unetPath.empty() || vaeDecoderPath.empty())
                    {
                        showHelpAndExit("Missing required model paths: --clip, --unet, and/or --vae_decoder");
                    }
                    if (tokenizerPath.empty())
                    {
                        showHelpAndExit("Missing option: --tokenizer");
                    }
                    if (use_safety_checker && safetyCheckerPath.empty())
                    {
                        showHelpAndExit("Missing option: --safety_checker");
                    }

                    ModelApps apps;
                    if (use_safety_checker)
                    {
                        apps.safety_checker_mnn = MNN::Interpreter::createFromFile(safetyCheckerPath.c_str());
                    }

                    if (systemLibraryPath.empty())
                    {
                        showHelpAndExit("Requires system library path.");
                    }
                    if (backEndPath.empty())
                    {
                        showHelpAndExit("Missing option: --backend");
                    }

                    QnnFunctionPointers qnnFunctionPointers_clip, qnnFunctionPointers_unet, qnnFunctionPointers_vae_decoder, qnnFunctionPointers_vae_encoder;

                    // CLIP
                    auto status = dynamicloadutil::getQnnFunctionPointers(
                            backEndPath, clipPath, &qnnFunctionPointers_clip,
                            &sg_backendHandle_clip, false, &sg_modelHandle);
                    if (dynamicloadutil::StatusCode::SUCCESS != status)
                    {
                        showHelpAndExit("Failed to get CLIP QNN function pointers.");
                    }

                    // UNET
                    status = dynamicloadutil::getQnnFunctionPointers(
                            backEndPath, unetPath, &qnnFunctionPointers_unet,
                            &sg_backendHandle_unet, false, &sg_modelHandle);
                    if (dynamicloadutil::StatusCode::SUCCESS != status)
                    {
                        showHelpAndExit("Failed to get UNET QNN function pointers.");
                    }

                    // VAE Decoder
                    status = dynamicloadutil::getQnnFunctionPointers(
                            backEndPath, vaeDecoderPath, &qnnFunctionPointers_vae_decoder,
                            &sg_backendHandle_vae_decoder, false, &sg_modelHandle);
                    if (dynamicloadutil::StatusCode::SUCCESS != status)
                    {
                        showHelpAndExit("Failed to get VAE Decoder QNN function pointers.");
                    }

                    if (img2img)
                    {
                        // VAE Encoder
                        status = dynamicloadutil::getQnnFunctionPointers(
                                backEndPath, vaeEncoderPath, &qnnFunctionPointers_vae_encoder,
                                &sg_backendHandle_vae_encoder, false, &sg_modelHandle);
                        if (dynamicloadutil::StatusCode::SUCCESS != status)
                        {
                            showHelpAndExit("Failed to get VAE Encoder QNN function pointers.");
                        }
                    }

                    if (!systemLibraryPath.empty())
                    {
                        for (auto *functionPointers : {&qnnFunctionPointers_clip, &qnnFunctionPointers_unet, &qnnFunctionPointers_vae_decoder})
                        {
                            status = dynamicloadutil::getQnnSystemFunctionPointers(
                                    systemLibraryPath, functionPointers);
                            if (dynamicloadutil::StatusCode::SUCCESS != status)
                            {
                                showHelpAndExit("Failed to get QNN system function pointers.");
                            }
                        }
                        if (img2img)
                        {
                            status = dynamicloadutil::getQnnSystemFunctionPointers(
                                    systemLibraryPath, &qnnFunctionPointers_vae_encoder);
                            if (dynamicloadutil::StatusCode::SUCCESS != status)
                            {
                                showHelpAndExit("Failed to get VAE Encoder system function pointers.");
                            }
                        }
                    }

                    apps.clip = std::make_unique<QnnModel>(
                            qnnFunctionPointers_clip,
                            inputListPaths,
                            opPackagePaths,
                            sg_backendHandle_clip,
                            outputPath,
                            debug,
                            parsedOutputDataType,
                            parsedInputDataType,
                            parsedProfilingLevel,
                            true,
                            clipPath,
                            saveBinaryName);

                    apps.unet = std::make_unique<QnnModel>(
                            qnnFunctionPointers_unet,
                            inputListPaths,
                            opPackagePaths,
                            sg_backendHandle_unet,
                            outputPath,
                            debug,
                            parsedOutputDataType,
                            parsedInputDataType,
                            parsedProfilingLevel,
                            true,
                            unetPath,
                            saveBinaryName);

                    apps.vae_decoder = std::make_unique<QnnModel>(
                            qnnFunctionPointers_vae_decoder,
                            inputListPaths,
                            opPackagePaths,
                            sg_backendHandle_vae_decoder,
                            outputPath,
                            debug,
                            parsedOutputDataType,
                            parsedInputDataType,
                            parsedProfilingLevel,
                            true,
                            vaeDecoderPath,
                            saveBinaryName);

                    if (img2img)
                    {
                        apps.vae_encoder = std::make_unique<QnnModel>(
                                qnnFunctionPointers_vae_encoder,
                                inputListPaths,
                                opPackagePaths,
                                sg_backendHandle_vae_encoder,
                                outputPath,
                                debug,
                                parsedOutputDataType,
                                parsedInputDataType,
                                parsedProfilingLevel,
                                true,
                                vaeEncoderPath,
                                saveBinaryName);
                    }
                    return apps;
                }

            } // namespace sample_app
        } // namespace tools
    } // namespace qnn
}

std::vector<int> EncodeText(const std::string &text, int bos, int pad, int max_length)
{
    int sd21_pad = 0;
    std::vector<int> ids = g_tokenizer->Encode(text);
    ids.insert(ids.begin(), bos);
    if (ids.size() > max_length - 1)
    {
        ids.resize(max_length - 1);
    }
    int pad_length = max_length - ids.size();
    ids.push_back(pad);
    for (int i = 0; i < pad_length - 1; i++)
    {
        if (text_embedding_size == 1024)
        {
            ids.push_back(sd21_pad);
        }
        else
        {
            ids.push_back(pad);
        }
    }

    return ids;
}

std::vector<int> processPrompt(
        const std::string &prompt,
        const std::string &negative_prompt = "",
        const int max_length = 77, bool use_cfg = true)
{
    std::vector<int> prompt_ids = EncodeText(prompt, 49406, 49407, max_length);
    std::vector<int> negative_prompt_ids = EncodeText(negative_prompt, 49406, 49407, max_length);
    std::vector<int> ids;
    int batch_size = 1;
    if (use_cfg)
    {
        batch_size = 2;
    }
    ids.reserve(batch_size * max_length);
    if (use_cfg)
    {
        ids.insert(ids.end(), negative_prompt_ids.begin(), negative_prompt_ids.end());
    }
    ids.insert(ids.end(), prompt_ids.begin(), prompt_ids.end());
    return ids;
}

GenerationResult generateImage(
        const std::string &prompt,
        const std::string &negative_prompt,
        int steps,
        float cfg,
        bool use_cfg,
        unsigned seed,
        std::vector<float> img_data,
        float denoise_strength,
        QnnModel *clipApp,
        QnnModel *unetApp,
        QnnModel *vaeDecoderApp,
        QnnModel *vaeEncoderApp,
        MNN::Interpreter *safetyCheckerInterpreter,
        std::function<void(int step, int total_steps)> progress_callback)
{
    using namespace qnn::tools::sample_app;
    if (use_safety_checker && safetyCheckerInterpreter == nullptr)
    {
        throw std::runtime_error("Safety Checker model not initialized");
    }
    if (!clipApp || !unetApp || !vaeDecoderApp)
    {
        throw std::runtime_error("Models not initialized");
    }
    if (img2img && !vaeEncoderApp)
    {
        throw std::runtime_error("VAE Encoder model not initialized");
    }
    if (prompt.empty())
    {
        throw std::invalid_argument("Input prompt cannot be empty");
    }
    try
    {
        auto start_time = std::chrono::high_resolution_clock::now();
        int first_step_time_ms = 0;
        int total_run_steps = steps + 2;

        int current_step = 0;

        ClipInput clip_input;
        UnetInput unet_input;
        int batch_size = 1;
        if (use_cfg)
        {
            batch_size = 2;
        }
        unet_input.latents.resize(batch_size * 4 * sample_size * sample_size);
        unet_input.text_embedding.resize(batch_size * 77 * text_embedding_size);
        unet_input.text_embedding_float.resize(batch_size * 77 * text_embedding_size);

        clip_input.input_ids = processPrompt(prompt, negative_prompt, 77, use_cfg);

        if (StatusCode::SUCCESS != clipApp->executeClipGraphs(clip_input, unet_input, use_cfg))
        {
            throw std::runtime_error("CLIP execution failed");
        }
        current_step++;
        if (progress_callback)
        {
            progress_callback(current_step, total_run_steps);
        }

        UnetOutput unet_output;
        unet_output.latents.resize(batch_size * 4 * sample_size * sample_size);
        VaeDecoderInput vae_decoder_input;
        vae_decoder_input.latents.resize(1 * 4 * sample_size * sample_size);
        Picture vae_decoder_output;
        vae_decoder_output.pixel_values.resize(1 * 3 * output_size * output_size);
        vae_decoder_output.pixels.resize(1 * 3 * output_size * output_size);

        DPMSolverMultistepScheduler scheduler(1000, 0.00085f, 0.012f, "scaled_linear", 2, "epsilon", "leading");
        scheduler.set_timesteps(steps);

        Qnn_Tensor_t *inputs = nullptr;
        Qnn_Tensor_t *outputs = nullptr;
        xt::xarray<float> timesteps = scheduler.get_timesteps();
        std::cout << timesteps << std::endl;
        auto shape2 = std::vector<int>{2, 4, sample_size, sample_size};
        auto shape = std::vector<int>{1, 4, sample_size, sample_size};

        xt::random::seed(seed);
        xt::xarray<float> latents = xt::random::randn<float>(shape);

        int start_step = 0;
        if (img2img && img_data.size() == 3 * output_size * output_size)
        {
            Picture vae_encoder_input;
            vae_encoder_input.pixel_values.resize(1 * 3 * output_size * output_size);
            memcpy(vae_encoder_input.pixel_values.data(), img_data.data(), 3 * output_size * output_size * sizeof(float));
            VaeEncoderOutput vae_encoder_output;
            vae_encoder_output.mean.resize(1 * 4 * sample_size * sample_size);
            vae_encoder_output.std.resize(1 * 4 * sample_size * sample_size);
            auto start = std::chrono::high_resolution_clock::now();
            if (StatusCode::SUCCESS != vaeEncoderApp->executeVaeEncoderGraphs(vae_encoder_input, vae_encoder_output))
            {
                throw std::runtime_error("VAE encoder execution failed");
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "VAE encoder runSession duration: " << duration.count() << "ms" << std::endl;
            auto mean = xt::adapt(vae_encoder_output.mean, {1, 4, sample_size, sample_size});
            auto std = xt::adapt(vae_encoder_output.std, {1, 4, sample_size, sample_size});
            xt::xarray<float> noise_0 = xt::random::randn<float>(shape);
            xt::xarray<float> img_latent_xt = xt::eval(mean + std * noise_0);
            xt::xarray<float> img_latent_scaled = xt::eval(0.18215 * img_latent_xt);

            start_step = (int)(steps * (1 - denoise_strength));

            total_run_steps -= start_step;
            scheduler.set_begin_index(start_step);
            std::vector<int> t = {(int)(timesteps[start_step])};
            xt::xarray<int> x_xt = xt::adapt(t, {1});
            latents = xt::random::randn<float>(shape);
            latents = scheduler.add_noise(img_latent_scaled, latents, x_xt);
        }

        for (int i = start_step; i < timesteps.size(); i++)
        {
            auto start = std::chrono::high_resolution_clock::now();
            xt::xarray<float> latents_input = xt::concatenate(xt::xtuple(latents, latents));
            unet_input.latents = std::vector<float>(latents_input.begin(), latents_input.end());
            unet_input.timestep = timesteps[i];

            if (i == start_step)
            {
                auto step_start = std::chrono::high_resolution_clock::now();
                if (StatusCode::SUCCESS != unetApp->executeUnetGraphsFirst(unet_input, unet_output, inputs, outputs, use_cfg))
                {
                    throw std::runtime_error("UNET first step execution failed");
                }
                auto step_end = std::chrono::high_resolution_clock::now();
                first_step_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start).count();
            }
            else
            {
                if (StatusCode::SUCCESS != unetApp->executeUnetGraphsRemain(unet_input, unet_output, inputs, outputs, use_cfg))
                {
                    throw std::runtime_error("UNET step execution failed");
                }
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "UNET runSession duration: " << duration.count() << "ms" << std::endl;

            xt::xarray<float> noise_pred;
            if (use_cfg)
            {
                noise_pred = xt::adapt(unet_output.latents, shape2);
                xt::xarray<float> noise_pred_uncond = xt::view(noise_pred, 0);
                xt::xarray<float> noise_pred_text = xt::view(noise_pred, 1);
                noise_pred = noise_pred_uncond + cfg * (noise_pred_text - noise_pred_uncond);
                noise_pred = xt::eval(noise_pred);
            }
            else
            {
                noise_pred = xt::adapt(unet_output.latents, shape);
                noise_pred = xt::eval(noise_pred);
            }

            latents = scheduler.step(noise_pred, timesteps[i], latents).prev_sample;
            auto end2 = std::chrono::high_resolution_clock::now();
            auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - end).count();
            std::cout << "Scheduler step duration: " << duration2 << "ms" << std::endl;

            current_step++;
            if (progress_callback)
            {
                progress_callback(current_step, total_run_steps);
            }
            auto end3 = std::chrono::high_resolution_clock::now();
            auto duration3 = std::chrono::duration_cast<std::chrono::milliseconds>(end3 - end2).count();
            std::cout << "callback duration: " << duration3 << "ms" << std::endl;
        }

        latents = xt::eval((1 / 0.18215) * latents);
        vae_decoder_input.latents = std::vector<float>(latents.begin(), latents.end());

        if (StatusCode::SUCCESS != vaeDecoderApp->executeVaeDecoderGraphs(vae_decoder_input, vae_decoder_output))
        {
            throw std::runtime_error("VAE decoder execution failed");
        }

        auto pixel_values = xt::adapt(vae_decoder_output.pixel_values, {1, 3, output_size, output_size});
        auto image = xt::view(pixel_values, 0);
        auto transposed = xt::transpose(image, {1, 2, 0});
        auto normalized = xt::clip(((transposed + 1.0) / 2.0) * 255.0, 0.0, 255.0);
        xt::xarray<uint8_t> uint8_image = xt::cast<uint8_t>(normalized);

        std::vector<uint8_t> output_data(uint8_image.begin(), uint8_image.end());

        unetApp->cleanUnetGraphs(inputs, outputs);

        if (progress_callback)
        {
            progress_callback(total_run_steps, total_run_steps);
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        if (use_safety_checker)
        {
            float nsfw_score = 0.0f;
            if (safety_check(output_data, output_size, output_size, nsfw_score, safetyCheckerInterpreter, safetyCheckerSession))
            {
                if (nsfw_score > nsfw_threshold)
                {
                    std::fill(output_data.begin(), output_data.end(), 255);
                }
            }
        }

        return GenerationResult{
                output_data,
                output_size, // width
                output_size, // height
                3,           // channels
                static_cast<int>(total_time),
                first_step_time_ms,
        };
    }
    catch (const std::exception &e)
    {
        QNN_ERROR("Image generation error: %s", e.what());
        throw;
    }
}

int main(int argc, char **argv)
{
    using namespace qnn::tools;

    if (!qnn::log::initializeLogging())
    {
        std::cerr << "ERROR: Unable to initialize logging!\n";
        return EXIT_FAILURE;
    }

    bool loadFromCachedBinary{true};
    std::string clipPath, unetPath, vaeDecoderPath, vaeEncoderPath, safetyCheckerPath;
    std::string tokenizerPath;

    auto res = sample_app::processCommandLine(argc, argv, loadFromCachedBinary, clipPath, unetPath, vaeEncoderPath, vaeDecoderPath, safetyCheckerPath, tokenizerPath);

    try
    {
        auto blob = LoadBytesFromFile(tokenizerPath);
        g_tokenizer = tokenizers::Tokenizer::FromBlobJSON(blob);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to load tokenizer or vocabulary: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    auto safetyCheckerApp = res.safety_checker_mnn;
    if (!safetyCheckerApp && use_safety_checker)
    {
        std::cerr << "Failed to load Safety Checker MNN model" << std::endl;
        return EXIT_FAILURE;
    }

    MNN::ScheduleConfig config_1;
    config_1.type = MNN_FORWARD_CPU;
    config_1.numThread = 4;
    MNN::BackendConfig backendConfig;
    backendConfig.memory = MNN::BackendConfig::Memory_Low;
    backendConfig.power = MNN::BackendConfig::Power_High;
    config_1.backendConfig = &backendConfig;

    MNN::ScheduleConfig config_2;
    config_2.type = MNN_FORWARD_CPU;
    config_2.numThread = 1;
    config_2.backendConfig = &backendConfig;

    if (use_safety_checker)
    {
        safetyCheckerSession = safetyCheckerApp->createSession(config_2);
    }

    auto clipApp = std::move(res.clip);
    auto unetApp = std::move(res.unet);
    auto vaeDecoderApp = std::move(res.vae_decoder);
    auto vaeEncoderApp = std::move(res.vae_encoder);

    auto status = initializeQnnApp(clipPath, clipApp, loadFromCachedBinary, "Clip");
    if (status != EXIT_SUCCESS)
    {
        return status;
    }

    status = initializeQnnApp(unetPath, unetApp, loadFromCachedBinary, "Unet");
    if (status != EXIT_SUCCESS)
    {
        return status;
    }

    status = initializeQnnApp(vaeDecoderPath, vaeDecoderApp, loadFromCachedBinary, "VaeDecoder");
    if (status != EXIT_SUCCESS)
    {
        return status;
    }

    if (img2img)
    {
        status = initializeQnnApp(vaeEncoderPath, vaeEncoderApp, loadFromCachedBinary, "VaeEncoder");
        if (status != EXIT_SUCCESS)
        {
            return status;
        }
    }

    httplib::Server svr;

    svr.Get("/health", [](const httplib::Request &req, httplib::Response &res)
    { res.status = 200; });

    svr.Post("/generate", [&clipApp, &unetApp, &vaeDecoderApp, &vaeEncoderApp, &safetyCheckerApp](const httplib::Request &req, httplib::Response &res)
    {
        try {
            auto json = nlohmann::json::parse(req.body);
            if (!json.contains("prompt")) {
                throw std::invalid_argument("Missing required field: 'prompt'");
            }
            int steps = 20;
            if (json.contains("steps")) {
                steps = json["steps"].get<int>();
            }
            float cfg = 7.5;
            if (json.contains("cfg")) {
                cfg = json["cfg"].get<float>();
            }
            std::string negative_prompt = "";
            if (json.contains("negative_prompt")) {
                negative_prompt = json["negative_prompt"].get<std::string>();
            }
            bool use_cfg = false;
            if (json.contains("use_cfg")) {
                use_cfg = json["use_cfg"].get<bool>();
            }
            int size = 512;
            if (json.contains("size")) {
                size = json["size"].get<int>();
                output_size = size;
                sample_size = size / 8;
            }
            img2img = false;
            std::vector<float> img_float_data;
            if (json.contains("image")) {
                img2img = true;
                auto image = json["image"].get<std::string>();
                auto decoded = base64_decode(image);
                auto decoded_buffer = std::vector<uint8_t>(decoded.begin(), decoded.end());
                std::vector<uint8_t> decoded_image;
                decode_image(decoded_buffer, decoded_image, output_size);
                if (decoded_image.size() != 3 * output_size * output_size)
                {
                    img_float_data.clear();
                }
                else
                {
                    xt::xarray<uint8_t> img_xt = xt::adapt(decoded_image, {1, output_size, output_size, 3});
                    xt::xarray<float> img_data = xt::cast<float>(img_xt);
                    img_data = xt::eval(img_data / 255.0);
                    img_data = xt::transpose(img_data, {0, 3, 1, 2});
                    img_data = xt::eval(img_data * 2.0 - 1.0);
                    img_float_data = std::vector<float>(img_data.begin(), img_data.end());
                }
            }
            float denoise_strength = 0.6;
            if (json.contains("denoise_strength")) {
                denoise_strength = json["denoise_strength"].get<float>();
            }
            unsigned seed = hashSeed(std::chrono::system_clock::now().time_since_epoch().count());
            if (json.contains("seed")) {
                seed = json["seed"].get<unsigned>();
            }
            std::cout<<"prompt: "<<json["prompt"].get<std::string>()<<std::endl;
            std::cout<<"negative_prompt: "<<negative_prompt<<std::endl;
            std::cout<<"steps: "<<steps<<std::endl;
            std::cout<<"cfg: "<<cfg<<std::endl;
            std::cout<<"use_cfg: "<<use_cfg<<std::endl;
            std::cout<<"seed: "<<seed<<std::endl;
            std::cout<<"size: "<<size<<std::endl;
            std::cout<<"denoise_strength: "<<denoise_strength<<std::endl;

            std::string prompt = json["prompt"].get<std::string>();

            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            res.set_chunked_content_provider(
                    "text/event-stream",
                    [prompt, negative_prompt, steps, cfg, use_cfg, seed, img_float_data, denoise_strength, &clipApp, &unetApp, &vaeDecoderApp, &vaeEncoderApp, &safetyCheckerApp]
                            (size_t, httplib::DataSink& sink) -> bool {
                        try {
                            auto result = generateImage(
                                    prompt,
                                    negative_prompt,
                                    steps,
                                    cfg,
                                    use_cfg,
                                    seed,
                                    img_float_data,
                                    denoise_strength,
                                    clipApp.get(),
                                    unetApp.get(),
                                    vaeDecoderApp.get(),
                                    vaeEncoderApp.get(),
                                    safetyCheckerApp,
                                    [&sink](int step, int total_steps) {
                                        nlohmann::json progress = {
                                                {"type", "progress"},
                                                {"step", step},
                                                {"total_steps", total_steps}
                                        };
                                        std::string event = "data: " + progress.dump() + "\n\n";
                                        sink.write(event.c_str(), event.size());
                                    });

                            auto encode_start = std::chrono::high_resolution_clock::now();

                            std::string encoded = base64_encode(
                                    std::string(result.image_data.begin(), result.image_data.end())
                            );

                            auto encode_end = std::chrono::high_resolution_clock::now();
                            auto encode_time = std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start).count();

                            std::cout << "Encoding time: " << encode_time << " ms" << std::endl;

                            auto send_start = std::chrono::high_resolution_clock::now();

                            nlohmann::json complete = {
                                    {"type", "complete"},
                                    {"image", encoded},
                                    {"seed", seed},
                                    {"width", result.width},
                                    {"height", result.height},
                                    {"channels", result.channels},
                                    {"generation_time_ms", result.generation_time_ms},
                                    {"first_step_time_ms", result.first_step_time_ms},
                            };

                            std::string event = "data: " + complete.dump() + "\n\n";
                            sink.write(event.c_str(), event.size());
                            sink.write("data: [DONE]\n\n", 15);
                            auto send_end = std::chrono::high_resolution_clock::now();
                            auto send_time = std::chrono::duration_cast<std::chrono::milliseconds>(send_end - send_start).count();
                            std::cout << "Sending time: " << send_time << " ms" << std::endl;

                            return false;

                        } catch (const std::exception& e) {
                            nlohmann::json error = {
                                    {"type", "error"},
                                    {"message", e.what()}
                            };
                            std::string event = "data: " + error.dump() + "\n\n";
                            sink.write(event.c_str(), event.size());
                            sink.write("data: [DONE]\n\n", 15);
                            return false;
                        }
                    });

        } catch (const std::exception& e) {
            nlohmann::json error = {
                    {"error", {
                            {"message", e.what()},
                            {"type", "server_error"}
                    }}
            };
            res.status = 400;
            res.set_content(error.dump(), "application/json");
        } });

    svr.listen(listen_address, port);

    if (sg_backendHandle_clip)
    {
        pal::dynamicloading::dlClose(sg_backendHandle_clip);
    }
    if (sg_backendHandle_unet)
    {
        pal::dynamicloading::dlClose(sg_backendHandle_unet);
    }
    if (sg_backendHandle_vae_decoder)
    {
        pal::dynamicloading::dlClose(sg_backendHandle_vae_decoder);
    }
    if (sg_modelHandle)
    {
        pal::dynamicloading::dlClose(sg_modelHandle);
    }
    return EXIT_SUCCESS;
}