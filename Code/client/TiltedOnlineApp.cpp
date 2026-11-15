#include <TiltedOnlinePCH.h>

#include <TiltedOnlineApp.h>

#include <DInputHook.hpp>
#include <dinput.h>
#include <WindowsHook.hpp>

#include <World.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/OverlayService.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>

#include <ScriptExtender.h>
#include <NvidiaUtil.h>
#include <RfabDiag.h>
#include <common/RfabModpackCompat.h>

using TiltedPhoques::Debug;

namespace
{
std::string ToUtf8(const std::filesystem::path& acPath)
{
    const std::wstring& wide = acPath.native();
    if (wide.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> CreateClientLogSink(const std::filesystem::path& acLogPath, std::filesystem::path& aOutFile)
{
    std::string filename = "tp_client.log";
    for (uint32_t instance = 1;; ++instance)
    {
        const auto mutexName = fmt::format("SkyrimTogether.ClientLog.Instance_{}", instance);
        HANDLE pMutex = CreateMutexA(nullptr, FALSE, mutexName.c_str());
        const DWORD error = GetLastError();
        if (!pMutex)
        {
            filename = fmt::format("tp_client_pid_{}.log", GetCurrentProcessId());
            break;
        }
        if (error == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(pMutex);
            continue;
        }
        if (instance > 1)
            filename = fmt::format("tp_client_instance_{}.log", instance);
        break;
    }
    aOutFile = acLogPath / filename;
    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(aOutFile, 1048576 * 10, 5);
}
} // namespace

extern "C" const wchar_t* GetTiltedClientDirW() noexcept;
extern void WriteBootTrace(const char* apMsg, bool aReset);

TiltedOnlineApp::TiltedOnlineApp()
{
    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);

    // <game>\RFAB_Online_Logs (RfabDiag.h): the game folder is real also under Mod Organizer 2, where
    // a file created in the client folder can land in MO2's overwrite folder. Playtest 2026-11-15:
    // both reports came without any client log for exactly that reason.
    const wchar_t* pClientDir = GetTiltedClientDirW();
    std::error_code tempError;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(tempError);
    const std::filesystem::path candidates[] = {
        RfabDiag::GetLogDirectory(),
        (pClientDir && *pClientDir) ? (std::filesystem::path(pClientDir) / "logs") : (TiltedPhoques::GetPath() / "logs"),
        tempError ? std::filesystem::path{} : (tempDir / "RFAB_Online_Logs"),
    };

    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> rotatingLogger;
    std::filesystem::path usedFile;
    std::string failures;
    for (const auto& candidate : candidates)
    {
        if (candidate.empty())
            continue;
        try
        {
            std::error_code ec;
            create_directories(candidate, ec);
            rotatingLogger = CreateClientLogSink(candidate, usedFile);
            break;
        }
        catch (const std::exception& acError)
        {
            // A log that cannot be opened must not stop the game from starting; say so instead.
            failures += fmt::format(" [{}: {}]", ToUtf8(candidate), acError.what());
        }
    }

    // rotatingLogger->set_level(spdlog::level::debug);
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{console};
    if (rotatingLogger)
        sinks.push_back(rotatingLogger);
    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
    logger->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [tid %t] %$ %v");
    // Warnings are rare and are what a report is read for: they go to disk at once.
    logger->flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
    set_default_logger(logger);

    // The startup trace names the file, so a report can be checked against it.
    char where[1536];
    if (rotatingLogger)
        std::snprintf(where, sizeof(where), "[BOOT] Client log: %s", ToUtf8(usedFile).c_str());
    else
        std::snprintf(where, sizeof(where), "[BOOT] WARNING: NO CLIENT LOG could be opened:%s", failures.c_str());
    WriteBootTrace(where, false);

    spdlog::info("RFAB Online client {} (build {}), pid {}, log file {}", RfabCompat::kRfabOnlineVersion, BUILD_COMMIT, GetCurrentProcessId(),
                 rotatingLogger ? ToUtf8(usedFile) : std::string("NONE"));
    if (!failures.empty())
        spdlog::warn("Client log folder(s) that could not be used:{}", failures);
}

TiltedOnlineApp::~TiltedOnlineApp() = default;

void* TiltedOnlineApp::GetMainAddress() const
{
    POINTER_SKYRIMSE(void, winMain, 35545);

    return winMain.GetPtr();
}

bool TiltedOnlineApp::BeginMain()
{
    World::Create();
    World::Get().ctx().at<DiscordService>().Init();
    World::Get().ctx().emplace<RenderSystemD3D11>(World::Get().ctx().at<OverlayService>(), World::Get().ctx().at<ImguiService>());

    LoadScriptExender();

    // TODO: Figure out a way to un-blacklist NvCamera64.dll (see DllBlocklist.cpp). Then this hack can be removed
    if (IsNvidiaOverlayLoaded())
        ApplyNvidiaFix();

    return true;
}

bool TiltedOnlineApp::EndMain()
{
    UninstallHooks();
    if (m_pDevice)
        m_pDevice->Release();

    return true;
}

void TiltedOnlineApp::Update()
{
    // Reverting a change that used to be here to disable bUseFaceGenPreprocessedHeads==true (which is 
    // the default) handling. Extensive testing over months by multiple parties showed that enabling 
    // the flag introduces no issues WITH PROPERLY GENERATED CHARACTERS (in-game character generation 
    // or showracemenu). The shortcut of  "coc riverwood" from the main menu skips proper character generation.
    // 
    // Plus, having it on  has some benefits like helping with neck seams. Comment to avoid revisiting.
    // 
    // There are still some issues to track down, like hair color and maybe face tint not syncing correctly,
    // but they are unrelated and unchanged by this flag.
    // 
 
    // Make sure the window stays active
    POINTER_SKYRIMSE(uint32_t, bAlwaysActive, 508799);

    *bAlwaysActive = 1;

    World::Get().Update();
}

bool TiltedOnlineApp::Attach()
{
    TiltedPhoques::Debug::OnAttach();

    // TiltedPhoques::Nop(0x1405D3FA1, 6);
    return true;
}

bool TiltedOnlineApp::Detach()
{
    TiltedPhoques::Debug::OnDetach();
    return true;
}

void TiltedOnlineApp::InstallHooks2()
{
    TiltedPhoques::Initializer::RunAll();

    TiltedPhoques::DInputHook::Install();
    TiltedPhoques::DInputHook::Get().SetToggleKeys({DIK_F2, DIK_RCONTROL});
}

void TiltedOnlineApp::UninstallHooks()
{
}

void TiltedOnlineApp::ApplyNvidiaFix() noexcept
{
    auto d3dFeatureLevelOut = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = CreateEarlyDxDevice(&m_pDevice, &d3dFeatureLevelOut);
    if (FAILED(hr))
        spdlog::error("D3D11CreateDevice failed. Detected an NVIDIA GPU, error code={0:x}", hr);

    if (d3dFeatureLevelOut < D3D_FEATURE_LEVEL_11_0)
        spdlog::warn("Unexpected D3D11 feature level detected (< 11.0), may cause issues");
}
