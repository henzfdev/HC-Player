#include "pch.h"
#include "MediaInfoBridge.h"

#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace
{
    using MediaInfo_New_fn =
        void* (__stdcall*)();

    using MediaInfo_Delete_fn =
        void (__stdcall*)(void* handle);

    using MediaInfo_Open_fn =
        size_t (__stdcall*)(void* handle, wchar_t const* fileName);

    using MediaInfo_Close_fn =
        void (__stdcall*)(void* handle);

    using MediaInfo_Inform_fn =
        wchar_t const* (__stdcall*)(void* handle, size_t reserved);

    using MediaInfo_Get_fn =
        wchar_t const* (__stdcall*)(
            void* handle,
            size_t streamKind,
            size_t streamNumber,
            wchar_t const* parameter,
            size_t kindOfInfo,
            size_t kindOfSearch);

    using MediaInfo_Count_Get_fn =
        size_t (__stdcall*)(
            void* handle,
            size_t streamKind,
            size_t streamNumber);

    using MediaInfo_Option_fn =
        wchar_t const* (__stdcall*)(
            void* handle,
            wchar_t const* option,
            wchar_t const* value);

    constexpr size_t InfoName = 0;
    constexpr size_t InfoText = 1;

    struct MediaInfoApi
    {
        HMODULE module{};

        MediaInfo_New_fn New{};
        MediaInfo_Delete_fn Delete{};
        MediaInfo_Open_fn Open{};
        MediaInfo_Close_fn Close{};
        MediaInfo_Inform_fn Inform{};
        MediaInfo_Get_fn Get{};
        MediaInfo_Count_Get_fn CountGet{};
        MediaInfo_Option_fn Option{};

        bool attempted{};
        bool ready{};

        ~MediaInfoApi()
        {
            if (module)
            {
                FreeLibrary(module);
                module = nullptr;
            }
        }

        static std::wstring ExecutableDirectory()
        {
            std::wstring buffer(32768, L'\0');

            DWORD const length = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));

            if (length == 0 || length >= buffer.size())
            {
                return {};
            }

            buffer.resize(length);

            size_t const separator = buffer.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
            {
                return {};
            }

            buffer.resize(separator);
            return buffer;
        }

        bool Load()
        {
            if (attempted)
            {
                return ready;
            }

            attempted = true;

            std::wstring const directory = ExecutableDirectory();
            if (directory.empty())
            {
                return false;
            }

            std::wstring const dllPath =
                directory + L"\\MediaInfo.dll";

            // Load only HC Player's bundled copy, beside the executable.
            module = LoadLibraryW(dllPath.c_str());
            if (!module)
            {
                return false;
            }

            New = reinterpret_cast<MediaInfo_New_fn>(
                GetProcAddress(module, "MediaInfo_New"));

            Delete = reinterpret_cast<MediaInfo_Delete_fn>(
                GetProcAddress(module, "MediaInfo_Delete"));

            Open = reinterpret_cast<MediaInfo_Open_fn>(
                GetProcAddress(module, "MediaInfo_Open"));

            Close = reinterpret_cast<MediaInfo_Close_fn>(
                GetProcAddress(module, "MediaInfo_Close"));

            Inform = reinterpret_cast<MediaInfo_Inform_fn>(
                GetProcAddress(module, "MediaInfo_Inform"));

            Get = reinterpret_cast<MediaInfo_Get_fn>(
                GetProcAddress(module, "MediaInfo_Get"));

            CountGet = reinterpret_cast<MediaInfo_Count_Get_fn>(
                GetProcAddress(module, "MediaInfo_Count_Get"));

            Option = reinterpret_cast<MediaInfo_Option_fn>(
                GetProcAddress(module, "MediaInfo_Option"));

            ready =
                New &&
                Delete &&
                Open &&
                Close &&
                Inform &&
                Get &&
                CountGet &&
                Option;

            if (!ready)
            {
                FreeLibrary(module);
                module = nullptr;

                New = nullptr;
                Delete = nullptr;
                Open = nullptr;
                Close = nullptr;
                Inform = nullptr;
                Get = nullptr;
                CountGet = nullptr;
                Option = nullptr;
            }

            return ready;
        }
    };

    MediaInfoApi g_mediaInfo;

    std::wstring GetValue(
        void* handle,
        MediaInfoBridge::StreamKind kind,
        size_t streamNumber,
        wchar_t const* parameter)
    {
        wchar_t const* const value =
            g_mediaInfo.Get(
                handle,
                static_cast<size_t>(kind),
                streamNumber,
                parameter,
                InfoText,
                InfoName);

        return value ? std::wstring{ value } : std::wstring{};
    }

    std::wstring FirstValue(
        void* handle,
        MediaInfoBridge::StreamKind kind,
        size_t streamNumber,
        std::initializer_list<wchar_t const*> parameters)
    {
        for (auto const* parameter : parameters)
        {
            std::wstring value =
                GetValue(handle, kind, streamNumber, parameter);

            if (!value.empty())
            {
                return value;
            }
        }

        return {};
    }

    void Add(
        MediaInfoBridge::Section& section,
        std::wstring name,
        std::wstring value)
    {
        if (value.empty())
        {
            return;
        }

        section.fields.push_back(
            { std::move(name), std::move(value) });
    }

    void AddFirst(
        void* handle,
        MediaInfoBridge::Section& section,
        std::wstring name,
        std::initializer_list<wchar_t const*> parameters)
    {
        Add(
            section,
            std::move(name),
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                parameters));
    }

    MediaInfoBridge::Section BuildGeneral(
        void* handle)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::General;
        section.streamIndex = 0;
        section.title = L"General";

        AddFirst(handle, section, L"Complete name",
            { L"CompleteName" });
        AddFirst(handle, section, L"Folder name",
            { L"FolderName" });
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Format version",
            { L"Format_Version" });
        AddFirst(handle, section, L"Attachments",
            { L"Attachments" });
        AddFirst(handle, section, L"Format profile",
            { L"Format_Profile" });
        AddFirst(handle, section, L"Codec ID",
            { L"CodecID" });
        AddFirst(handle, section, L"File size",
            { L"FileSize/String", L"FileSize" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Overall bit rate mode",
            { L"OverallBitRate_Mode/String", L"OverallBitRate_Mode" });
        AddFirst(handle, section, L"Overall bit rate",
            { L"OverallBitRate/String", L"OverallBitRate" });
        AddFirst(handle, section, L"Frame rate",
            { L"FrameRate/String", L"FrameRate" });
        AddFirst(handle, section, L"Encoded date",
            { L"Encoded_Date" });
        AddFirst(handle, section, L"Tagged date",
            { L"Tagged_Date" });
        AddFirst(handle, section, L"Writing application",
            { L"Encoded_Application/String", L"Encoded_Application" });
        AddFirst(handle, section, L"Writing library",
            { L"Encoded_Library/String", L"Encoded_Library" });

        return section;
    }

    MediaInfoBridge::Section BuildAudioOnlyGeneral(
        void* handle)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::General;
        section.streamIndex = 0;
        section.title = L"General";

        // Audio-only files intentionally use a curated metadata set.
        // Keep this list explicit so the HC Player panel stays useful and
        // predictable instead of exposing every vendor-specific MediaInfo tag.
        AddFirst(handle, section, L"Complete name",
            { L"CompleteName" });
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Format information",
            { L"Format_Info", L"Format/Info" });
        AddFirst(handle, section, L"Format profile",
            { L"Format_Profile" });
        AddFirst(handle, section, L"Codec ID",
            { L"CodecID/String", L"CodecID" });
        AddFirst(handle, section, L"File size",
            { L"FileSize/String", L"FileSize" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Overall bit rate mode",
            { L"OverallBitRate_Mode/String", L"OverallBitRate_Mode" });
        AddFirst(handle, section, L"Overall bit rate",
            { L"OverallBitRate/String", L"OverallBitRate" });
        AddFirst(handle, section, L"Title",
            { L"Title" });
        AddFirst(handle, section, L"Album",
            { L"Album" });
        AddFirst(handle, section, L"Album performer",
            { L"Album/Performer", L"Album_Performer" });
        AddFirst(handle, section, L"Part position",
            { L"Part/Position", L"Part_Position" });
        AddFirst(handle, section, L"Part total",
            { L"Part/Position_Total", L"Part_Position_Total" });
        AddFirst(handle, section, L"Track position",
            { L"Track/Position", L"Track_Position" });
        AddFirst(handle, section, L"Track total",
            { L"Track/Position_Total", L"Track_Position_Total" });
        AddFirst(handle, section, L"Performer",
            { L"Performer" });
        AddFirst(handle, section, L"Composer",
            { L"Composer", L"Composer/String" });
        AddFirst(handle, section, L"Producer",
            { L"Producer" });
        AddFirst(handle, section, L"Recorded date",
            { L"Recorded_Date" });
        AddFirst(handle, section, L"ISRC",
            { L"ISRC" });
        AddFirst(handle, section, L"Copyright",
            { L"Copyright" });
        AddFirst(handle, section, L"Cover",
            { L"Cover" });
        AddFirst(handle, section, L"Cover type",
            { L"Cover_Type", L"Cover/Type" });
        AddFirst(handle, section, L"Cover MIME",
            { L"Cover_Mime", L"Cover/Mime" });
        AddFirst(handle, section, L"Lyrics",
            { L"Lyrics" });
        AddFirst(handle, section, L"Content rating",
            { L"ContentType", L"Rating" });
        AddFirst(handle, section, L"UPC",
            { L"UPC", L"BarCode" });

        return section;
    }

    MediaInfoBridge::Section BuildVideo(
        void* handle,
        size_t index)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::Video;
        section.streamIndex = index;
        section.title = L"Video";

        AddFirst(handle, section, L"ID",
            { L"ID/String", L"ID" });
        AddFirst(handle, section, L"Title",
            { L"Title" });
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Format information",
            { L"Format_Info", L"Format/Info" });
        AddFirst(handle, section, L"Format profile",
            { L"Format_Profile" });
        AddFirst(handle, section, L"Format level",
            { L"Format_Level" });
        AddFirst(handle, section, L"Format settings",
            { L"Format_Settings" });
        AddFirst(handle, section, L"Codec ID",
            { L"CodecID" });
        AddFirst(handle, section, L"Codec ID information",
            { L"CodecID/Info", L"CodecID_Info" });
        AddFirst(handle, section, L"Codec configuration box",
            { L"CodecConfigurationBox" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Bit rate mode",
            { L"BitRate_Mode/String", L"BitRate_Mode" });
        AddFirst(handle, section, L"Bit rate",
            { L"BitRate/String", L"BitRate" });
        AddFirst(handle, section, L"Maximum bit rate",
            { L"BitRate_Maximum/String", L"BitRate_Maximum" });
        AddFirst(handle, section, L"Bits/(Pixel*Frame)",
            { L"Bits-(Pixel*Frame)" });
        AddFirst(handle, section, L"Width",
            { L"Width/String", L"Width" });
        AddFirst(handle, section, L"Height",
            { L"Height/String", L"Height" });
        AddFirst(handle, section, L"Display aspect ratio",
            { L"DisplayAspectRatio/String", L"DisplayAspectRatio" });
        AddFirst(handle, section, L"Frame rate mode",
            { L"FrameRate_Mode/String", L"FrameRate_Mode" });
        AddFirst(handle, section, L"Frame rate",
            { L"FrameRate/String", L"FrameRate" });
        AddFirst(handle, section, L"Frame count",
            { L"FrameCount" });
        AddFirst(handle, section, L"Color space",
            { L"ColorSpace" });
        AddFirst(handle, section, L"Chroma subsampling",
            { L"ChromaSubsampling" });
        AddFirst(handle, section, L"Bit depth",
            { L"BitDepth/String", L"BitDepth" });
        AddFirst(handle, section, L"Scan type",
            { L"ScanType" });
        AddFirst(handle, section, L"Color range",
            { L"colour_range", L"ColorRange" });
        AddFirst(handle, section, L"Color primaries",
            { L"colour_primaries", L"colour_primaries/String" });
        AddFirst(handle, section, L"Transfer characteristics",
            { L"transfer_characteristics", L"transfer_characteristics/String" });
        AddFirst(handle, section, L"Matrix coefficients",
            { L"matrix_coefficients", L"matrix_coefficients/String" });
        AddFirst(handle, section, L"HDR format",
            { L"HDR_Format" });
        AddFirst(handle, section, L"HDR compatibility",
            { L"HDR_Format_Compatibility" });
        AddFirst(handle, section, L"Mastering display color primaries",
            { L"MasteringDisplay_ColorPrimaries" });
        AddFirst(handle, section, L"Mastering display luminance",
            { L"MasteringDisplay_Luminance" });
        AddFirst(handle, section, L"Maximum content light level",
            { L"MaxCLL/String", L"MaxCLL" });
        AddFirst(handle, section, L"Maximum frame-average light level",
            { L"MaxFALL/String", L"MaxFALL" });
        AddFirst(handle, section, L"Stream size",
            { L"StreamSize/String", L"StreamSize" });
        AddFirst(handle, section, L"Writing library",
            { L"Encoded_Library/String", L"Encoded_Library" });
        AddFirst(handle, section, L"Encoding settings",
            { L"Encoded_Library_Settings" });
        AddFirst(handle, section, L"Language",
            { L"Language/String", L"Language" });
        AddFirst(handle, section, L"Default",
            { L"Default/String", L"Default" });
        AddFirst(handle, section, L"Forced",
            { L"Forced/String", L"Forced" });

        return section;
    }

    MediaInfoBridge::Section BuildAudio(
        void* handle,
        size_t index,
        bool audioOnly)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::Audio;
        section.streamIndex = index;
        section.title = L"Audio";

        AddFirst(handle, section, L"ID",
            { L"ID/String", L"ID" });
        AddFirst(handle, section, L"Title",
            { L"Title" });
        std::wstring const baseFormat =
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                { L"Format" });

        std::wstring displayFormat =
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                { L"Format/String", L"Format" });

        std::wstring const formatAdditionalFeatures =
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                { L"Format_AdditionalFeatures" });

        // Keep JOC attached to the E-AC-3 format name ("E-AC-3 JOC")
        // instead of exposing a separate "Format additional features" row.
        if (baseFormat == L"E-AC-3" &&
            !formatAdditionalFeatures.empty() &&
            displayFormat.find(formatAdditionalFeatures) == std::wstring::npos)
        {
            if (!displayFormat.empty())
            {
                displayFormat += L" ";
            }
            displayFormat += formatAdditionalFeatures;
        }

        Add(section, L"Format", displayFormat);

        std::wstring const formatProfile =
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                { L"Format_Profile" });

        if (!formatProfile.empty() &&
            displayFormat.find(formatProfile) == std::wstring::npos)
        {
            Add(section, L"Format profile", formatProfile);
        }

        AddFirst(handle, section, L"Format information",
            { L"Format_Info", L"Format/Info" });
        AddFirst(handle, section, L"Format settings",
            { L"Format_Settings" });

        std::wstring const commercialName =
            FirstValue(
                handle,
                section.kind,
                section.streamIndex,
                { L"Format_Commercial_IfAny", L"Format_Commercial", L"CommercialName" });

        // Avoid redundant rows such as "Format: FLAC" + "Commercial name: FLAC".
        // Keep meaningful names such as Dolby Digital Plus with Dolby Atmos.
        if (!commercialName.empty() &&
            commercialName != displayFormat &&
            commercialName != baseFormat)
        {
            Add(section, L"Commercial name", commercialName);
        }
        AddFirst(handle, section, L"Codec ID",
            { L"CodecID" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Bit rate mode",
            { L"BitRate_Mode/String", L"BitRate_Mode" });
        AddFirst(handle, section, L"Bit rate",
            { L"BitRate/String", L"BitRate" });
        AddFirst(handle, section, L"Maximum bit rate",
            { L"BitRate_Maximum/String", L"BitRate_Maximum" });
        AddFirst(handle, section, L"Channels",
            { L"Channels/String", L"Channels" });
        AddFirst(handle, section, L"Channel layout",
            { L"ChannelLayout" });
        AddFirst(handle, section, L"Sampling rate",
            { L"SamplingRate/String", L"SamplingRate" });
        AddFirst(handle, section, L"Frame rate",
            { L"FrameRate/String", L"FrameRate" });
        AddFirst(handle, section, L"Bit depth",
            { L"BitDepth/String", L"BitDepth" });
        AddFirst(handle, section, L"Compression mode",
            { L"Compression_Mode" });
        if (audioOnly)
        {
            AddFirst(handle, section, L"Replay gain",
                { L"ReplayGain_Gain/String", L"ReplayGain_Gain" });
            AddFirst(handle, section, L"Replay gain peak",
                { L"ReplayGain_Peak" });
            AddFirst(handle, section, L"Library used",
                { L"Encoded_Library/String", L"Encoded_Library" });
            AddFirst(handle, section, L"MD5 of the unencoded content",
                { L"MD5_Unencoded" });
        }
        AddFirst(handle, section, L"Stream size",
            { L"StreamSize/String", L"StreamSize" });
        AddFirst(handle, section, L"Service kind",
            { L"ServiceKind/String", L"ServiceKind" });
        AddFirst(handle, section, L"Language",
            { L"Language/String", L"Language" });
        AddFirst(handle, section, L"Default",
            { L"Default/String", L"Default" });
        AddFirst(handle, section, L"Forced",
            { L"Forced/String", L"Forced" });
        AddFirst(handle, section, L"Alternate group",
            { L"AlternateGroup/String", L"AlternateGroup" });
        AddFirst(handle, section, L"Encoded date",
            { L"Encoded_Date" });
        AddFirst(handle, section, L"Tagged date",
            { L"Tagged_Date" });

        if (baseFormat == L"E-AC-3")
        {
            AddFirst(handle, section, L"Complexity index",
                { L"ComplexityIndex" });
            AddFirst(handle, section, L"Number of dynamic objects",
                { L"NumberOfDynamicObjects" });
            AddFirst(handle, section, L"Bed channel count",
                { L"BedChannelCount/String", L"BedChannelCount" });
            AddFirst(handle, section, L"Bed channel configuration",
                { L"BedChannelConfiguration" });
        }

        if (baseFormat == L"AC-3" || baseFormat == L"E-AC-3")
        {
            auto addDbField = [&](std::wstring name,
                                  std::initializer_list<wchar_t const*> parameters)
            {
                std::wstring value =
                    FirstValue(
                        handle,
                        section.kind,
                        section.streamIndex,
                        parameters);

                if (!value.empty() &&
                    value.find(L"dB") == std::wstring::npos)
                {
                    value += L" dB";
                }

                Add(section, std::move(name), std::move(value));
            };

            addDbField(L"Dialog normalization",
                { L"dialnorm/String", L"dialnorm" });
            addDbField(L"compr",
                { L"compr/String", L"compr" });
            AddFirst(handle, section, L"dmixmod",
                { L"dmixmod/String", L"dmixmod" });
            addDbField(L"cmixlev",
                { L"cmixlev/String", L"cmixlev" });
            addDbField(L"surmixlev",
                { L"surmixlev/String", L"surmixlev" });
            addDbField(L"ltrtcmixlev",
                { L"ltrtcmixlev/String", L"ltrtcmixlev" });
            addDbField(L"ltrtsurmixlev",
                { L"ltrtsurmixlev/String", L"ltrtsurmixlev" });
            addDbField(L"lorocmixlev",
                { L"lorocmixlev/String", L"lorocmixlev" });
            addDbField(L"lorosurmixlev",
                { L"lorosurmixlev/String", L"lorosurmixlev" });
            addDbField(L"dialnorm_Average",
                { L"dialnorm_Average/String", L"dialnorm_Average" });
            addDbField(L"dialnorm_Minimum",
                { L"dialnorm_Minimum/String", L"dialnorm_Minimum" });
            addDbField(L"dialnorm_Maximum",
                { L"dialnorm_Maximum/String", L"dialnorm_Maximum" });
        }

        return section;
    }

    MediaInfoBridge::Section BuildText(
        void* handle,
        size_t index)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::Text;
        section.streamIndex = index;
        section.title = L"Text";

        AddFirst(handle, section, L"ID",
            { L"ID/String", L"ID" });
        AddFirst(handle, section, L"Title",
            { L"Title" });
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Codec ID",
            { L"CodecID" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Frame rate",
            { L"FrameRate/String", L"FrameRate" });
        AddFirst(handle, section, L"Element count",
            { L"ElementCount" });
        AddFirst(handle, section, L"Stream size",
            { L"StreamSize/String", L"StreamSize" });
        AddFirst(handle, section, L"Language",
            { L"Language/String", L"Language" });
        AddFirst(handle, section, L"Default",
            { L"Default/String", L"Default" });
        AddFirst(handle, section, L"Forced",
            { L"Forced/String", L"Forced" });

        return section;
    }

    MediaInfoBridge::Section BuildImage(
        void* handle,
        size_t index,
        bool audioOnly)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::Image;
        section.streamIndex = index;
        section.title = L"Image";

        AddFirst(handle, section, L"ID",
            { L"ID/String", L"ID" });
        if (audioOnly)
        {
            AddFirst(handle, section, L"Type",
                { L"Type" });
        }
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Width",
            { L"Width/String", L"Width" });
        AddFirst(handle, section, L"Height",
            { L"Height/String", L"Height" });
        AddFirst(handle, section, L"Color space",
            { L"ColorSpace" });
        AddFirst(handle, section, L"Chroma subsampling",
            { L"ChromaSubsampling" });
        AddFirst(handle, section, L"Bit depth",
            { L"BitDepth/String", L"BitDepth" });
        AddFirst(handle, section, L"Compression mode",
            { L"Compression_Mode" });
        if (audioOnly)
        {
            AddFirst(handle, section, L"Size",
                { L"StreamSize/String", L"StreamSize" });
        }

        return section;
    }

    MediaInfoBridge::Section BuildMenu(
        void* handle,
        size_t index)
    {
        using namespace MediaInfoBridge;

        Section section;
        section.kind = StreamKind::Menu;
        section.streamIndex = index;
        section.title = L"Menu";

        AddFirst(handle, section, L"ID",
            { L"ID/String", L"ID" });
        AddFirst(handle, section, L"Format",
            { L"Format" });
        AddFirst(handle, section, L"Duration",
            { L"Duration/String", L"Duration/String3", L"Duration" });
        AddFirst(handle, section, L"Language",
            { L"Language/String", L"Language" });

        return section;
    }

    void AppendStreams(
        void* handle,
        MediaInfoBridge::StreamKind kind,
        MediaInfoBridge::Analysis& analysis,
        bool audioOnly)
    {
        size_t const count =
            g_mediaInfo.CountGet(
                handle,
                static_cast<size_t>(kind),
                static_cast<size_t>(-1));

        for (size_t index = 0; index < count; ++index)
        {
            MediaInfoBridge::Section section;

            switch (kind)
            {
            case MediaInfoBridge::StreamKind::Video:
                section = BuildVideo(handle, index);
                break;

            case MediaInfoBridge::StreamKind::Audio:
                section = BuildAudio(handle, index, audioOnly);
                break;

            case MediaInfoBridge::StreamKind::Text:
                section = BuildText(handle, index);
                break;

            case MediaInfoBridge::StreamKind::Image:
                section = BuildImage(handle, index, audioOnly);
                break;

            case MediaInfoBridge::StreamKind::Menu:
                section = BuildMenu(handle, index);
                break;

            default:
                continue;
            }

            if (!section.fields.empty())
            {
                analysis.sections.push_back(std::move(section));
            }
        }
    }

    bool OpenAnalysisContext(
        std::wstring const& filePath,
        void*& handle,
        std::wstring& error)
    {
        handle = nullptr;
        error.clear();

        if (filePath.empty())
        {
            error = L"No media file was provided.";
            return false;
        }

        if (!g_mediaInfo.Load())
        {
            error =
                L"MediaInfo.dll could not be loaded or is incompatible.";
            return false;
        }

        handle = g_mediaInfo.New();
        if (!handle)
        {
            error = L"MediaInfo could not create an analysis context.";
            return false;
        }

        size_t const opened =
            g_mediaInfo.Open(handle, filePath.c_str());

        if (opened == 0)
        {
            g_mediaInfo.Delete(handle);
            handle = nullptr;

            error = L"MediaInfo could not open this media source.";
            return false;
        }

        return true;
    }

    void CloseAnalysisContext(void* handle)
    {
        if (!handle)
        {
            return;
        }

        g_mediaInfo.Close(handle);
        g_mediaInfo.Delete(handle);
    }
}

namespace MediaInfoBridge
{
    bool IsAvailable()
    {
        return g_mediaInfo.Load();
    }

    bool AnalyzeFileStructured(
        std::wstring const& filePath,
        Analysis& analysis,
        std::wstring& error)
    {
        analysis = {};
        error.clear();

        void* handle = nullptr;
        if (!OpenAnalysisContext(filePath, handle, error))
        {
            return false;
        }

        // Keep the established structured panel for video/container media.
        // For pure audio files, use the curated metadata list above so common
        // music tags are visible without dumping every custom field MediaInfo knows.
        size_t const videoCount =
            g_mediaInfo.CountGet(
                handle,
                static_cast<size_t>(StreamKind::Video),
                static_cast<size_t>(-1));

        size_t const audioCount =
            g_mediaInfo.CountGet(
                handle,
                static_cast<size_t>(StreamKind::Audio),
                static_cast<size_t>(-1));

        bool const audioOnly =
            videoCount == 0 && audioCount > 0;

        Section general =
            audioOnly ? BuildAudioOnlyGeneral(handle) : BuildGeneral(handle);

        if (!general.fields.empty())
        {
            analysis.sections.push_back(std::move(general));
        }

        AppendStreams(handle, StreamKind::Video, analysis, audioOnly);
        AppendStreams(handle, StreamKind::Audio, analysis, audioOnly);
        AppendStreams(handle, StreamKind::Text, analysis, audioOnly);
        AppendStreams(handle, StreamKind::Image, analysis, audioOnly);
        AppendStreams(handle, StreamKind::Menu, analysis, audioOnly);

        // Keep MediaInfo's exhaustive report only for Copy all / diagnostics.
        // It is deliberately NOT intended for the visible UI.
        g_mediaInfo.Option(handle, L"Complete", L"1");

        wchar_t const* const nativeReport =
            g_mediaInfo.Inform(handle, 0);

        if (nativeReport)
        {
            analysis.rawReport.assign(nativeReport);
        }

        CloseAnalysisContext(handle);

        if (analysis.sections.empty() && analysis.rawReport.empty())
        {
            error = L"MediaInfo returned no usable information.";
            return false;
        }

        return true;
    }

    bool AnalyzeFile(
        std::wstring const& filePath,
        std::wstring& report,
        std::wstring& error)
    {
        report.clear();

        Analysis analysis;
        if (!AnalyzeFileStructured(
            filePath,
            analysis,
            error))
        {
            return false;
        }

        report = std::move(analysis.rawReport);

        if (report.empty())
        {
            error = L"MediaInfo returned an empty report.";
            return false;
        }

        return true;
    }
}
