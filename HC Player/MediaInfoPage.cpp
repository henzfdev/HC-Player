#include "pch.h"
#include "MediaInfoPage.h"
#include "LocalizationManager.h"

#if __has_include("MediaInfoPage.g.cpp")
#include "MediaInfoPage.g.cpp"
#endif

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

using namespace winrt;

namespace
{
    using MediaInfoBridge::Analysis;
    using MediaInfoBridge::Field;
    using MediaInfoBridge::Section;
    using MediaInfoBridge::StreamKind;

    std::wstring MediaInfoString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(resourceId, fallback);
    }

    std::wstring LocalizedFieldName(std::wstring const& fieldName)
    {
        // MediaInfo's structured bridge intentionally keeps its canonical
        // English field identifiers. Translate only the label shown on screen;
        // values and the copied technical report remain untouched.
        return hc::localization::GetStringForFallback(
            L"MediaInfoField",
            fieldName);
    }

    std::wstring LocalizedMediaInfoError(std::wstring const& message)
    {
        if (message.empty())
        {
            return MediaInfoString(
                L"MediaInfoErrorUnavailableSource",
                L"As informações de mídia não estão disponíveis para esta fonte.");
        }

        if (message == L"No media is currently open.")
        {
            return MediaInfoString(
                L"MediaInfoErrorNoMediaOpen",
                L"Nenhuma mídia está aberta no momento.");
        }

        if (message == L"No media file is available for analysis.")
        {
            return MediaInfoString(
                L"MediaInfoErrorNoFileForAnalysis",
                L"Nenhum arquivo de mídia está disponível para análise.");
        }

        if (message ==
            L"As informações detalhadas do MediaInfo estão disponíveis apenas para arquivos locais.")
        {
            return MediaInfoString(
                L"MediaInfoErrorLocalFilesOnly",
                L"As informações detalhadas do MediaInfo estão disponíveis apenas para arquivos locais.");
        }

        if (message ==
            L"Detailed MediaInfo information is not available for optical discs.")
        {
            return MediaInfoString(
                L"MediaInfoErrorOpticalDisc",
                L"As informações detalhadas do MediaInfo não estão disponíveis para DVD e Blu-ray abertos como mídia óptica.");
        }

        if (message == L"No media file was provided.")
        {
            return MediaInfoString(
                L"MediaInfoErrorNoFileProvided",
                L"Nenhum arquivo de mídia foi fornecido.");
        }

        if (message ==
            L"MediaInfo.dll could not be loaded or is incompatible.")
        {
            return MediaInfoString(
                L"MediaInfoErrorDllUnavailable",
                L"MediaInfo.dll não pôde ser carregada ou é incompatível.");
        }

        if (message ==
            L"MediaInfo could not create an analysis context.")
        {
            return MediaInfoString(
                L"MediaInfoErrorAnalysisContext",
                L"O MediaInfo não pôde criar um contexto de análise.");
        }

        if (message ==
            L"MediaInfo could not open this media source.")
        {
            return MediaInfoString(
                L"MediaInfoErrorOpenSource",
                L"O MediaInfo não pôde abrir esta fonte de mídia.");
        }

        if (message ==
            L"MediaInfo returned an empty report.")
        {
            return MediaInfoString(
                L"MediaInfoErrorEmptyReport",
                L"O MediaInfo retornou um relatório vazio.");
        }

        // Unknown low-level errors are preserved verbatim instead of being
        // guessed or losing diagnostic information.
        return message;
    }


    bool UseWindows11CardStyle()
    {
        std::wstring value = L"hcplayer";
        PlayerTryGetSavedMpvOption(L"ui-card-style", value);
        return _wcsicmp(value.c_str(), L"windows11") == 0;
    }

    void ApplyMediaInfoSurfaceStyle(
        winrt::HCPlayer::implementation::MediaInfoPage& page)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Media;

        bool const windows11 = UseWindows11CardStyle();

        auto theme = page.Resources().ThemeDictionaries().Lookup(
            winrt::box_value(PlayerIsLightTheme() ? L"Light" : L"Dark"))
            .as<ResourceDictionary>();

        auto surface = theme.Lookup(winrt::box_value(
            windows11
                ? L"MediaInfoSurfaceWindows11"
                : L"MediaInfoSurface"))
            .as<Brush>();

        page.MediaInfoSurfaceHost().Background(surface);
        page.MediaInfoRoot().Background(surface);
    }

    std::wstring FieldValue(
        Section const& section,
        std::wstring const& name)
    {
        for (auto const& field : section.fields)
        {
            if (field.name == name)
            {
                return field.value;
            }
        }

        return {};
    }

    bool ShouldShowField(
        Section const& section,
        Field const& field)
    {
        // "Complete name" already contains the full path, so showing
        // "Folder name" immediately below it is redundant in HC Player.
        // Keep the MediaInfo analysis untouched; this is display/copy filtering only.
        return !(section.kind == StreamKind::General &&
            field.name == L"Folder name");
    }

    std::wstring CompactPixels(std::wstring const& value)
    {
        std::wstring digits;

        for (wchar_t ch : value)
        {
            if (iswdigit(ch))
            {
                digits.push_back(ch);
            }
        }

        return digits.empty() ? value : digits;
    }

    std::wstring LocalizedSectionTitle(
        Section const& section,
        size_t indexWithinKind,
        size_t totalOfKind)
    {
        std::wstring title;

        switch (section.kind)
        {
        case StreamKind::General:
            title = MediaInfoString(L"MediaInfoSectionGeneral", L"Geral");
            break;

        case StreamKind::Video:
            title = MediaInfoString(L"MediaInfoSectionVideo", L"Vídeo");
            break;

        case StreamKind::Audio:
            title = MediaInfoString(L"MediaInfoSectionAudio", L"Áudio");
            break;

        case StreamKind::Text:
            title = MediaInfoString(L"MediaInfoSectionSubtitles", L"Legendas");
            break;

        case StreamKind::Image:
            title = MediaInfoString(L"MediaInfoSectionImage", L"Imagem");
            break;

        case StreamKind::Menu:
            title = MediaInfoString(L"MediaInfoSectionMenu", L"Menu");
            break;

        default:
            title = MediaInfoString(L"MediaInfoSectionOther", L"Outros");
            break;
        }

        if (totalOfKind > 1)
        {
            title += L" #";
            title += std::to_wstring(indexWithinKind + 1);
        }

        return title;
    }

    wchar_t const* SectionGlyph(StreamKind kind)
    {
        switch (kind)
        {
        case StreamKind::General:
            return L"\xE8A5"; // Document
        case StreamKind::Video:
            return L"\xE714"; // Video
        case StreamKind::Audio:
            return L"\xE8D6"; // Music
        case StreamKind::Text:
            return L"\xE8C1"; // Text
        case StreamKind::Image:
            return L"\xEB9F"; // Photo
        case StreamKind::Menu:
            return L"\xE8FD"; // List
        default:
            return L"\xE946"; // Info
        }
    }

    std::vector<std::wstring> BuildChips(
        Section const& section)
    {
        std::vector<std::wstring> chips;

        auto add = [&](std::wstring value)
        {
            if (!value.empty() && chips.size() < 4)
            {
                if (std::find(chips.begin(), chips.end(), value) == chips.end())
                {
                    chips.push_back(std::move(value));
                }
            }
        };

        switch (section.kind)
        {
        case StreamKind::General:
            add(FieldValue(section, L"Format"));
            add(FieldValue(section, L"File size"));
            add(FieldValue(section, L"Duration"));
            add(FieldValue(section, L"Overall bit rate"));
            break;

        case StreamKind::Video:
        {
            add(FieldValue(section, L"Format"));

            std::wstring width =
                CompactPixels(FieldValue(section, L"Width"));

            std::wstring height =
                CompactPixels(FieldValue(section, L"Height"));

            if (!width.empty() && !height.empty())
            {
                add(width + L"×" + height);
            }

            add(FieldValue(section, L"Frame rate"));
            add(FieldValue(section, L"Bit rate"));

            if (chips.size() < 4)
            {
                add(FieldValue(section, L"HDR format"));
            }
            break;
        }

        case StreamKind::Audio:
        {
            std::wstring format = FieldValue(section, L"Format");
            std::wstring profile = FieldValue(section, L"Format profile");
            std::wstring features =
                FieldValue(section, L"Format additional features");

            if (!profile.empty() && profile != format)
            {
                if (!format.empty())
                {
                    format += L" ";
                }
                format += profile;
            }

            if (!features.empty())
            {
                if (!format.empty())
                {
                    format += L" ";
                }
                format += features;
            }

            add(format);
            add(FieldValue(section, L"Language"));
            add(FieldValue(section, L"Channels"));
            add(FieldValue(section, L"Bit rate"));
            break;
        }

        case StreamKind::Text:
            add(FieldValue(section, L"Format"));
            add(FieldValue(section, L"Language"));
            add(FieldValue(section, L"Title"));
            add(FieldValue(section, L"Default"));
            break;

        case StreamKind::Image:
        {
            add(FieldValue(section, L"Format"));

            std::wstring width =
                CompactPixels(FieldValue(section, L"Width"));

            std::wstring height =
                CompactPixels(FieldValue(section, L"Height"));

            if (!width.empty() && !height.empty())
            {
                add(width + L"×" + height);
            }

            add(FieldValue(section, L"Bit depth"));
            add(FieldValue(section, L"Color space"));
            break;
        }

        case StreamKind::Menu:
            add(FieldValue(section, L"Format"));
            add(FieldValue(section, L"Duration"));
            add(FieldValue(section, L"Language"));
            break;

        default:
            add(FieldValue(section, L"Format"));
            break;
        }

        return chips;
    }

    winrt::Microsoft::UI::Xaml::Controls::Border MakeChip(
        std::wstring const& text,
        winrt::Microsoft::UI::Xaml::Style const& chipStyle)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;

        Border chip;
        chip.Style(chipStyle);

        TextBlock textBlock;
        textBlock.Text(winrt::hstring{ text });
        textBlock.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe UI Variable Text" });
        textBlock.FontSize(11.0);
        textBlock.Opacity(0.72);
        textBlock.MaxWidth(150.0);
        textBlock.TextTrimming(TextTrimming::CharacterEllipsis);

        chip.Child(textBlock);
        return chip;
    }

    winrt::Microsoft::UI::Xaml::Controls::TextBlock MakeFieldsBlock(
        Section const& section,
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush const& selectionBrush)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Documents;

        // IMPORTANT:
        // Use ONE selectable TextBlock for the whole card. Text selection in
        // WinUI cannot cross from one TextBlock into another, so this allows
        // normal mouse drag-selection across any number of MediaInfo fields.
        TextBlock text;
        text.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe UI Variable Text" });
        text.FontSize(12.5);
        text.TextWrapping(TextWrapping::WrapWholeWords);
        text.IsTextSelectionEnabled(true);
        text.SelectionHighlightColor(selectionBrush);

        // Preserve approximately the same vertical density as the previous
        // StackPanel of individual rows.
        text.LineStackingStrategy(LineStackingStrategy::BaselineToBaseline);
        text.LineHeight(24.0);

        bool hasVisibleField = false;

        for (auto const& field : section.fields)
        {
            if (!ShouldShowField(section, field))
            {
                continue;
            }

            if (hasVisibleField)
            {
                LineBreak lineBreak;
                text.Inlines().Append(lineBreak);
            }

            Run label;
            label.Text(winrt::hstring{
                LocalizedFieldName(field.name) + L": " });
            label.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());

            Run value;
            value.Text(winrt::hstring{ field.value });

            text.Inlines().Append(label);
            text.Inlines().Append(value);
            hasVisibleField = true;
        }

        return text;
    }

    winrt::Microsoft::UI::Xaml::Controls::StackPanel MakeSectionView(
        Section const& section,
        std::wstring const& localizedTitle,
        winrt::Microsoft::UI::Xaml::Style const& cardStyle,
        winrt::Microsoft::UI::Xaml::Style const& chipStyle,
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush const& selectionBrush)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;

        StackPanel sectionRoot;
        sectionRoot.Spacing(9.0);

        StackPanel heading;
        heading.Orientation(Orientation::Horizontal);
        heading.Spacing(9.0);

        FontIcon icon;
        icon.Glyph(SectionGlyph(section.kind));
        icon.FontSize(17.0);
        icon.VerticalAlignment(VerticalAlignment::Center);

        TextBlock title;
        title.Text(winrt::hstring{ localizedTitle });
        title.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe UI Variable Display" });
        title.FontSize(22.0);
        title.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.VerticalAlignment(VerticalAlignment::Center);

        heading.Children().Append(icon);
        heading.Children().Append(title);

        sectionRoot.Children().Append(heading);

        auto chips = BuildChips(section);

        if (!chips.empty())
        {
            StackPanel chipRow;
            chipRow.Orientation(Orientation::Horizontal);
            chipRow.Spacing(6.0);

            for (auto const& chipText : chips)
            {
                chipRow.Children().Append(
                    MakeChip(
                        chipText,
                        chipStyle));
            }

            sectionRoot.Children().Append(chipRow);
        }

        Border card;
        card.Style(cardStyle);

        card.Child(
            MakeFieldsBlock(
                section,
                selectionBrush));

        sectionRoot.Children().Append(card);

        return sectionRoot;
    }

    std::wstring BuildCleanCopyText(
        Analysis const& analysis)
    {
        std::wstring text;

        for (size_t i = 0; i < analysis.sections.size(); ++i)
        {
            Section const& section = analysis.sections[i];

            size_t totalOfKind = 0;
            size_t indexWithinKind = 0;

            for (size_t j = 0; j < analysis.sections.size(); ++j)
            {
                if (analysis.sections[j].kind == section.kind)
                {
                    if (j < i)
                    {
                        ++indexWithinKind;
                    }
                    ++totalOfKind;
                }
            }

            // Keep the copied technical report itself in English.
            std::wstring title = section.title;
            if (totalOfKind > 1)
            {
                title += L" #";
                title += std::to_wstring(indexWithinKind + 1);
            }

            if (!text.empty())
            {
                text += L"\r\n\r\n";
            }

            text += title;
            text += L"\r\n";

            for (auto const& field : section.fields)
            {
                if (!ShouldShowField(section, field))
                {
                    continue;
                }

                text += field.name;
                text += L": ";
                text += field.value;
                text += L"\r\n";
            }
        }

        return text;
    }
}

namespace winrt::HCPlayer::implementation
{
    MediaInfoPage::MediaInfoPage()
    {
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);
    }

    void MediaInfoPage::MediaInfoLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_loaded = true;

        CopyAllButton().IsEnabled(false);
    }

    void MediaInfoPage::PrepareForOpen()
    {
        if (!m_loaded)
        {
            return;
        }

        m_closing = false;

        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);

        ApplyMediaInfoSurfaceStyle(*this);

        MediaInfoRoot().IsHitTestVisible(true);
        MediaInfoTranslate().X(0.0);
        MediaInfoRoot().Opacity(1.0);

        // Extra top spacing is used only by the unsupported/web notice.
        // Reset it here so local-file/loading layout stays unchanged.
        MediaInfoStatus().Margin(
            Microsoft::UI::Xaml::Thickness{
                0.0, 0.0, 0.0, 0.0 });

        auto visual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(MediaInfoRoot());

        visual.StopAnimation(L"Offset");
        visual.Offset({ -112.0f, 0.0f, 0.0f });

        auto headerVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(MediaInfoHeader());

        headerVisual.StopAnimation(L"Opacity");
        headerVisual.Opacity(0.76f);

        auto contentVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(MediaInfoScrollViewer());

        contentVisual.StopAnimation(L"Opacity");
        contentVisual.Opacity(0.88f);

        ++m_requestGeneration;
        RefreshAsync();
    }

    void MediaInfoPage::BeginOpenAnimation()
    {
        auto root = MediaInfoRoot();
        auto header = MediaInfoHeader();
        auto content = MediaInfoScrollViewer();

        root.DispatcherQueue().TryEnqueue([root, header, content]()
        {
            auto visual =
                Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(root);

            auto compositor = visual.Compositor();

            auto arrival = compositor.CreateCubicBezierEasingFunction(
                { 0.16f, 1.0f },
                { 0.30f, 1.0f });

            auto settle = compositor.CreateCubicBezierEasingFunction(
                { 0.20f, 0.0f },
                { 0.20f, 1.0f });

            auto slide = compositor.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(
                0.82f,
                { 3.0f, 0.0f, 0.0f },
                arrival);

            slide.InsertKeyFrame(
                1.0f,
                { 0.0f, 0.0f, 0.0f },
                settle);

            slide.Duration(std::chrono::milliseconds(360));
            slide.StopBehavior(
                Microsoft::UI::Composition::AnimationStopBehavior::
                    SetToFinalValue);

            visual.StartAnimation(L"Offset", slide);

            auto animateContent =
                [compositor, arrival](
                    auto const& target,
                    std::chrono::milliseconds delay,
                    std::chrono::milliseconds duration)
            {
                auto targetVisual =
                    Microsoft::UI::Xaml::Hosting::
                        ElementCompositionPreview::
                            GetElementVisual(target);

                auto opacity =
                    compositor.CreateScalarKeyFrameAnimation();

                opacity.InsertKeyFrame(
                    1.0f,
                    1.0f,
                    arrival);

                opacity.DelayTime(delay);
                opacity.Duration(duration);
                opacity.StopBehavior(
                    Microsoft::UI::Composition::AnimationStopBehavior::
                        SetToFinalValue);

                targetVisual.StartAnimation(
                    L"Opacity",
                    opacity);
            };

            animateContent(
                header,
                std::chrono::milliseconds(45),
                std::chrono::milliseconds(235));

            animateContent(
                content,
                std::chrono::milliseconds(75),
                std::chrono::milliseconds(285));
        });
    }

    void MediaInfoPage::RequestClose()
    {
        if (m_closing)
        {
            return;
        }

        m_closing = true;
        ++m_requestGeneration;

        using namespace Microsoft::UI::Xaml::Media::Animation;

        auto slide = DoubleAnimation{};
        slide.To(-520.0);
        slide.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(190)));
        slide.EnableDependentAnimation(true);

        auto ease = CubicEase{};
        ease.EasingMode(EasingMode::EaseIn);
        slide.EasingFunction(ease);

        Storyboard::SetTarget(slide, MediaInfoTranslate());
        Storyboard::SetTargetProperty(slide, L"X");

        auto fade = DoubleAnimation{};
        fade.To(0.0);
        fade.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(145)));

        Storyboard::SetTarget(fade, MediaInfoRoot());
        Storyboard::SetTargetProperty(fade, L"Opacity");

        auto storyboard = Storyboard{};
        storyboard.Children().Append(slide);
        storyboard.Children().Append(fade);

        auto root = MediaInfoRoot();
        storyboard.Completed([root](auto&&, auto&&)
        {
            root.IsHitTestVisible(false);
            PlayerCloseMediaInfo();
        });

        storyboard.Begin();
    }

    void MediaInfoPage::ScrollBy(int wheelDelta)
    {
        double const target =
            MediaInfoScrollViewer().VerticalOffset() - wheelDelta;

        MediaInfoScrollViewer().ChangeView(
            nullptr,
            target,
            nullptr,
            false);
    }

    void MediaInfoPage::ShowLoading()
    {
        m_copyText.clear();

        CopyAllButton().IsEnabled(false);
        MediaInfoStatus().IsOpen(false);

        LoadingPanel().Visibility(
            Microsoft::UI::Xaml::Visibility::Visible);

        LoadingRing().IsActive(true);

        SectionsHost().Children().Clear();
    }

    void MediaInfoPage::ShowAnalysis(
        MediaInfoBridge::Analysis const& analysis)
    {
        LoadingRing().IsActive(false);

        LoadingPanel().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);

        MediaInfoStatus().IsOpen(false);
        MediaInfoStatus().Margin(
            Microsoft::UI::Xaml::Thickness{
                0.0, 0.0, 0.0, 0.0 });
        SectionsHost().Children().Clear();

        auto resources = Resources();

        auto cardStyle =
            resources.Lookup(box_value(
                UseWindows11CardStyle()
                    ? L"MediaInfoCardWindows11"
                    : L"MediaInfoCard")).
                as<Microsoft::UI::Xaml::Style>();

        auto chipStyle =
            resources.Lookup(box_value(L"MediaInfoChip")).
                as<Microsoft::UI::Xaml::Style>();

        auto selectionBrush =
            resources.Lookup(box_value(L"MediaInfoSelectionBrush")).
                as<Microsoft::UI::Xaml::Media::SolidColorBrush>();

        for (size_t i = 0; i < analysis.sections.size(); ++i)
        {
            auto const& section = analysis.sections[i];

            size_t totalOfKind = 0;
            size_t indexWithinKind = 0;

            for (size_t j = 0; j < analysis.sections.size(); ++j)
            {
                if (analysis.sections[j].kind == section.kind)
                {
                    if (j < i)
                    {
                        ++indexWithinKind;
                    }

                    ++totalOfKind;
                }
            }

            SectionsHost().Children().Append(
                MakeSectionView(
                    section,
                    LocalizedSectionTitle(
                        section,
                        indexWithinKind,
                        totalOfKind),
                    cardStyle,
                    chipStyle,
                    selectionBrush));
        }

        m_copyText = BuildCleanCopyText(analysis);
        CopyAllButton().IsEnabled(!m_copyText.empty());

        MediaInfoScrollViewer().ChangeView(
            nullptr,
            0.0,
            nullptr,
            true);
    }

    void MediaInfoPage::ShowError(
        std::wstring const& message)
    {
        m_copyText.clear();

        LoadingRing().IsActive(false);

        LoadingPanel().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);

        CopyAllButton().IsEnabled(false);
        SectionsHost().Children().Clear();

        MediaInfoStatus().Severity(
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);

        // Unsupported/web sources have plenty of vertical space. Give the
        // notice a little more separation from the header without moving the
        // normal local-file MediaInfo layout.
        MediaInfoStatus().Margin(
            Microsoft::UI::Xaml::Thickness{
                0.0, 10.0, 0.0, 0.0 });

        winrt::hstring const statusMessage{
            LocalizedMediaInfoError(message) };

        // MediaInfoStatusText lives inside InfoBar.Content. Depending on the
        // XAML namescope, Page::FindName() is not guaranteed to resolve that
        // child. Never use a throwing .as<TextBlock>() lookup on the web/error
        // path: a missing namescope entry must not be able to terminate HC Player.
        bool customMessageApplied = false;

        if (auto contentGrid =
            MediaInfoStatus().Content().
                try_as<Microsoft::UI::Xaml::Controls::Grid>())
        {
            for (auto const& child : contentGrid.Children())
            {
                if (auto statusText =
                    child.try_as<Microsoft::UI::Xaml::Controls::TextBlock>())
                {
                    statusText.Text(statusMessage);
                    customMessageApplied = true;
                    break;
                }
            }
        }

        if (!customMessageApplied)
        {
            // Defensive fallback: if the custom centered-icon content ever
            // changes or fails to materialize, use the native InfoBar message
            // instead of throwing/crashing. This path is intentionally boring
            // but guarantees that unsupported web sources remain safe.
            MediaInfoStatus().Content(nullptr);
            MediaInfoStatus().IsIconVisible(true);
            MediaInfoStatus().Message(statusMessage);
        }

        MediaInfoStatus().IsOpen(true);
    }

    winrt::fire_and_forget MediaInfoPage::RefreshAsync()
    {
        auto lifetime = get_strong();
        auto uiContext = winrt::apartment_context{};

        uint64_t const generation =
            m_requestGeneration;

        ShowLoading();

        MediaInfoBridge::Analysis analysis;
        std::wstring error;

        co_await winrt::resume_background();

        bool const success =
            PlayerGetMediaInfoAnalysis(
                analysis,
                error);

        co_await uiContext;

        if (generation != m_requestGeneration)
        {
            co_return;
        }

        if (success)
        {
            ShowAnalysis(analysis);
        }
        else
        {
            ShowError(error);
        }
    }

    void MediaInfoPage::CopyAllClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_copyText.empty())
        {
            return;
        }

        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(winrt::hstring{ m_copyText });

        Windows::ApplicationModel::DataTransfer::Clipboard::
            SetContent(package);

        Windows::ApplicationModel::DataTransfer::Clipboard::
            Flush();
    }

    void MediaInfoPage::CloseClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RequestClose();
    }
}
