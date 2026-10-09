#include "pch.h"
#include "YouTubeCommentsPage.h"
#include "LocalizationManager.h"

#if __has_include("YouTubeCommentsPage.g.cpp")
#include "YouTubeCommentsPage.g.cpp"
#endif

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

using namespace winrt;

namespace
{
    std::wstring CommentsString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(resourceId, fallback);
    }

    std::wstring SavedUiValue(std::wstring const& name)
    {
        std::wstring value;
        return PlayerTryGetSavedMpvOption(name, value) ? value : std::wstring{};
    }

    bool UseWindows11CardStyle()
    {
        std::wstring value = L"hcplayer";
        PlayerTryGetSavedMpvOption(L"ui-card-style", value);
        return _wcsicmp(value.c_str(), L"windows11") == 0;
    }

    void ApplySurfaceStyle(
        winrt::HCPlayer::implementation::YouTubeCommentsPage& page)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Media;

        auto theme = page.Resources().ThemeDictionaries().Lookup(
            box_value(PlayerIsLightTheme() ? L"Light" : L"Dark"))
            .as<ResourceDictionary>();

        auto surface = theme.Lookup(box_value(
            UseWindows11CardStyle()
                ? L"YouTubeCommentsSurfaceWindows11"
                : L"YouTubeCommentsSurface"))
            .as<Brush>();

        page.CommentsSurfaceHost().Background(surface);
        page.CommentsRoot().Background(surface);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    }

    std::wstring ExtractYouTubeVideoId(std::wstring const& input)
    {
        if (input.empty()) return {};

        std::wstring value = input;
        auto fragment = value.find(L'#');
        if (fragment != std::wstring::npos) value.resize(fragment);

        std::wstring lower = ToLower(value);

        auto queryValue = [&](std::wstring const& key) -> std::wstring
        {
            std::wstring marker = key + L"=";
            size_t pos = lower.find(marker);
            while (pos != std::wstring::npos)
            {
                bool const validStart = pos == 0 || lower[pos - 1] == L'?' ||
                    lower[pos - 1] == L'&';
                if (validStart)
                {
                    size_t start = pos + marker.size();
                    size_t end = value.find_first_of(L"&#", start);
                    std::wstring result = value.substr(
                        start,
                        end == std::wstring::npos ? std::wstring::npos : end - start);
                    if (!result.empty()) return result;
                }
                pos = lower.find(marker, pos + marker.size());
            }
            return {};
        };

        if (auto id = queryValue(L"v"); !id.empty()) return id;

        auto afterPathSegment = [&](std::wstring const& segment) -> std::wstring
        {
            size_t pos = lower.find(segment);
            if (pos == std::wstring::npos) return {};
            size_t start = pos + segment.size();
            size_t end = value.find_first_of(L"?&#/", start);
            return value.substr(
                start,
                end == std::wstring::npos ? std::wstring::npos : end - start);
        };

        for (auto const* segment :
            { L"youtu.be/", L"/shorts/", L"/live/", L"/embed/" })
        {
            auto id = afterPathSegment(segment);
            if (!id.empty()) return id;
        }

        return {};
    }

    std::wstring UrlEncode(std::wstring_view value)
    {
        std::string utf8 = winrt::to_string(winrt::hstring{ value });
        std::wostringstream out;
        out << std::uppercase << std::hex;
        for (unsigned char ch : utf8)
        {
            if ((ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                out << static_cast<wchar_t>(ch);
            }
            else
            {
                out << L'%' << std::setw(2) << std::setfill(L'0')
                    << static_cast<int>(ch);
            }
        }
        return out.str();
    }

    std::wstring YouTubeApiError(std::wstring const& body)
    {
        using namespace winrt::Windows::Data::Json;
        try
        {
            JsonObject root = JsonObject::Parse(winrt::hstring{ body });
            if (root.HasKey(L"error"))
            {
                JsonObject error = root.GetNamedObject(L"error");

                // YouTube returns commentsDisabled as a structured API reason.
                // Handle it explicitly so the UI never exposes the raw API
                // message, which currently contains HTML documentation markup.
                if (error.HasKey(L"errors"))
                {
                    JsonArray errors = error.GetNamedArray(L"errors");
                    for (uint32_t index = 0; index < errors.Size(); ++index)
                    {
                        try
                        {
                            JsonObject detail = errors.GetObjectAt(index);
                            auto reason = detail.GetNamedString(L"reason", L"");
                            if (reason == L"commentsDisabled")
                            {
                                return CommentsString(
                                    L"YouTubeCommentsDisabled",
                                    L"Os comentários estão desativados para este vídeo");
                            }
                        }
                        catch (...)
                        {
                        }
                    }
                }

                auto message = error.GetNamedString(L"message", L"");
                if (!message.empty()) return message.c_str();
            }
        }
        catch (...)
        {
        }
        return CommentsString(
            L"YouTubeCommentsApiGenericError",
            L"O YouTube recusou a solicitação de comentários");
    }

    std::wstring CompactPublishedDate(std::wstring value)
    {
        if (value.size() >= 10 && value[4] == L'-' && value[7] == L'-')
        {
            return value.substr(0, 10);
        }
        return value;
    }

    bool TryParseYouTubePublishedAt(
        std::wstring const& value,
        std::chrono::system_clock::time_point& result)
    {
        // YouTube Data API publishedAt values are RFC 3339 timestamps. Keep the
        // parser deliberately small: use the fixed date/time part and honor the
        // normal Z or numeric UTC offset. If the value ever arrives in another
        // shape, callers safely fall back to the existing YYYY-MM-DD display.
        if (value.size() < 19 ||
            value[4] != L'-' || value[7] != L'-' ||
            (value[10] != L'T' && value[10] != L' ') ||
            value[13] != L':' || value[16] != L':')
        {
            return false;
        }

        auto parsePart = [&](size_t offset, size_t count, int& output)
        {
            int parsed = 0;
            for (size_t index = 0; index < count; ++index)
            {
                wchar_t const ch = value[offset + index];
                if (ch < L'0' || ch > L'9') return false;
                parsed = (parsed * 10) + static_cast<int>(ch - L'0');
            }
            output = parsed;
            return true;
        };

        int year{}, month{}, day{}, hour{}, minute{}, second{};
        if (!parsePart(0, 4, year) || !parsePart(5, 2, month) ||
            !parsePart(8, 2, day) || !parsePart(11, 2, hour) ||
            !parsePart(14, 2, minute) || !parsePart(17, 2, second))
        {
            return false;
        }

        std::tm utc{};
        utc.tm_year = year - 1900;
        utc.tm_mon = month - 1;
        utc.tm_mday = day;
        utc.tm_hour = hour;
        utc.tm_min = minute;
        utc.tm_sec = second;
        utc.tm_isdst = 0;

        std::time_t raw = _mkgmtime(&utc);
        if (raw == static_cast<std::time_t>(-1)) return false;
        result = std::chrono::system_clock::from_time_t(raw);

        size_t zone = 19;
        if (zone < value.size() && value[zone] == L'.')
        {
            ++zone;
            while (zone < value.size() &&
                value[zone] >= L'0' && value[zone] <= L'9')
            {
                ++zone;
            }
        }

        if (zone >= value.size() || value[zone] == L'Z' || value[zone] == L'z')
        {
            return true;
        }

        if (value[zone] != L'+' && value[zone] != L'-') return false;
        bool const positiveOffset = value[zone] == L'+';
        ++zone;
        if (zone + 4 >= value.size() || value[zone + 2] != L':') return false;

        int offsetHour{}, offsetMinute{};
        if (!parsePart(zone, 2, offsetHour) ||
            !parsePart(zone + 3, 2, offsetMinute) ||
            offsetHour > 23 || offsetMinute > 59)
        {
            return false;
        }

        auto const offset = std::chrono::minutes{
            (offsetHour * 60) + offsetMinute };
        result += positiveOffset ? -offset : offset;
        return true;
    }

    std::wstring RelativeTimeText(
        std::wstring_view singularResource,
        std::wstring_view pluralResource,
        std::wstring_view singularFallback,
        std::wstring_view pluralFallback,
        std::int64_t count)
    {
        std::wstring text = CommentsString(
            count == 1 ? singularResource : pluralResource,
            count == 1 ? singularFallback : pluralFallback);
        auto const marker = text.find(L"{0}");
        if (marker != std::wstring::npos)
        {
            text.replace(marker, 3, std::to_wstring(count));
        }
        return text;
    }

    std::wstring CompactPublishedTime(std::wstring const& value)
    {
        std::wstring const date = CompactPublishedDate(value);
        std::chrono::system_clock::time_point published{};
        if (!TryParseYouTubePublishedAt(value, published)) return date;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - published).count();

        // Small clock differences should not turn a brand-new comment into a
        // future date. Larger future timestamps stay on the safe date fallback.
        if (elapsed < -60) return date;
        if (elapsed < 1) elapsed = 1;

        if (elapsed < 60)
        {
            return RelativeTimeText(
                L"YouTubeCommentsSecondAgoSingular",
                L"YouTubeCommentsSecondAgoPlural",
                L"há {0} segundo", L"há {0} segundos", elapsed);
        }

        if (elapsed < 60 * 60)
        {
            auto const minutes = elapsed / 60;
            return RelativeTimeText(
                L"YouTubeCommentsMinuteAgoSingular",
                L"YouTubeCommentsMinuteAgoPlural",
                L"há {0} minuto", L"há {0} minutos", minutes);
        }

        if (elapsed < 24 * 60 * 60)
        {
            auto const hours = elapsed / (60 * 60);
            return RelativeTimeText(
                L"YouTubeCommentsHourAgoSingular",
                L"YouTubeCommentsHourAgoPlural",
                L"há {0} hora", L"há {0} horas", hours);
        }

        return date;
    }

    bool YouTubeCommentWasEdited(
        std::wstring const& publishedAt,
        std::wstring const& updatedAt)
    {
        if (publishedAt.empty() || updatedAt.empty()) return false;

        std::chrono::system_clock::time_point published{};
        std::chrono::system_clock::time_point updated{};
        if (!TryParseYouTubePublishedAt(publishedAt, published) ||
            !TryParseYouTubePublishedAt(updatedAt, updated))
        {
            return false;
        }

        return updated > published;
    }

    std::wstring CommentPublishedTime(
        std::wstring const& publishedAt,
        std::wstring const& updatedAt)
    {
        std::wstring text = CompactPublishedTime(publishedAt);
        if (!text.empty() && YouTubeCommentWasEdited(publishedAt, updatedAt))
        {
            text += L" ";
            text += CommentsString(L"YouTubeCommentsEdited", L"(editado)");
        }
        return text;
    }
}

namespace winrt::HCPlayer::implementation
{
    YouTubeCommentsPage::YouTubeCommentsPage()
    {
        // Pin the page theme before XAML is created. A DesktopWindowXamlSource
        // otherwise resolves the first set of ThemeResources from the Windows
        // theme, which can make the very first opening dark while HC Player is
        // explicitly using its light theme.
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);
        InitializeComponent();
    }

    void YouTubeCommentsPage::CommentsLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_loaded = true;
    }

    void YouTubeCommentsPage::PrepareForOpen()
    {
        m_closing = false;
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);

        if (!m_loaded) return;

        ApplySurfaceStyle(*this);

        CommentsRoot().IsHitTestVisible(true);
        CommentsTranslate().X(0.0);
        CommentsRoot().Opacity(1.0);

        auto visual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(CommentsRoot());
        visual.StopAnimation(L"Offset");
        visual.Offset({ 112.0f, 0.0f, 0.0f });

        auto headerVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(CommentsHeader());
        headerVisual.StopAnimation(L"Opacity");
        headerVisual.Opacity(0.76f);

        auto contentVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(CommentsScrollViewer());
        contentVisual.StopAnimation(L"Opacity");
        contentVisual.Opacity(0.88f);

        ++m_requestGeneration;
        std::wstring const currentVideoId =
            ExtractYouTubeVideoId(PlayerGetCurrentMediaPath());

        // Normal videos keep their existing XAML tree while this panel is hidden.
        // Reopening the same video therefore preserves the exact scroll position,
        // loaded pages and expanded replies without another YouTube API request.
        // Live chat deliberately does not use this path and reconnects as before.
        if (m_reuseNormalState && !currentVideoId.empty() &&
            currentVideoId == m_videoId)
        {
            m_reuseNormalState = false;
            m_liveMode = false;
            m_liveChatId.clear();
            m_liveNextPageToken.clear();
            SetCommentsModeUi();
            ShowLoading(false, false);
            return;
        }

        m_reuseNormalState = false;
        m_videoId = currentVideoId;
        m_nextPageToken.clear();
        m_liveMode = false;
        m_liveChatId.clear();
        m_liveNextPageToken.clear();
        SetCommentsModeUi();
        CommentsHost().Children().Clear();
        CommentsScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
        DetectModeAndLoadAsync();
    }

    void YouTubeCommentsPage::PrepareForClose()
    {
        // Reuse only a fully settled normal-comments view. If a request is still
        // running, or this is live chat, reopening follows the existing reload
        // path so no half-finished state is retained.
        m_reuseNormalState =
            !m_liveMode && !m_loading &&
            m_replyRequestsInFlight == 0 && !m_videoId.empty();

        ++m_requestGeneration;
        m_loading = false;
        m_liveMode = false;
        m_liveChatId.clear();
        m_liveNextPageToken.clear();
        LoadingRing().IsActive(false);
    }

    void YouTubeCommentsPage::BeginOpenAnimation()
    {
        auto root = CommentsRoot();
        auto header = CommentsHeader();
        auto content = CommentsScrollViewer();

        root.DispatcherQueue().TryEnqueue([root, header, content]()
        {
            auto visual =
                Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(root);
            auto compositor = visual.Compositor();
            auto arrival = compositor.CreateCubicBezierEasingFunction(
                { 0.16f, 1.0f }, { 0.30f, 1.0f });
            auto settle = compositor.CreateCubicBezierEasingFunction(
                { 0.20f, 0.0f }, { 0.20f, 1.0f });

            auto slide = compositor.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(0.82f, { -3.0f, 0.0f, 0.0f }, arrival);
            slide.InsertKeyFrame(1.0f, { 0.0f, 0.0f, 0.0f }, settle);
            slide.Duration(std::chrono::milliseconds(360));
            slide.StopBehavior(
                Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
            visual.StartAnimation(L"Offset", slide);

            auto animateOpacity = [compositor, arrival](
                auto const& target,
                std::chrono::milliseconds delay,
                std::chrono::milliseconds duration)
            {
                auto targetVisual =
                    Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                        GetElementVisual(target);
                auto opacity = compositor.CreateScalarKeyFrameAnimation();
                opacity.InsertKeyFrame(1.0f, 1.0f, arrival);
                opacity.DelayTime(delay);
                opacity.Duration(duration);
                opacity.StopBehavior(
                    Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
                targetVisual.StartAnimation(L"Opacity", opacity);
            };

            animateOpacity(header,
                std::chrono::milliseconds(45),
                std::chrono::milliseconds(235));
            animateOpacity(content,
                std::chrono::milliseconds(75),
                std::chrono::milliseconds(285));
        });
    }

    void YouTubeCommentsPage::RequestClose()
    {
        if (m_closing) return;
        m_closing = true;
        ++m_requestGeneration;

        using namespace Microsoft::UI::Xaml::Media::Animation;

        auto slide = DoubleAnimation{};
        slide.To(520.0);
        slide.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(190)));
        slide.EnableDependentAnimation(true);
        auto ease = CubicEase{};
        ease.EasingMode(EasingMode::EaseIn);
        slide.EasingFunction(ease);
        Storyboard::SetTarget(slide, CommentsTranslate());
        Storyboard::SetTargetProperty(slide, L"X");

        auto fade = DoubleAnimation{};
        fade.To(0.0);
        fade.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(145)));
        Storyboard::SetTarget(fade, CommentsRoot());
        Storyboard::SetTargetProperty(fade, L"Opacity");

        auto storyboard = Storyboard{};
        storyboard.Children().Append(slide);
        storyboard.Children().Append(fade);

        auto root = CommentsRoot();
        storyboard.Completed([root](auto&&, auto&&)
        {
            root.IsHitTestVisible(false);
            PlayerCloseYouTubeComments();
        });
        storyboard.Begin();
    }

    void YouTubeCommentsPage::ScrollBy(int wheelDelta)
    {
        double const target = CommentsScrollViewer().VerticalOffset() - wheelDelta;
        CommentsScrollViewer().ChangeView(nullptr, target, nullptr, false);
    }

    void YouTubeCommentsPage::RefreshClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_loading) return;
        ++m_requestGeneration;
        m_videoId = ExtractYouTubeVideoId(PlayerGetCurrentMediaPath());
        m_nextPageToken.clear();
        m_liveMode = false;
        m_liveChatId.clear();
        m_liveNextPageToken.clear();
        SetCommentsModeUi();
        CommentsHost().Children().Clear();
        CommentsScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
        DetectModeAndLoadAsync();
    }

    void YouTubeCommentsPage::LoadMoreClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_liveMode || m_loading || m_nextPageToken.empty()) return;
        LoadCommentsAsync(true);
    }

    void YouTubeCommentsPage::SortSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        // SelectedIndex is initialized while XAML is being created. Do not
        // start a request until the panel has actually finished loading.
        if (!m_loaded || m_loading || m_liveMode) return;

        auto item = SortComboBox().SelectedItem()
            .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        if (!item) return;

        std::wstring order;
        try
        {
            order = winrt::unbox_value<winrt::hstring>(item.Tag()).c_str();
        }
        catch (...)
        {
            return;
        }

        if (order != L"relevance" && order != L"time") return;
        if (order == m_order) return;

        m_order = order;
        ++m_requestGeneration;
        m_nextPageToken.clear();
        CommentsHost().Children().Clear();
        CommentsScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
        LoadCommentsAsync(false);
    }

    void YouTubeCommentsPage::CloseClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RequestClose();
    }

    void YouTubeCommentsPage::SetCommentsModeUi()
    {
        HeaderTitleText().Text(winrt::hstring{
            CommentsString(L"YouTubeCommentsHeaderTitleText", L"Comentários do YouTube") });
        HeaderSubtitleText().Text(winrt::hstring{
            CommentsString(L"YouTubeCommentsHeaderSubtitleText", L"Comentários públicos do vídeo atual") });
        LoadingText().Text(winrt::hstring{
            CommentsString(L"YouTubeCommentsLoadingTextValue", L"Carregando comentários…") });
        SortComboBox().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    }

    void YouTubeCommentsPage::SetLiveChatModeUi()
    {
        HeaderTitleText().Text(winrt::hstring{
            CommentsString(L"YouTubeLiveChatHeaderTitle", L"Chat ao vivo") });
        HeaderSubtitleText().Text(winrt::hstring{
            CommentsString(L"YouTubeLiveChatHeaderSubtitle", L"Mensagens da transmissão ao vivo") });
        LoadingText().Text(winrt::hstring{
            CommentsString(L"YouTubeLiveChatLoading", L"Conectando ao chat ao vivo…") });
        SortComboBox().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        LoadMoreButton().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void YouTubeCommentsPage::ShowStatus(std::wstring const& message)
    {
        StatusText().Text(winrt::hstring{ message });
        StatusText().Visibility(
            Microsoft::UI::Xaml::Visibility::Visible);
    }

    void YouTubeCommentsPage::ShowLoading(bool loading, bool append)
    {
        LoadingRing().IsActive(loading);
        LoadingPanel().Visibility(
            loading && !append
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed);
        RefreshButton().IsEnabled(!loading);
        LoadMoreButton().IsEnabled(!loading);
        SortComboBox().IsEnabled(!loading);
        if (loading)
        {
            StatusText().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    Microsoft::UI::Xaml::Controls::Border
        YouTubeCommentsPage::CreateCommentCard(
            std::wstring const& commentId,
            std::wstring const& author,
            std::wstring const& avatarUrl,
            std::wstring const& text,
            int64_t likeCount,
            int64_t replyCount,
            std::wstring const& publishedAt,
            std::wstring const& updatedAt)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Imaging;

        Border card;
        card.Style(Resources().Lookup(box_value(
            UseWindows11CardStyle()
                ? L"YouTubeCommentCardWindows11"
                : L"YouTubeCommentCard")).as<Microsoft::UI::Xaml::Style>());

        Grid layout;
        layout.ColumnSpacing(11.0);
        layout.ColumnDefinitions().Append(ColumnDefinition{});
        layout.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromPixels(36.0));
        layout.ColumnDefinitions().Append(ColumnDefinition{});
        layout.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(
            1.0, GridUnitType::Star));

        Border avatar;
        avatar.Width(34.0);
        avatar.Height(34.0);
        avatar.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 17.0 });
        avatar.VerticalAlignment(VerticalAlignment::Top);
        if (!avatarUrl.empty())
        {
            try
            {
                BitmapImage bitmap;
                bitmap.UriSource(Windows::Foundation::Uri{ winrt::hstring{ avatarUrl } });
                ImageBrush imageBrush;
                imageBrush.ImageSource(bitmap);
                imageBrush.Stretch(Stretch::UniformToFill);
                avatar.Background(imageBrush);
            }
            catch (...)
            {
            }
        }

        StackPanel body;
        body.Spacing(5.0);
        Grid::SetColumn(body, 1);

        std::wstring authorLine = author.empty()
            ? CommentsString(L"YouTubeCommentsUnknownAuthor", L"Usuário")
            : author;
        auto date = CommentPublishedTime(publishedAt, updatedAt);

        Grid authorHeader;
        authorHeader.ColumnDefinitions().Append(ColumnDefinition{});
        authorHeader.ColumnDefinitions().Append(ColumnDefinition{});
        authorHeader.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(
            1.0, GridUnitType::Star));
        authorHeader.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(
            0.0, GridUnitType::Auto));

        TextBlock authorText;
        authorText.Text(winrt::hstring{ authorLine });
        authorText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        authorText.FontSize(12.5);
        authorText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        authorText.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetColumn(authorText, 0);
        authorHeader.Children().Append(authorText);

        if (!date.empty())
        {
            TextBlock dateText;
            dateText.Text(winrt::hstring{ date });
            dateText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            dateText.FontSize(11.0);
            dateText.FontWeight(Windows::UI::Text::FontWeights::Normal());
            dateText.Opacity(0.72);
            dateText.Margin(Thickness{ 8.0, 1.0, 0.0, 0.0 });
            dateText.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(dateText, 1);
            authorHeader.Children().Append(dateText);
        }

        TextBlock commentText;
        commentText.Text(winrt::hstring{ text });
        commentText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        commentText.FontSize(13.0);
        commentText.TextWrapping(TextWrapping::WrapWholeWords);
        commentText.IsTextSelectionEnabled(true);

        std::wstring meta;
        if (likeCount > 0)
        {
            meta += CommentsString(L"YouTubeCommentsLikesPrefix", L"Curtidas") +
                L": " + std::to_wstring(likeCount);
        }
        if (replyCount > 0)
        {
            if (!meta.empty()) meta += L"   ";
            meta += CommentsString(L"YouTubeCommentsRepliesPrefix", L"Respostas") +
                L": " + std::to_wstring(replyCount);
        }
        body.Children().Append(authorHeader);
        body.Children().Append(commentText);

        if (!meta.empty())
        {
            TextBlock metaText;
            metaText.Text(winrt::hstring{ meta });
            metaText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            metaText.FontSize(11.0);
            metaText.Opacity(0.58);
            body.Children().Append(metaText);
        }

        // Replies are intentionally loaded only when requested. The main comments
        // list stays light, while a user can expand any thread that has replies.
        if (replyCount > 0 && !commentId.empty())
        {
            Button repliesButton;
            repliesButton.Style(Resources().Lookup(
                box_value(L"YouTubeCommentsReplyButton"))
                .as<Microsoft::UI::Xaml::Style>());

            // Keep the approved stock dark hover, while giving the light theme
            // the same subtle hover used by HC Player's light transport controls.
            // Comments can be preserved across close/reopen, so a reply Button may
            // survive a theme change. Remove the light-only local override again
            // when the Button becomes dark; otherwise the translucent black brush
            // is retained on a dark surface and the hover appears to disappear.
            auto applyReplyHoverTheme = [](Button const& button, bool lightTheme)
            {
                auto const key = box_value(L"ButtonBackgroundPointerOver");
                if (lightTheme)
                {
                    auto hoverBrush = SolidColorBrush{
                        Windows::UI::Color{ 0x0F, 0x00, 0x00, 0x00 } };
                    button.Resources().Insert(key, hoverBrush);
                }
                else
                {
                    if (button.Resources().HasKey(key))
                    {
                        button.Resources().Remove(key);
                    }
                }
            };

            applyReplyHoverTheme(repliesButton, PlayerIsLightTheme());
            repliesButton.ActualThemeChanged(
                [applyReplyHoverTheme](FrameworkElement const& sender, IInspectable const&)
                {
                    auto button = sender.try_as<Button>();
                    if (!button) return;
                    applyReplyHoverTheme(
                        button, button.ActualTheme() == ElementTheme::Light);
                });

            repliesButton.HorizontalAlignment(HorizontalAlignment::Left);
            repliesButton.Padding(Thickness{ 6.0, 5.0, 6.0, 5.0 });
            repliesButton.Background(nullptr);
            repliesButton.BorderThickness(Thickness{ 0.0 });
            repliesButton.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            repliesButton.FontSize(11.5);
            repliesButton.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            repliesButton.Tag(box_value(false));

            std::wstring const viewLabel =
                CommentsString(L"YouTubeCommentsViewRepliesPrefix", L"Ver") + L" " +
                std::to_wstring(replyCount) + L" " +
                CommentsString(
                    replyCount == 1
                        ? L"YouTubeCommentsReplySingular"
                        : L"YouTubeCommentsReplyPlural",
                    replyCount == 1 ? L"resposta" : L"respostas");
            repliesButton.Content(box_value(winrt::hstring{ viewLabel }));

            StackPanel repliesHost;
            repliesHost.Spacing(8.0);
            repliesHost.Margin(Thickness{ 0.0, 2.0, 0.0, 0.0 });
            repliesHost.Visibility(Visibility::Collapsed);

            repliesButton.Click(
                [this, commentId, repliesHost, repliesButton, replyCount](auto&&, auto&&)
                {
                    bool loaded = false;
                    try
                    {
                        loaded = unbox_value<bool>(repliesButton.Tag());
                    }
                    catch (...)
                    {
                    }

                    if (!loaded)
                    {
                        repliesHost.Visibility(Visibility::Visible);
                        LoadRepliesAsync(commentId, repliesHost, repliesButton, replyCount);
                        return;
                    }

                    bool const showing =
                        repliesHost.Visibility() == Visibility::Visible;
                    repliesHost.Visibility(
                        showing ? Visibility::Collapsed : Visibility::Visible);

                    if (showing)
                    {
                        std::wstring const label =
                            CommentsString(L"YouTubeCommentsViewRepliesPrefix", L"Ver") + L" " +
                            std::to_wstring(replyCount) + L" " +
                            CommentsString(
                                replyCount == 1
                                    ? L"YouTubeCommentsReplySingular"
                                    : L"YouTubeCommentsReplyPlural",
                                replyCount == 1 ? L"resposta" : L"respostas");
                        repliesButton.Content(box_value(winrt::hstring{ label }));
                    }
                    else
                    {
                        repliesButton.Content(box_value(winrt::hstring{
                            CommentsString(
                                L"YouTubeCommentsHideReplies",
                                L"Ocultar respostas") }));
                    }
                });

            body.Children().Append(repliesButton);
            body.Children().Append(repliesHost);
        }

        layout.Children().Append(avatar);
        layout.Children().Append(body);
        card.Child(layout);
        return card;
    }

    Microsoft::UI::Xaml::Controls::Border YouTubeCommentsPage::CreateLiveChatCard(
        std::wstring const& author,
        std::wstring const& avatarUrl,
        std::wstring const& text,
        std::wstring const& amount,
        bool isOwner,
        bool isModerator,
        bool isMember)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Imaging;

        Border card;
        card.Style(Resources().Lookup(box_value(
            UseWindows11CardStyle()
                ? L"YouTubeCommentCardWindows11"
                : L"YouTubeCommentCard")).as<Microsoft::UI::Xaml::Style>());
        card.Padding(Thickness{ 11.0, 9.0, 11.0, 9.0 });

        Grid layout;
        layout.ColumnSpacing(10.0);
        layout.ColumnDefinitions().Append(ColumnDefinition{});
        layout.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromPixels(32.0));
        layout.ColumnDefinitions().Append(ColumnDefinition{});
        layout.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(
            1.0, GridUnitType::Star));

        Border avatar;
        avatar.Width(30.0);
        avatar.Height(30.0);
        avatar.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 15.0 });
        avatar.VerticalAlignment(VerticalAlignment::Top);
        if (!avatarUrl.empty())
        {
            try
            {
                BitmapImage bitmap;
                bitmap.UriSource(Windows::Foundation::Uri{ winrt::hstring{ avatarUrl } });
                ImageBrush imageBrush;
                imageBrush.ImageSource(bitmap);
                imageBrush.Stretch(Stretch::UniformToFill);
                avatar.Background(imageBrush);
            }
            catch (...)
            {
            }
        }

        StackPanel body;
        body.Spacing(3.0);
        Grid::SetColumn(body, 1);

        StackPanel authorLine;
        authorLine.Orientation(Orientation::Horizontal);
        authorLine.Spacing(6.0);

        TextBlock authorText;
        authorText.Text(winrt::hstring{
            author.empty()
                ? CommentsString(L"YouTubeCommentsUnknownAuthor", L"Usuário")
                : author });
        authorText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        authorText.FontSize(11.5);
        authorText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        authorText.TextTrimming(TextTrimming::CharacterEllipsis);
        authorLine.Children().Append(authorText);

        std::wstring role;
        if (isOwner)
            role = CommentsString(L"YouTubeLiveChatOwner", L"Canal");
        else if (isModerator)
            role = CommentsString(L"YouTubeLiveChatModerator", L"Moderador");
        else if (isMember)
            role = CommentsString(L"YouTubeLiveChatMember", L"Membro");

        if (!role.empty())
        {
            TextBlock roleText;
            roleText.Text(winrt::hstring{ role });
            roleText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            roleText.FontSize(10.0);
            roleText.Opacity(0.55);
            roleText.VerticalAlignment(VerticalAlignment::Center);
            authorLine.Children().Append(roleText);
        }

        if (!amount.empty())
        {
            TextBlock amountText;
            amountText.Text(winrt::hstring{ amount });
            amountText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            amountText.FontSize(11.0);
            amountText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            body.Children().Append(authorLine);
            body.Children().Append(amountText);
        }
        else
        {
            body.Children().Append(authorLine);
        }

        TextBlock messageText;
        messageText.Text(winrt::hstring{ text });
        messageText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        messageText.FontSize(12.5);
        messageText.TextWrapping(TextWrapping::WrapWholeWords);
        messageText.IsTextSelectionEnabled(true);
        body.Children().Append(messageText);

        layout.Children().Append(avatar);
        layout.Children().Append(body);
        card.Child(layout);
        return card;
    }

    Microsoft::UI::Xaml::Controls::Grid YouTubeCommentsPage::CreateReplyRow(
        std::wstring const& author,
        std::wstring const& avatarUrl,
        std::wstring const& text,
        int64_t likeCount,
        std::wstring const& publishedAt,
        std::wstring const& updatedAt)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Imaging;

        Grid row;
        row.Margin(Thickness{ 5.0, 2.0, 0.0, 2.0 });
        row.ColumnSpacing(9.0);
        row.ColumnDefinitions().Append(ColumnDefinition{});
        row.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromPixels(29.0));
        row.ColumnDefinitions().Append(ColumnDefinition{});
        row.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(
            1.0, GridUnitType::Star));

        Border avatar;
        avatar.Width(27.0);
        avatar.Height(27.0);
        avatar.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 13.5 });
        avatar.VerticalAlignment(VerticalAlignment::Top);
        if (!avatarUrl.empty())
        {
            try
            {
                BitmapImage bitmap;
                bitmap.UriSource(Windows::Foundation::Uri{ winrt::hstring{ avatarUrl } });
                ImageBrush imageBrush;
                imageBrush.ImageSource(bitmap);
                imageBrush.Stretch(Stretch::UniformToFill);
                avatar.Background(imageBrush);
            }
            catch (...)
            {
            }
        }

        StackPanel body;
        body.Spacing(3.0);
        Grid::SetColumn(body, 1);

        std::wstring authorLine = author.empty()
            ? CommentsString(L"YouTubeCommentsUnknownAuthor", L"Usuário")
            : author;
        auto date = CommentPublishedTime(publishedAt, updatedAt);

        Grid authorHeader;
        authorHeader.ColumnDefinitions().Append(ColumnDefinition{});
        authorHeader.ColumnDefinitions().Append(ColumnDefinition{});
        authorHeader.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(
            1.0, GridUnitType::Star));
        authorHeader.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(
            0.0, GridUnitType::Auto));

        TextBlock authorText;
        authorText.Text(winrt::hstring{ authorLine });
        authorText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        authorText.FontSize(11.5);
        authorText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        authorText.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetColumn(authorText, 0);
        authorHeader.Children().Append(authorText);

        if (!date.empty())
        {
            TextBlock dateText;
            dateText.Text(winrt::hstring{ date });
            dateText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            dateText.FontSize(10.0);
            dateText.FontWeight(Windows::UI::Text::FontWeights::Normal());
            dateText.Opacity(0.70);
            dateText.Margin(Thickness{ 8.0, 1.0, 0.0, 0.0 });
            dateText.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(dateText, 1);
            authorHeader.Children().Append(dateText);
        }

        TextBlock replyText;
        replyText.Text(winrt::hstring{ text });
        replyText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        replyText.FontSize(12.5);
        replyText.TextWrapping(TextWrapping::WrapWholeWords);
        replyText.IsTextSelectionEnabled(true);

        std::wstring meta;
        if (likeCount > 0)
        {
            meta = CommentsString(L"YouTubeCommentsLikesPrefix", L"Curtidas") +
                L": " + std::to_wstring(likeCount);
        }
        body.Children().Append(authorHeader);
        body.Children().Append(replyText);
        if (!meta.empty())
        {
            TextBlock metaText;
            metaText.Text(winrt::hstring{ meta });
            metaText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            metaText.FontSize(10.5);
            metaText.Opacity(0.56);
            body.Children().Append(metaText);
        }

        row.Children().Append(avatar);
        row.Children().Append(body);
        return row;
    }

    winrt::fire_and_forget YouTubeCommentsPage::LoadRepliesAsync(
        std::wstring parentCommentId,
        Microsoft::UI::Xaml::Controls::StackPanel repliesHost,
        Microsoft::UI::Xaml::Controls::Button toggleButton,
        int64_t replyCount)
    {
        auto lifetime = get_strong();
        std::uint64_t const generation = m_requestGeneration;

        ++m_replyRequestsInFlight;
        struct ReplyRequestReset
        {
            std::uint32_t& count;
            ~ReplyRequestReset()
            {
                if (count > 0) --count;
            }
        } replyReset{ m_replyRequestsInFlight };

        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        toggleButton.IsEnabled(false);
        toggleButton.Content(box_value(winrt::hstring{
            CommentsString(L"YouTubeCommentsLoadingReplies", L"Carregando respostas…") }));
        repliesHost.Children().Clear();

        std::wstring const apiKey = SavedUiValue(L"ui-youtube-api-key");
        if (apiKey.empty() || parentCommentId.empty())
        {
            toggleButton.IsEnabled(true);
            co_return;
        }

        try
        {
            using namespace Windows::Data::Json;
            using namespace Windows::Foundation;
            using namespace Windows::Web::Http;

            std::wstring url =
                L"https://www.googleapis.com/youtube/v3/comments"
                L"?part=snippet&maxResults=50&textFormat=plainText&parentId=" +
                UrlEncode(parentCommentId) + L"&key=" + UrlEncode(apiKey);

            HttpClient client{};
            HttpResponseMessage response = co_await client.GetAsync(
                Uri{ winrt::hstring{ url } });
            winrt::hstring bodyText = co_await response.Content().ReadAsStringAsync();

            if (generation != m_requestGeneration || m_closing) co_return;

            if (!response.IsSuccessStatusCode())
            {
                TextBlock errorText;
                errorText.Text(winrt::hstring{ YouTubeApiError(bodyText.c_str()) });
                errorText.FontSize(11.5);
                errorText.Opacity(0.62);
                errorText.TextWrapping(TextWrapping::Wrap);
                repliesHost.Children().Append(errorText);
                repliesHost.Visibility(Visibility::Visible);
                toggleButton.IsEnabled(true);
                toggleButton.Content(box_value(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRetryReplies",
                        L"Tentar carregar respostas novamente") }));
                co_return;
            }

            JsonObject root = JsonObject::Parse(bodyText);
            std::wstring const nextPageToken =
                root.GetNamedString(L"nextPageToken", L"").c_str();
            uint32_t added{};

            if (root.HasKey(L"items"))
            {
                JsonArray items = root.GetNamedArray(L"items");
                for (uint32_t index = 0; index < items.Size(); ++index)
                {
                    try
                    {
                        JsonObject item = items.GetObjectAt(index);
                        JsonObject snippet = item.GetNamedObject(L"snippet");
                        std::wstring author = snippet.GetNamedString(
                            L"authorDisplayName", L"").c_str();
                        std::wstring avatar = snippet.GetNamedString(
                            L"authorProfileImageUrl", L"").c_str();
                        std::wstring text = snippet.GetNamedString(
                            L"textDisplay", L"").c_str();
                        int64_t const likes = static_cast<int64_t>(
                            snippet.GetNamedNumber(L"likeCount", 0));
                        std::wstring published = snippet.GetNamedString(
                            L"publishedAt", L"").c_str();
                        std::wstring updated = snippet.GetNamedString(
                            L"updatedAt", L"").c_str();

                        if (text.empty()) continue;
                        repliesHost.Children().Append(CreateReplyRow(
                            author, avatar, text, likes, published, updated));
                        ++added;
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (added == 0)
            {
                TextBlock emptyText;
                emptyText.Text(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRepliesUnavailable",
                        L"As respostas não estão disponíveis agora") });
                emptyText.FontSize(11.5);
                emptyText.Opacity(0.60);
                repliesHost.Children().Append(emptyText);
            }

            if (!nextPageToken.empty())
            {
                Button moreButton;
                moreButton.HorizontalAlignment(HorizontalAlignment::Left);
                moreButton.Padding(Thickness{ 10.0, 5.0, 10.0, 5.0 });
                moreButton.Margin(Thickness{ 0.0, 2.0, 0.0, 0.0 });
                moreButton.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{
                    L"Segoe UI Variable Text" });
                moreButton.FontSize(11.0);
                moreButton.Tag(box_value(winrt::hstring{ nextPageToken }));

                std::wstring const moreLabel = CommentsString(
                    L"YouTubeCommentsLoadMoreReplies",
                    L"Carregar mais respostas");
                moreButton.Content(box_value(winrt::hstring{ moreLabel }));

                moreButton.Click(
                    [this, parentCommentId, repliesHost, moreButton](auto&&, auto&&)
                    {
                        LoadMoreRepliesAsync(
                            parentCommentId, repliesHost, moreButton);
                    });

                repliesHost.Children().Append(moreButton);
            }

            repliesHost.Visibility(Visibility::Visible);
            toggleButton.Tag(box_value(true));
            toggleButton.Content(box_value(winrt::hstring{
                CommentsString(L"YouTubeCommentsHideReplies", L"Ocultar respostas") }));
            toggleButton.IsEnabled(true);
        }
        catch (...)
        {
            if (generation == m_requestGeneration && !m_closing)
            {
                repliesHost.Children().Clear();
                TextBlock errorText;
                errorText.Text(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRepliesNetworkError",
                        L"Não foi possível carregar as respostas agora") });
                errorText.FontSize(11.5);
                errorText.Opacity(0.62);
                errorText.TextWrapping(TextWrapping::Wrap);
                repliesHost.Children().Append(errorText);
                repliesHost.Visibility(Visibility::Visible);
                toggleButton.Content(box_value(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRetryReplies",
                        L"Tentar carregar respostas novamente") }));
                toggleButton.IsEnabled(true);
            }
        }
    }

    winrt::fire_and_forget YouTubeCommentsPage::LoadMoreRepliesAsync(
        std::wstring parentCommentId,
        Microsoft::UI::Xaml::Controls::StackPanel repliesHost,
        Microsoft::UI::Xaml::Controls::Button moreButton)
    {
        auto lifetime = get_strong();
        std::uint64_t const generation = m_requestGeneration;

        ++m_replyRequestsInFlight;
        struct ReplyRequestReset
        {
            std::uint32_t& count;
            ~ReplyRequestReset()
            {
                if (count > 0) --count;
            }
        } replyReset{ m_replyRequestsInFlight };

        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        std::wstring pageToken;
        try
        {
            pageToken = unbox_value<winrt::hstring>(moreButton.Tag()).c_str();
        }
        catch (...)
        {
        }

        std::wstring const apiKey = SavedUiValue(L"ui-youtube-api-key");
        if (apiKey.empty() || parentCommentId.empty() || pageToken.empty())
        {
            co_return;
        }

        std::wstring const moreLabel = CommentsString(
            L"YouTubeCommentsLoadMoreReplies",
            L"Carregar mais respostas");

        moreButton.IsEnabled(false);
        moreButton.Content(box_value(winrt::hstring{
            CommentsString(L"YouTubeCommentsLoadingReplies", L"Carregando respostas…") }));

        try
        {
            using namespace Windows::Data::Json;
            using namespace Windows::Foundation;
            using namespace Windows::Web::Http;

            std::wstring url =
                L"https://www.googleapis.com/youtube/v3/comments"
                L"?part=snippet&maxResults=50&textFormat=plainText&parentId=" +
                UrlEncode(parentCommentId) + L"&key=" + UrlEncode(apiKey) +
                L"&pageToken=" + UrlEncode(pageToken);

            HttpClient client{};
            HttpResponseMessage response = co_await client.GetAsync(
                Uri{ winrt::hstring{ url } });
            winrt::hstring bodyText = co_await response.Content().ReadAsStringAsync();

            if (generation != m_requestGeneration || m_closing) co_return;

            if (!response.IsSuccessStatusCode())
            {
                moreButton.Content(box_value(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRetryReplies",
                        L"Tentar carregar respostas novamente") }));
                moreButton.IsEnabled(true);
                co_return;
            }

            JsonObject root = JsonObject::Parse(bodyText);
            std::wstring const nextPageToken =
                root.GetNamedString(L"nextPageToken", L"").c_str();

            if (root.HasKey(L"items"))
            {
                JsonArray items = root.GetNamedArray(L"items");
                for (uint32_t index = 0; index < items.Size(); ++index)
                {
                    try
                    {
                        JsonObject item = items.GetObjectAt(index);
                        JsonObject snippet = item.GetNamedObject(L"snippet");
                        std::wstring author = snippet.GetNamedString(
                            L"authorDisplayName", L"").c_str();
                        std::wstring avatar = snippet.GetNamedString(
                            L"authorProfileImageUrl", L"").c_str();
                        std::wstring text = snippet.GetNamedString(
                            L"textDisplay", L"").c_str();
                        int64_t const likes = static_cast<int64_t>(
                            snippet.GetNamedNumber(L"likeCount", 0));
                        std::wstring published = snippet.GetNamedString(
                            L"publishedAt", L"").c_str();
                        std::wstring updated = snippet.GetNamedString(
                            L"updatedAt", L"").c_str();

                        if (text.empty()) continue;

                        auto const insertIndex = repliesHost.Children().Size() - 1;
                        repliesHost.Children().InsertAt(
                            insertIndex,
                            CreateReplyRow(
                                author, avatar, text, likes, published, updated));
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (nextPageToken.empty())
            {
                auto const count = repliesHost.Children().Size();
                if (count > 0)
                {
                    repliesHost.Children().RemoveAt(count - 1);
                }
            }
            else
            {
                moreButton.Tag(box_value(winrt::hstring{ nextPageToken }));
                moreButton.Content(box_value(winrt::hstring{ moreLabel }));
                moreButton.IsEnabled(true);
            }
        }
        catch (...)
        {
            if (generation == m_requestGeneration && !m_closing)
            {
                moreButton.Content(box_value(winrt::hstring{
                    CommentsString(
                        L"YouTubeCommentsRetryReplies",
                        L"Tentar carregar respostas novamente") }));
                moreButton.IsEnabled(true);
            }
        }
    }

    winrt::fire_and_forget YouTubeCommentsPage::DetectModeAndLoadAsync()
    {
        auto lifetime = get_strong();
        if (m_loading) co_return;
        m_loading = true;
        std::uint64_t const generation = m_requestGeneration;
        ShowLoading(true, false);
        LoadMoreButton().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);

        auto finishLoading = [this]()
        {
            m_loading = false;
            ShowLoading(false, false);
        };

        std::wstring const apiKey = SavedUiValue(L"ui-youtube-api-key");
        if (apiKey.empty())
        {
            finishLoading();
            ShowStatus(CommentsString(
                L"YouTubeCommentsMissingApiKey",
                L"Configure uma API key da YouTube Data API em Configurações > Mídia > YouTube"));
            co_return;
        }

        if (m_videoId.empty())
        {
            finishLoading();
            ShowStatus(CommentsString(
                L"YouTubeCommentsNoVideo",
                L"Abra um vídeo do YouTube para ver os comentários"));
            co_return;
        }

        std::wstring liveChatId;
        try
        {
            using namespace Windows::Data::Json;
            using namespace Windows::Foundation;
            using namespace Windows::Web::Http;

            std::wstring url =
                L"https://www.googleapis.com/youtube/v3/videos"
                L"?part=liveStreamingDetails&id=" + UrlEncode(m_videoId) +
                L"&key=" + UrlEncode(apiKey);

            HttpClient client{};
            HttpResponseMessage response = co_await client.GetAsync(
                Uri{ winrt::hstring{ url } });
            winrt::hstring bodyText = co_await response.Content().ReadAsStringAsync();

            if (generation != m_requestGeneration || m_closing)
            {
                finishLoading();
                co_return;
            }

            if (response.IsSuccessStatusCode())
            {
                JsonObject root = JsonObject::Parse(bodyText);
                if (root.HasKey(L"items"))
                {
                    JsonArray items = root.GetNamedArray(L"items");
                    if (items.Size() > 0)
                    {
                        JsonObject video = items.GetObjectAt(0);
                        if (video.HasKey(L"liveStreamingDetails"))
                        {
                            JsonObject details = video.GetNamedObject(L"liveStreamingDetails");
                            liveChatId = details.GetNamedString(
                                L"activeLiveChatId", L"").c_str();
                        }
                    }
                }
            }
        }
        catch (...)
        {
            // Live-mode detection is an enhancement. If it cannot be completed,
            // preserve the already working normal-comments path instead of
            // blocking the whole panel.
        }

        if (generation != m_requestGeneration || m_closing)
        {
            finishLoading();
            co_return;
        }

        if (!liveChatId.empty())
        {
            m_liveMode = true;
            m_liveChatId = liveChatId;
            m_liveNextPageToken.clear();
            m_nextPageToken.clear();
            SetLiveChatModeUi();
            CommentsHost().Children().Clear();
            StatusText().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
            finishLoading();
            PollLiveChatAsync(generation, liveChatId);
            co_return;
        }

        m_liveMode = false;
        m_liveChatId.clear();
        m_liveNextPageToken.clear();
        SetCommentsModeUi();
        finishLoading();
        LoadCommentsAsync(false);
    }

    winrt::fire_and_forget YouTubeCommentsPage::PollLiveChatAsync(
        std::uint64_t generation,
        std::wstring liveChatId)
    {
        auto lifetime = get_strong();
        winrt::apartment_context uiContext;

        std::wstring const apiKey = SavedUiValue(L"ui-youtube-api-key");
        if (apiKey.empty() || liveChatId.empty()) co_return;

        using namespace Microsoft::UI::Xaml;
        using namespace Windows::Data::Json;
        using namespace Windows::Foundation;
        using namespace Windows::Web::Http;

        HttpClient client{};
        std::wstring pageToken;
        bool firstRequest = true;

        while (generation == m_requestGeneration &&
               !m_closing && m_liveMode && m_liveChatId == liveChatId)
        {
            try
            {
                std::wstring url =
                    L"https://www.googleapis.com/youtube/v3/liveChat/messages"
                    L"?part=snippet,authorDetails&maxResults=200&profileImageSize=48"
                    L"&liveChatId=" + UrlEncode(liveChatId) +
                    L"&key=" + UrlEncode(apiKey);
                if (!pageToken.empty())
                {
                    url += L"&pageToken=" + UrlEncode(pageToken);
                }

                HttpResponseMessage response = co_await client.GetAsync(
                    Uri{ winrt::hstring{ url } });
                winrt::hstring bodyText = co_await response.Content().ReadAsStringAsync();

                if (generation != m_requestGeneration || m_closing ||
                    !m_liveMode || m_liveChatId != liveChatId)
                {
                    co_return;
                }

                if (!response.IsSuccessStatusCode())
                {
                    ShowStatus(YouTubeApiError(bodyText.c_str()));
                    co_return;
                }

                JsonObject root = JsonObject::Parse(bodyText);
                pageToken = root.GetNamedString(L"nextPageToken", L"").c_str();
                m_liveNextPageToken = pageToken;

                double pollingMs = root.GetNamedNumber(L"pollingIntervalMillis", 5000.0);
                if (pollingMs < 1000.0) pollingMs = 5000.0;

                bool const nearBottom = firstRequest ||
                    (CommentsScrollViewer().ScrollableHeight() -
                        CommentsScrollViewer().VerticalOffset() < 120.0);

                uint32_t added{};
                bool chatEnded{};
                if (root.HasKey(L"items"))
                {
                    JsonArray items = root.GetNamedArray(L"items");
                    for (uint32_t index = 0; index < items.Size(); ++index)
                    {
                        try
                        {
                            JsonObject item = items.GetObjectAt(index);
                            JsonObject snippet = item.GetNamedObject(L"snippet");
                            std::wstring type = snippet.GetNamedString(L"type", L"").c_str();
                            if (type == L"chatEndedEvent")
                            {
                                chatEnded = true;
                                continue;
                            }

                            std::wstring message = snippet.GetNamedString(
                                L"displayMessage", L"").c_str();
                            if (message.empty()) continue;

                            std::wstring author;
                            std::wstring avatar;
                            bool isOwner{};
                            bool isModerator{};
                            bool isMember{};
                            if (item.HasKey(L"authorDetails"))
                            {
                                JsonObject details = item.GetNamedObject(L"authorDetails");
                                author = details.GetNamedString(L"displayName", L"").c_str();
                                avatar = details.GetNamedString(L"profileImageUrl", L"").c_str();
                                isOwner = details.GetNamedBoolean(L"isChatOwner", false);
                                isModerator = details.GetNamedBoolean(L"isChatModerator", false);
                                isMember = details.GetNamedBoolean(L"isChatSponsor", false);
                            }

                            std::wstring amount;
                            if (snippet.HasKey(L"superChatDetails"))
                            {
                                amount = snippet.GetNamedObject(L"superChatDetails")
                                    .GetNamedString(L"amountDisplayString", L"").c_str();
                            }
                            else if (snippet.HasKey(L"superStickerDetails"))
                            {
                                amount = snippet.GetNamedObject(L"superStickerDetails")
                                    .GetNamedString(L"amountDisplayString", L"").c_str();
                            }

                            CommentsHost().Children().Append(CreateLiveChatCard(
                                author, avatar, message, amount,
                                isOwner, isModerator, isMember));
                            ++added;

                            // Keep long-running livestreams bounded. The panel is
                            // a view of the recent chat, not an unbounded archive.
                            while (CommentsHost().Children().Size() > 350)
                            {
                                CommentsHost().Children().RemoveAt(0);
                            }
                        }
                        catch (...)
                        {
                        }
                    }
                }

                if (added > 0)
                {
                    StatusText().Visibility(Visibility::Collapsed);
                    if (nearBottom)
                    {
                        CommentsScrollViewer().UpdateLayout();
                        CommentsScrollViewer().ChangeView(
                            nullptr,
                            CommentsScrollViewer().ScrollableHeight(),
                            nullptr,
                            true);
                    }
                }
                else if (firstRequest)
                {
                    ShowStatus(CommentsString(
                        L"YouTubeLiveChatWaiting",
                        L"Aguardando novas mensagens do chat…"));
                }

                firstRequest = false;

                if (chatEnded || root.HasKey(L"offlineAt"))
                {
                    ShowStatus(CommentsString(
                        L"YouTubeLiveChatEnded",
                        L"A transmissão ao vivo terminou"));
                    co_return;
                }

                co_await winrt::resume_after(std::chrono::milliseconds(
                    static_cast<int64_t>(pollingMs)));
                co_await uiContext;
            }
            catch (winrt::hresult_error const& error)
            {
                if (generation == m_requestGeneration && !m_closing && m_liveMode)
                {
                    std::wstring message = error.message().c_str();
                    if (message.empty())
                    {
                        message = CommentsString(
                            L"YouTubeLiveChatNetworkError",
                            L"Não foi possível acessar o chat ao vivo agora");
                    }
                    ShowStatus(message);
                }
                co_return;
            }
            catch (...)
            {
                if (generation == m_requestGeneration && !m_closing && m_liveMode)
                {
                    ShowStatus(CommentsString(
                        L"YouTubeLiveChatNetworkError",
                        L"Não foi possível acessar o chat ao vivo agora"));
                }
                co_return;
            }
        }
    }

    winrt::fire_and_forget YouTubeCommentsPage::LoadCommentsAsync(bool append)
    {
        auto lifetime = get_strong();
        if (m_loading) co_return;
        m_loading = true;
        std::uint64_t const generation = m_requestGeneration;
        struct LoadingReset
        {
            YouTubeCommentsPage* page{};
            bool append{};
            ~LoadingReset()
            {
                if (page)
                {
                    page->m_loading = false;
                    page->ShowLoading(false, append);
                }
            }
        } reset{ this, append };

        ShowLoading(true, append);
        LoadMoreButton().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);

        std::wstring const apiKey = SavedUiValue(L"ui-youtube-api-key");
        if (apiKey.empty())
        {
            ShowStatus(CommentsString(
                L"YouTubeCommentsMissingApiKey",
                L"Configure uma API key da YouTube Data API em Configurações > Mídia > YouTube"));
            co_return;
        }

        if (m_videoId.empty())
        {
            ShowStatus(CommentsString(
                L"YouTubeCommentsNoVideo",
                L"Abra um vídeo do YouTube para ver os comentários"));
            co_return;
        }

        try
        {
            using namespace Windows::Data::Json;
            using namespace Windows::Foundation;
            using namespace Windows::Web::Http;

            std::wstring url =
                L"https://www.googleapis.com/youtube/v3/commentThreads"
                L"?part=snippet&maxResults=30&order=" + m_order +
                L"&textFormat=plainText&videoId=" +
                UrlEncode(m_videoId) + L"&key=" + UrlEncode(apiKey);
            if (append && !m_nextPageToken.empty())
            {
                url += L"&pageToken=" + UrlEncode(m_nextPageToken);
            }

            HttpClient client{};
            HttpResponseMessage response = co_await client.GetAsync(
                Uri{ winrt::hstring{ url } });
            winrt::hstring bodyText = co_await response.Content().ReadAsStringAsync();

            if (generation != m_requestGeneration || m_closing) co_return;

            if (!response.IsSuccessStatusCode())
            {
                ShowStatus(YouTubeApiError(bodyText.c_str()));
                co_return;
            }

            JsonObject root = JsonObject::Parse(bodyText);
            m_nextPageToken = root.GetNamedString(L"nextPageToken", L"").c_str();
            if (!root.HasKey(L"items"))
            {
                ShowStatus(CommentsString(
                    L"YouTubeCommentsEmpty",
                    L"Nenhum comentário público foi encontrado para este vídeo"));
                co_return;
            }
            JsonArray items = root.GetNamedArray(L"items");

            uint32_t added{};
            for (uint32_t index = 0; index < items.Size(); ++index)
            {
                try
                {
                    JsonObject item = items.GetObjectAt(index);
                    JsonObject threadSnippet = item.GetNamedObject(L"snippet");
                    int64_t const replyCount = static_cast<int64_t>(
                        threadSnippet.GetNamedNumber(L"totalReplyCount", 0));
                    JsonObject topLevel = threadSnippet.GetNamedObject(L"topLevelComment");
                    std::wstring commentId = topLevel.GetNamedString(L"id", L"").c_str();
                    JsonObject snippet = topLevel.GetNamedObject(L"snippet");

                    std::wstring author = snippet.GetNamedString(
                        L"authorDisplayName", L"").c_str();
                    std::wstring avatar = snippet.GetNamedString(
                        L"authorProfileImageUrl", L"").c_str();
                    std::wstring text = snippet.GetNamedString(
                        L"textDisplay", L"").c_str();
                    int64_t const likes = static_cast<int64_t>(
                        snippet.GetNamedNumber(L"likeCount", 0));
                    std::wstring published = snippet.GetNamedString(
                        L"publishedAt", L"").c_str();
                    std::wstring updated = snippet.GetNamedString(
                        L"updatedAt", L"").c_str();

                    if (text.empty()) continue;
                    CommentsHost().Children().Append(CreateCommentCard(
                        commentId, author, avatar, text, likes, replyCount,
                        published, updated));
                    ++added;
                }
                catch (...)
                {
                }
            }

            if (!append && added == 0)
            {
                ShowStatus(CommentsString(
                    L"YouTubeCommentsEmpty",
                    L"Nenhum comentário público foi encontrado para este vídeo"));
            }
            else
            {
                StatusText().Visibility(
                    Microsoft::UI::Xaml::Visibility::Collapsed);
            }

            LoadMoreButton().Visibility(
                !m_nextPageToken.empty()
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        catch (winrt::hresult_error const& error)
        {
            if (generation == m_requestGeneration && !m_closing)
            {
                std::wstring message = error.message().c_str();
                if (message.empty())
                {
                    message = CommentsString(
                        L"YouTubeCommentsNetworkError",
                        L"Não foi possível acessar os comentários do YouTube agora");
                }
                ShowStatus(message);
            }
        }
        catch (...)
        {
            if (generation == m_requestGeneration && !m_closing)
            {
                ShowStatus(CommentsString(
                    L"YouTubeCommentsNetworkError",
                    L"Não foi possível acessar os comentários do YouTube agora"));
            }
        }
    }
}
