//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "GlobalMedia_win.h"

#include <Functiondiscoverykeys_devpkey.h>

#include "../Utils.h"
#include "../Logger.h"

namespace Core::GlobalMedia {

using namespace std::chrono_literals;
using namespace winrt::Windows::Media::Control;

namespace Details {

class MediaProgramThroughVirtualKeyAbstract : public MediaProgramAbstract
{
public:
    enum WindowMatchingFlag : uint32_t {
        HasChildren = 0b00000001,
        NonEmptyTitle = 0b00000010,
    };
    Q_DECLARE_FLAGS(WindowMatchingFlags, WindowMatchingFlag)

    virtual inline ~MediaProgramThroughVirtualKeyAbstract() {}

    bool IsAvailable() override
    {
        if (_windowProcess.has_value()) {
            return true;
        }

        _windowProcess = FindWindowAndProcess();
        if (!_windowProcess.has_value()) {
            return false;
        }

        _audioMeterInfo = GetProcessAudioMeterInfo();
        return true;
    }

    bool IsPlaying() const override
    {
        auto optAudioVolume = GetProcessAudioVolume();
        return optAudioVolume.has_value() && optAudioVolume.value() != 0.f;
    }

    bool Play() override
    {
        LOG(Trace, "Do play.");

        if (IsPlaying()) {
            LOG(Trace, "The media program is already playing.");
            return true;
        }
        return Switch();
    }

    bool Pause() override
    {
        LOG(Trace, "Do pause.");

        if (!IsPlaying()) {
            LOG(Trace, "The media program nothing is playing.");
            return true;
        }
        return Switch();
    }

protected:
    virtual std::wstring GetProcessName() const = 0;
    virtual std::wstring GetWindowClassName() const = 0;
    virtual std::optional<std::wstring> GetWindowTitleName() const
    {
        return std::nullopt;
    }
    virtual WindowMatchingFlags GetWindowMatchingFlags() const = 0;

private:
    std::optional<std::pair<HWND, uint32_t>> _windowProcess;
    OS::Windows::Com::UniquePtr<IAudioMeterInformation> _audioMeterInfo;

    OS::Windows::Com::UniquePtr<IAudioMeterInformation> GetProcessAudioMeterInfo()
    {
        LOG(Trace, "Try to get IAudioMeterInformation of this process.");

        OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> deviceEnumerator;
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, deviceEnumerator.GetIID(),
            (void **)deviceEnumerator.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Trace, "Create COM instance 'IMMDeviceEnumerator' failed. HRESULT: {:#x}", result);
            return {};
        }

        OS::Windows::Com::UniquePtr<IMMDevice> audioEndpoint;
        result = deviceEnumerator->GetDefaultAudioEndpoint(
            eRender, eMultimedia, audioEndpoint.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Trace, "'GetDefaultAudioEndpoint' failed. HRESULT: {:#x}", result);
            return {};
        }

        OS::Windows::Com::UniquePtr<IAudioSessionManager2> sessionMgr;
        result = audioEndpoint->Activate(
            sessionMgr.GetIID(), CLSCTX_ALL, nullptr, (void **)sessionMgr.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Trace, "'IMMDevice::Activate' IAudioSessionManager2 failed. HRESULT: {:#x}",
                result);
            return {};
        }

        OS::Windows::Com::UniquePtr<IAudioSessionEnumerator> sessionEnumerator;
        result = sessionMgr->GetSessionEnumerator(sessionEnumerator.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Trace, "'IAudioSessionManager2::GetSessionEnumerator' failed. HRESULT: {:#x}",
                result);
            return {};
        }

        int sessionCount = 0;
        result = sessionEnumerator->GetCount(&sessionCount);
        if (FAILED(result)) {
            LOG(Trace, "'IAudioSessionEnumerator::GetCount' failed. HRESULT: {:#x}", result);
            return {};
        }

        for (int i = 0; i < sessionCount; ++i) {
            LOG(Trace, "Enumerating the {}th session.", i);

            OS::Windows::Com::UniquePtr<IAudioSessionControl> session;
            result = sessionEnumerator->GetSession(i, session.ReleaseAndAddressOf());
            if (FAILED(result)) {
                LOG(Trace, "'IAudioSessionEnumerator::GetSession' failed. HRESULT: {:#x}", result);
                continue;
            }

            OS::Windows::Com::UniquePtr<IAudioSessionControl2> sessionEx;
            result = session->QueryInterface(
                sessionEx.GetIID(), (void **)sessionEx.ReleaseAndAddressOf());
            if (FAILED(result)) {
                LOG(Trace,
                    "'IAudioSessionControl::QueryInterface' IAudioSessionControl2 failed. "
                    "HRESULT: {:#x}",
                    result);
                continue;
            }

            uint32_t thisProcessId = 0;
            result = sessionEx->GetProcessId((DWORD *)&thisProcessId);
            if (FAILED(result)) {
                LOG(Trace, "'IAudioSessionControl2::GetProcessId' failed. HRESULT: {:#x}", result);
                continue;
            }

            LOG(Trace, "Get the session process id: {}", thisProcessId);

            if (_windowProcess->second != thisProcessId) {
                LOG(Trace, "Process id mismatch, try next.");
                continue;
            }

            OS::Windows::Com::UniquePtr<IAudioMeterInformation> audioMeterInfo;
            result = sessionEx->QueryInterface(
                audioMeterInfo.GetIID(), (void **)audioMeterInfo.ReleaseAndAddressOf());
            if (FAILED(result)) {
                LOG(Trace,
                    "'IAudioSessionControl2::QueryInterface' IAudioMeterInformation "
                    "failed. HRESULT: {:#x}",
                    result);
                return {};
            }

            LOG(Trace, "Get IAudioMeterInformation successfully.");
            return audioMeterInfo;
        }

        LOG(Trace, "The desired IAudioMeterInformation not found.");
        return {};
    }

    std::optional<float> GetProcessAudioVolume() const
    {
        if (!_audioMeterInfo) {
            return std::nullopt;
        }

        float peak = 0.f;
        HRESULT result = _audioMeterInfo->GetPeakValue(&peak);
        if (FAILED(result)) {
            LOG(Trace, "'IAudioMeterInformation::GetPeakValue' failed. HRESULT: {:#x}", result);
            return std::nullopt;
        }

        return peak;
    }

    std::optional<std::pair<HWND, uint32_t>> FindWindowAndProcess() const
    {
        auto processName = GetProcessName();
        auto className = GetWindowClassName();
        auto optTitleName = GetWindowTitleName();

        LOG(Trace, L"Try to find the media window. Process name: '{}', Class name: '{}'",
            processName, className);

        auto windowsInfo = OS::Windows::Window::FindWindowsInfo(className, optTitleName);
        if (windowsInfo.empty()) {
            LOG(Trace, L"No matching media windows found.");
            return std::nullopt;
        }

        WindowMatchingFlags windowMatchingFlags = GetWindowMatchingFlags();

        for (const auto &info : windowsInfo) {
            // LOG(Trace,
            //     L"Enumerating window. hwnd: {}, Process id: {}, Process name: {}",
            //     (void*)info.hwnd,
            //     info.processId,
            //     info.processName
            // );

            if (Utils::Text::ToLower(info.processName) != Utils::Text::ToLower(processName)) {
                // LOG(Trace, L"The media window process name mismatch.");
                continue;
            }

            if (windowMatchingFlags.testFlag(WindowMatchingFlag::HasChildren)) {
                if (GetWindow(info.hwnd, GW_CHILD) == 0) {
                    // LOG(Trace, L"The window doesn't have any child windows.");
                    continue;
                }
            }

            if (windowMatchingFlags.testFlag(WindowMatchingFlag::NonEmptyTitle)) {
                if (info.titleName.empty()) {
                    // LOG(Trace, L"The window doesn't have title name.");
                    continue;
                }
            }

            LOG(Trace,
                L"The media window is matched. Return the information. hwnd: {}, Process "
                L"id: {}, Process name: {}",
                (void *)info.hwnd, info.processId, info.processName);

            return std::make_pair(info.hwnd, info.processId);
        }

        LOG(Trace, L"The desired media window not found.");
        return std::nullopt;
    }

    bool Switch()
    {
        bool postDown = PostMessageW(_windowProcess->first, WM_KEYDOWN, VK_SPACE, 0) != 0;

        std::this_thread::sleep_for(50ms);

        bool postUp = PostMessageW(_windowProcess->first, WM_KEYUP, VK_SPACE, 0) != 0;

        if (!postDown || !postUp) {
            LOG(Trace, "Switch failed. Post messages failed: hwnd '{}' down '{}' up '{}'",
                (void *)_windowProcess->first, postDown, postUp);
            return false;
        }
        return true;
    }
};

Q_DECLARE_OPERATORS_FOR_FLAGS(MediaProgramThroughVirtualKeyAbstract::WindowMatchingFlags)

//////////////////////////////////////////////////
// Media programs
//

class UniversalSystemSession final : public MediaProgramAbstract
{
public:
    UniversalSystemSession() = default;

    bool IsAvailable() override
    {
        try {
            _currentSession = GetCurrentSession();
        }
        catch (const OS::Windows::Winrt::Exception &ex) {
            LOG(Warn, "UniversalSystemSession get current session failed. Code: {:#x}, Message: {}",
                ex.code(), winrt::to_string(ex.message()));
            return false;
        }

        if (!_currentSession.value()) {
            _currentSession.reset();
            LOG(Trace, "UniversalSystemSession current session is unavailable.");
            return false;
        }

        return true;
    }

    bool IsPlaying() const override
    {
        return IsPlaying(_currentSession.value());
    }

    bool Play() override
    {
        try {
            _currentSession->TryPlayAsync().get();
            return true;
        }
        catch (const OS::Windows::Winrt::Exception &ex) {
            LOG(Warn, "_currentSession->TryPlayAsync() failed. {}", Helper::ToString(ex));
            return false;
        }
    }

    bool Pause() override
    {
        try {
            _currentSession->TryPauseAsync().get();
            return true;
        }
        catch (const OS::Windows::Winrt::Exception &ex) {
            LOG(Warn, "_currentSession->TryPauseAsync() failed. {}", Helper::ToString(ex));
            return false;
        }
    }

    std::wstring GetProgramName() const override
    {
        return L"UniversalSystemSession";
    }

protected:
    Priority GetPriority() const override
    {
        return Priority::SystemSession;
    }

private:
    std::optional<GlobalSystemMediaTransportControlsSession> _currentSession;

    static GlobalSystemMediaTransportControlsSession GetCurrentSession()
    {
        return GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
            .get()
            .GetCurrentSession();
    }

    static auto GetSessions()
    {
        auto sessions =
            GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get().GetSessions();

        std::vector<GlobalSystemMediaTransportControlsSession> result;
        result.reserve(sessions.Size());

        for (size_t i = 0; i < sessions.Size(); ++i) {
            result.emplace_back(sessions.GetAt(i));
        }

        return result;
    }

    static bool IsPlaying(const GlobalSystemMediaTransportControlsSession &gsmtcs)
    {
        try {
            return gsmtcs.GetPlaybackInfo().PlaybackStatus() ==
                   GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        }
        catch (const OS::Windows::Winrt::Exception &ex) {
            LOG(Warn, "UniversalSystemSession Get playback status failed. Code: {:#x}, Message: {}",
                ex.code(), winrt::to_string(ex.message()));
            return false;
        }
    }
};

class QQMusic final : public MediaProgramThroughVirtualKeyAbstract
{
public:
    std::wstring GetProgramName() const override
    {
        return L"QQMusic";
    }

protected:
    std::wstring GetProcessName() const override
    {
        return L"QQMusic.exe";
    }
    std::wstring GetWindowClassName() const override
    {
        return L"TXGuiFoundation";
    }
    WindowMatchingFlags GetWindowMatchingFlags() const override
    {
        return WindowMatchingFlag::HasChildren | WindowMatchingFlag::NonEmptyTitle;
    }
    Priority GetPriority() const override
    {
        return Priority::MusicPlayer;
    }
};

class NeteaseMusic final : public MediaProgramThroughVirtualKeyAbstract
{
public:
    std::wstring GetProgramName() const override
    {
        return L"NeteaseMusic";
    }

protected:
    std::wstring GetProcessName() const override
    {
        return L"cloudmusic.exe";
    }
    std::wstring GetWindowClassName() const override
    {
        return L"MiniPlayer";
    }
    WindowMatchingFlags GetWindowMatchingFlags() const override
    {
        return WindowMatchingFlag::NonEmptyTitle;
    }
    Priority GetPriority() const override
    {
        return Priority::MusicPlayer;
    }
};

class KuGouMusic final : public MediaProgramThroughVirtualKeyAbstract
{
public:
    std::wstring GetProgramName() const override
    {
        return L"KuGouMusic";
    }

protected:
    std::wstring GetProcessName() const override
    {
        return L"KuGou.exe";
    }
    std::wstring GetWindowClassName() const override
    {
        return L"kugou_ui";
    }
    WindowMatchingFlags GetWindowMatchingFlags() const override
    {
        return WindowMatchingFlag::NonEmptyTitle;
    }
    Priority GetPriority() const override
    {
        return Priority::MusicPlayer;
    }
};

auto GetAvailablePrograms()
{
    std::vector<std::unique_ptr<MediaProgramAbstract>> result;

#define PUSH_IF_AVAILABLE(type)                                                                    \
    {                                                                                              \
        auto ptr = std::make_unique<type>();                                                       \
        if (ptr->IsAvailable()) {                                                                  \
            result.emplace_back(std::move(ptr));                                                   \
        }                                                                                          \
    }

    PUSH_IF_AVAILABLE(UniversalSystemSession);
    PUSH_IF_AVAILABLE(QQMusic);
    PUSH_IF_AVAILABLE(NeteaseMusic);
    PUSH_IF_AVAILABLE(KuGouMusic);

#undef PUSH_IF_AVAILABLE

    if (result.empty()) {
        return result;
    }

    // Sort by priority
    //
    std::sort(result.begin(), result.end(), [](const auto &first, const auto &second) {
        return first->GetPriority() < second->GetPriority();
    });

    return result;
}

//////////////////////////////////////////////////
// VolumeLevelLimiter::Callback
//

VolumeLevelLimiter::Callback::Callback(std::function<bool(uint32_t)> volumeLevelSetter)
    : _volumeLevelSetter{std::move(volumeLevelSetter)}
{
}

ULONG STDMETHODCALLTYPE VolumeLevelLimiter::Callback::AddRef()
{
    return ++_ref;
}

ULONG STDMETHODCALLTYPE VolumeLevelLimiter::Callback::Release()
{
    auto ref = --_ref;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE
VolumeLevelLimiter::Callback::QueryInterface(REFIID riid, void **ppvInterface)
{
    if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *)this;
    }
    else if (__uuidof(IAudioEndpointVolumeCallback) == riid) {
        AddRef();
        *ppvInterface = (IAudioEndpointVolumeCallback *)this;
    }
    else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE
VolumeLevelLimiter::Callback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify)
{
    LOG(Trace, "VolumeLevelLimiter::Callback::OnNotify() called.");

    if (pNotify == nullptr) {
        LOG(Warn, "VolumeLevelLimiter::Callback::OnNotify(): pNotify == nullptr");
        return E_INVALIDARG;
    }

    do {
        auto optVolumeLevel = _volumeLevel.load();

        if (!optVolumeLevel.has_value()) {
            LOG(Trace, "VolumeLevelLimiter::Callback::OnNotify(): Volume level unlimited.");
            break;
        }

        if (pNotify->fMasterVolume > optVolumeLevel.value() / 100.f) {
            LOG(Trace,
                "VolumeLevelLimiter::Callback::OnNotify(): Exceeded the limit, reduce. "
                "('{}', '{}')",
                pNotify->fMasterVolume, optVolumeLevel.value());
            _volumeLevelSetter(optVolumeLevel.value());
        }
        else {
            LOG(Trace,
                "VolumeLevelLimiter::Callback::OnNotify(): Not exceeded the limit. ('{}', '{}')",
                pNotify->fMasterVolume, optVolumeLevel.value());
        }

    } while (false);

    return S_OK;
}

void VolumeLevelLimiter::Callback::SetMaxValue(std::optional<uint32_t> volumeLevel)
{
    if (volumeLevel.has_value()) {
        volumeLevel = std::min<uint32_t>(volumeLevel.value(), 100);
    }
    _volumeLevel = std::move(volumeLevel);
}

//////////////////////////////////////////////////
// VolumeLevelLimiter
//

VolumeLevelLimiter::VolumeLevelLimiter(const std::string &deviceName)
    : _callback{[this](uint32_t volumeLevel) { return SetVolumeLevel(volumeLevel); }}
{
    _inited = Initialize(deviceName);
}

VolumeLevelLimiter::~VolumeLevelLimiter()
{
    if (_endpointVolume) {
        _endpointVolume->UnregisterControlChangeNotify(&_callback);
    }
}

std::optional<uint32_t> VolumeLevelLimiter::GetMaxValue() const
{
    return _maxVolumeLevel;
}

void VolumeLevelLimiter::SetMaxValue(std::optional<uint32_t> volumeLevel)
{
    _maxVolumeLevel = std::move(volumeLevel);

    if (!_inited) {
        LOG(Warn, "VolumeLevelLimiter is uninitiated or the initialization is failed.");
        return;
    }

    LOG(Info, "VolumeLevelLimiter::SetMaxValue() value: {}",
        _maxVolumeLevel.has_value() ? std::to_string(_maxVolumeLevel.value()) : "nullopt");

    if (_maxVolumeLevel.has_value()) {
        auto optCurrent = GetVolumeLevel();

        // Reduce the current volume level if it's exceeded the limit
        if (optCurrent.has_value() && optCurrent.value() > _maxVolumeLevel.value()) {
            SetVolumeLevel(_maxVolumeLevel.value());
        }
    }

    _callback.SetMaxValue(_maxVolumeLevel);
}

std::optional<uint32_t> VolumeLevelLimiter::GetVolumeLevel() const
{
    if (!_inited) {
        LOG(Warn, "VolumeLevelLimiter is uninitiated or the initialization is failed.");
        return std::nullopt;
    }

    float value = 0.f;
    HRESULT result = _endpointVolume->GetMasterVolumeLevelScalar(&value);
    if (FAILED(result)) {
        LOG(Trace, "'IAudioEndpointVolume::GetMasterVolumeLevelScalar' failed. HRESULT: {:#x}",
            result);
        return std::nullopt;
    }

    uint32_t return_value = (uint32_t)(value * 100.f);
    LOG(Trace, "GetVolumeLevel() returns '{}'", return_value);

    return return_value;
}

bool VolumeLevelLimiter::SetVolumeLevel(uint32_t volumeLevel) const
{
    if (!_inited) {
        LOG(Warn, "VolumeLevelLimiter is uninitiated or the initialization is failed.");
        return false;
    }

    LOG(Trace, "SetVolumeLevel() '{}'", volumeLevel);

    HRESULT result = _endpointVolume->SetMasterVolumeLevelScalar(volumeLevel / 100.f, nullptr);
    if (FAILED(result)) {
        LOG(Trace, "'IAudioEndpointVolume::SetMasterVolumeLevelScalar' failed. HRESULT: {:#x}",
            result);
        return false;
    }
    return true;
}

bool VolumeLevelLimiter::Initialize(const std::string &deviceName)
{
    LOG(Info, "VolumeLevelLimiter initializing.");

    OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> deviceEnumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, deviceEnumerator.GetIID(),
        (void **)deviceEnumerator.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "Create COM instance 'IMMDeviceEnumerator' failed. HRESULT: {:#x}", result);
        return false;
    }

    OS::Windows::Com::UniquePtr<IMMDeviceCollection> collection;
    result = deviceEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, collection.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "'EnumAudioEndpoints' failed. HRESULT: {:#x}", result);
        return false;
    }

    uint32_t count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        LOG(Warn, "'IMMDeviceCollection::GetCount' failed. HRESULT: {:#x}", result);
        return false;
    }

    OS::Windows::Com::UniquePtr<IMMDevice> audioEndpoint;

    for (uint32_t i = 0; i < count; ++i) {
        OS::Windows::Com::UniquePtr<IMMDevice> device;
        result = collection->Item(i, device.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Warn, "'IMMDeviceCollection::Item' failed. HRESULT: {:#x}", result);
            continue;
        }

        OS::Windows::Com::UniquePtr<IPropertyStore> property;
        result = device->OpenPropertyStore(STGM_READ, property.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Warn, "'IMMDevice::OpenPropertyStore' failed. HRESULT: {:#x}", result);
            continue;
        }

        PROPVARIANT variant;
        result = property->GetValue(PKEY_DeviceInterface_FriendlyName, &variant);
        if (FAILED(result)) {
            LOG(Warn, "Get property PKEY_DeviceInterface_FriendlyName failed. HRESULT: {:#x}",
                result);
            continue;
        }

        if (variant.pwszVal == nullptr) {
            LOG(Warn, "variant.pwszVal == nullptr");
        }
        else {
            std::wstring friendlyName{variant.pwszVal};
            LOG(Info, L"IMMDevice FriendlyName: {}", friendlyName);
            if (friendlyName == QString::fromStdString(deviceName).toStdWString()) {
                audioEndpoint = std::move(device);
            }
        }

        PropVariantClear(&variant);

        if (audioEndpoint) {
            break;
        }
    }

    if (!audioEndpoint) {
        LOG(Warn, "Cannot find the AirPods audio endpoint IMMDevice.");
        return false;
    }

    result = deviceEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, audioEndpoint.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "'GetDefaultAudioEndpoint' failed. HRESULT: {:#x}", result);
        return false;
    }

    result = audioEndpoint->Activate(
        _endpointVolume.GetIID(), CLSCTX_ALL, nullptr,
        (void **)_endpointVolume.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "'IMMDevice::Activate' IAudioEndpointVolume failed. HRESULT: {:#x}", result);
        return false;
    }

    result = _endpointVolume->RegisterControlChangeNotify(&_callback);
    if (FAILED(result)) {
        LOG(Warn, "IAudioEndpointVolume::RegisterControlChangeNotify failed. HRESULT: {:#x}",
            result);
        return false;
    }

    LOG(Info, "VolumeLevelLimiter initialization succeeded.");
    return true;
}
} // namespace Details

void Controller::Play()
{
    std::lock_guard<std::mutex> lock{_mutex};

    if (_pausedPrograms.empty()) {
        LOG(Trace, L"Paused programs vector is empty.");
        return;
    }

    for (const auto &program : _pausedPrograms) {
        if (!program->Play()) {
            LOG(Warn, L"Failed to play media. Program name: {}", program->GetProgramName());
        }
        else {
            LOG(Trace, L"Media played. Program name: {}", program->GetProgramName());
        }
    }

    _pausedPrograms.clear();
}

void Controller::Pause()
{
    std::lock_guard<std::mutex> lock{_mutex};

    auto programs = Details::GetAvailablePrograms();

    for (auto &&program : programs) {
        if (program->IsPlaying()) {
            if (!program->Pause()) {
                LOG(Warn, L"Failed to pause media. Program name: {}", program->GetProgramName());
            }
            else {
                LOG(Trace, L"Media paused. Program name: {}", program->GetProgramName());
                _pausedPrograms.emplace_back(std::move(program));
            }
        }
    }
}

void Controller::OnLimitedDeviceStateChanged(const std::string &deviceName)
{
    std::lock_guard<std::mutex> lock{_mutex};

    _volumeLevelLimiter.reset();

    if (!deviceName.empty()) {
        std::this_thread::sleep_for(1s);
        _volumeLevelLimiter = std::make_unique<Details::VolumeLevelLimiter>(deviceName);
        _volumeLevelLimiter->SetMaxValue(_maxVolumeLevel);
    }
}

void Controller::LimitVolume(std::optional<uint32_t> volumeLevel)
{
    std::lock_guard<std::mutex> lock{_mutex};

    _maxVolumeLevel = std::move(volumeLevel);

    if (_volumeLevelLimiter) {
        _volumeLevelLimiter->SetMaxValue(_maxVolumeLevel);
    }
}
} // namespace Core::GlobalMedia
