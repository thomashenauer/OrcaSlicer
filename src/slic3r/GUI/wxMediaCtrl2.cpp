#include "wxMediaCtrl2.h"
#include "libslic3r/Time.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include <boost/filesystem/operations.hpp>
#ifdef __WIN32__
#include <winuser.h>
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

#ifdef __LINUX__
#include "Printer/gstbambusrc.h"
#include <gst/gst.h> // main gstreamer header
#include <gst/base/gstbasesink.h>
class WXDLLIMPEXP_MEDIA
    wxGStreamerMediaBackend : public wxMediaBackendCommonBase
{
public:
    GstElement *m_playbin; // GStreamer media element
};

static bool has_gobject_property(GObject *obj, const char *property_name)
{
    if (!obj || !property_name)
        return false;
    return g_object_class_find_property(G_OBJECT_GET_CLASS(obj), property_name) != nullptr;
}

static void set_bool_property_if_present(GObject *obj, const char *property_name, gboolean value)
{
    if (has_gobject_property(obj, property_name))
        g_object_set(obj, property_name, value, NULL);
}

static void set_int64_property_if_present(GObject *obj, const char *property_name, gint64 value)
{
    if (has_gobject_property(obj, property_name))
        g_object_set(obj, property_name, value, NULL);
}

static void tune_live_preview_sink(GstElement *element)
{
    if (!element)
        return;

    GObject *obj = G_OBJECT(element);
    // Live preview should not accumulate lateness or drop frames for A/V sync.
    set_bool_property_if_present(obj, "sync", FALSE);
    set_bool_property_if_present(obj, "qos", FALSE);
    set_int64_property_if_present(obj, "max-lateness", -1);
}

static bool has_avdec_h264_decoder()
{
    static const bool has_decoder = []() {
        GstElementFactory *factory = gst_element_factory_find("avdec_h264");
        if (!factory)
            return false;
        gst_object_unref(factory);
        return true;
    }();
    return has_decoder;
}

static bool is_bambu_uri_for_playbin(GstElement *playbin)
{
    if (!playbin)
        return false;
    static GQuark is_bambu_quark = 0;
    if (is_bambu_quark == 0)
        is_bambu_quark = g_quark_from_static_string("orca-is-bambu-uri");
    if (g_object_get_qdata(G_OBJECT(playbin), is_bambu_quark) != nullptr)
        return true;

    gchar *uri = nullptr;
    g_object_get(G_OBJECT(playbin), "uri", &uri, NULL);
    const bool is_bambu = (uri != nullptr) && g_str_has_prefix(uri, "bambu://");
    g_free(uri);
    return is_bambu;
}

static gint on_uridecodebin_autoplug_select(GstElement * /*decodebin*/, GstPad * /*pad*/, GstCaps * /*caps*/, GstElementFactory *factory, gpointer user_data)
{
    auto *playbin = GST_ELEMENT(user_data);
    if (!factory)
        return 0; // GST_AUTOPLUG_SELECT_TRY

    const gchar *factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    const bool is_bambu = is_bambu_uri_for_playbin(playbin);
    if (!is_bambu)
        return 0; // GST_AUTOPLUG_SELECT_TRY
    if (!factory_name)
        return 0;

    // Only skip NVIDIA HW decode if a robust software fallback (libav) exists in this runtime.
    const bool can_fallback_to_sw = has_avdec_h264_decoder();

    // For Bambu live preview, skip NVIDIA HW decoders to avoid CUDAMemory-only output negotiation.
    if (g_strcmp0(factory_name, "nvh264dec") == 0 || g_strcmp0(factory_name, "nvh265dec") == 0) {
        if (!can_fallback_to_sw)
            return 0; // Keep stream working rather than black video.
        return 2; // GST_AUTOPLUG_SELECT_SKIP
    }

    return 0;
}

static void maybe_attach_bambu_decoder_filter(GstElement *playbin, GstElement *element)
{
    if (!playbin || !element)
        return;

    auto *factory = gst_element_get_factory(element);
    if (!factory)
        return;
    const gchar *factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (!factory_name)
        return;
    const bool is_uri_decodebin = g_str_has_prefix(factory_name, "uridecodebin");
    const bool is_decodebin = g_str_has_prefix(factory_name, "decodebin");
    if (!is_uri_decodebin && !is_decodebin)
        return;

    static GQuark attached_quark = 0;
    if (attached_quark == 0)
        attached_quark = g_quark_from_static_string("orca-bambu-autoplug-filter-attached");
    if (g_object_get_qdata(G_OBJECT(element), attached_quark) != nullptr)
        return;

    g_signal_connect(element, "autoplug-select", G_CALLBACK(on_uridecodebin_autoplug_select), playbin);
    g_object_set_qdata(G_OBJECT(element), attached_quark, GINT_TO_POINTER(1));
}

static void on_playbin_deep_element_added(GstBin *playbin, GstBin * /*sub_bin*/, GstElement *element, gpointer /*user_data*/)
{
    auto *playbin_element = GST_ELEMENT(playbin);
    if (!is_bambu_uri_for_playbin(playbin_element))
        return;

    if (GST_IS_BASE_SINK(element))
        tune_live_preview_sink(element);
    maybe_attach_bambu_decoder_filter(playbin_element, element);
}
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

wxMediaCtrl2::wxMediaCtrl2(wxWindow *parent)
{
#ifdef __WIN32__
    auto hModExe = GetModuleHandle(NULL);
    // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: GetModuleHandle " << hModExe;
    auto NvOptimusEnablement = (DWORD *) GetProcAddress(hModExe, "NvOptimusEnablement");
    auto AmdPowerXpressRequestHighPerformance = (int *) GetProcAddress(hModExe, "AmdPowerXpressRequestHighPerformance");
    if (NvOptimusEnablement) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: NvOptimusEnablement " << *NvOptimusEnablement;
        *NvOptimusEnablement = 0;
    }
    if (AmdPowerXpressRequestHighPerformance) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: AmdPowerXpressRequestHighPerformance " << *AmdPowerXpressRequestHighPerformance;
        *AmdPowerXpressRequestHighPerformance = 0;
    }
#endif
    wxMediaCtrl::Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxMEDIACTRLPLAYERCONTROLS_NONE);
#ifdef __LINUX__
    /* Register only after we have created the wxMediaCtrl, since only then are we guaranteed to have fired up Gstreamer's plugin registry. */
    auto playbin = reinterpret_cast<wxGStreamerMediaBackend *>(m_imp)->m_playbin;
    g_object_set (G_OBJECT (playbin),
                  "audio-sink", NULL,
                   NULL);
    g_signal_connect(playbin, "deep-element-added", G_CALLBACK(on_playbin_deep_element_added), nullptr);
    gstbambusrc_register();
    Bind(wxEVT_MEDIA_LOADED, [this](auto & e) {
        m_loaded = true;
    });
#endif
}

#define CLSID_BAMBU_SOURCE L"{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}"

void wxMediaCtrl2::Load(wxURI url)
{
#ifdef __WIN32__
    InvalidateBestSize();
    if (m_imp == nullptr) {
        static bool notified = false;
        if (!notified) CallAfter([] {
            auto res = wxMessageBox(_L("Windows Media Player is required for this task! Do you want to enable 'Windows Media Player' for your operation system?"), _L("Error"), wxOK | wxCANCEL);
            if (res == wxOK) {
                wxString url = IsWindows10OrGreater() 
                        ? "ms-settings:optionalfeatures?activationSource=SMC-Article-14209" 
                        : "https://support.microsoft.com/en-au/windows/get-windows-media-player-81718e0d-cfce-25b1-aee3-94596b658287";
                wxExecute("cmd /c start " + url, wxEXEC_HIDE_CONSOLE);
            }
        });
        m_error = 100;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    {
        wxRegKey key11(wxRegKey::HKCU, L"SOFTWARE\\Classes\\CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxRegKey key12(wxRegKey::HKCR, L"CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxString path = key11.Exists() ? key11.QueryDefaultValue() 
                                       : key12.Exists() ? key12.QueryDefaultValue() : wxString{};
        wxRegKey key2(wxRegKey::HKCR, "bambu");
        wxString clsid;
        if (key2.Exists())
            key2.QueryRawValue("Source Filter", clsid);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": clsid %1% path %2%") % clsid % path;

        std::string             data_dir_str = Slic3r::data_dir();
        boost::filesystem::path data_dir_path(data_dir_str);
        auto                    dll_path = data_dir_path / "plugins" / "BambuSource.dll";
        if (path.empty() || !wxFile::Exists(path) || clsid != CLSID_BAMBU_SOURCE) {
            if (boost::filesystem::exists(dll_path)) {
                CallAfter(
                    [dll_path] {
                    int res = wxMessageBox(_L("BambuSource has not correctly been registered for media playing! Press Yes to re-register it. You will be promoted twice"), _L("Error"), wxYES_NO);
                    if (res == wxYES) {
                        std::string regContent = R"(Windows Registry Editor Version 5.00
                                                    [HKEY_CLASSES_ROOT\bambu]
                                                    "Source Filter"="{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}"
                                                    )";

                        auto reg_path = (fs::temp_directory_path() / fs::unique_path()).replace_extension(".reg");
                        std::ofstream temp_reg_file(reg_path.c_str());
                        if (!temp_reg_file) {
                            return false;
                        }
                        temp_reg_file << regContent;
                        temp_reg_file.close();
                        auto sei_params = L"/q /s " + reg_path.wstring();
                        SHELLEXECUTEINFO sei{sizeof(sei), SEE_MASK_NOCLOSEPROCESS, NULL,   L"open",
                                             L"regedit",  sei_params.c_str(),SW_HIDE,SW_HIDE};
                        ::ShellExecuteEx(&sei);

                        wstring quoted_dll_path = L"\"" + dll_path.wstring() + L"\"";
                        SHELLEXECUTEINFO info{sizeof(info), 0, NULL, L"runas", L"regsvr32", quoted_dll_path.c_str(), SW_HIDE };
                        ::ShellExecuteEx(&info);
                        fs::remove(reg_path);
                    }
                    return true;
                });
            } else {
                CallAfter([] {
                    wxMessageBox(_L("Missing BambuSource component registered for media playing! Please re-install OrcaSlicer or seek community help."), _L("Error"), wxOK);
                });
            }
            m_error = clsid != CLSID_BAMBU_SOURCE ? 101 : path.empty() ? 102 : 103;
            wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
            event.SetId(GetId());
            event.SetEventObject(this);
            wxPostEvent(this, event);
            return;
        }
        if (path != dll_path) {
            static bool notified = false;
            if (!notified) CallAfter([dll_path] {
                int res = wxMessageBox(_L("Using a BambuSource from a different install, video play may not work correctly! Press Yes to fix it."), _L("Warning"), wxYES_NO | wxICON_WARNING);
                if (res == wxYES) {
                    auto path = dll_path.wstring();
                    if (path.find(L' ') != std::wstring::npos)
                        path = L"\"" + path + L"\"";
                    SHELLEXECUTEINFO info{sizeof(info), 0, NULL, L"open", L"regsvr32", path.c_str(), SW_HIDE};
                    ::ShellExecuteEx(&info);
                }
            });
            notified = true;
        }
        wxRegKey keyWmp(wxRegKey::HKCU, "SOFTWARE\\Microsoft\\MediaPlayer\\Player\\Extensions\\.");
        keyWmp.Create();
        long permissions = 0;
        if (keyWmp.HasValue("Permissions"))
            keyWmp.QueryValue("Permissions", &permissions);
        if ((permissions & 32) == 0) {
            permissions |= 32;
            keyWmp.SetValue("Permissions", permissions);
        }
    }
    url = wxURI(url.BuildURI().append("&hwnd=").append(boost::lexical_cast<std::string>(GetHandle())).append("&tid=").append(
        boost::lexical_cast<std::string>(GetCurrentThreadId())));
#endif
#ifdef __WXGTK3__
    if (m_imp != nullptr) {
        auto playbin = reinterpret_cast<wxGStreamerMediaBackend *>(m_imp)->m_playbin;
        if (playbin) {
            static GQuark is_bambu_quark = 0;
            if (is_bambu_quark == 0)
                is_bambu_quark = g_quark_from_static_string("orca-is-bambu-uri");
            const bool is_bambu = (url.GetScheme() == "bambu");
            g_object_set_qdata(G_OBJECT(playbin), is_bambu_quark, is_bambu ? GINT_TO_POINTER(1) : nullptr);
        }
    }

    GstElementFactory *factory;
    int hasplugins = 1;
    
    factory = gst_element_factory_find("h264parse");
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    factory = gst_element_factory_find("openh264dec");
    if (!factory) {
        factory = gst_element_factory_find("avdec_h264");
    }
    if (!factory) {
        factory = gst_element_factory_find("vaapih264dec");
    }
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    if (!hasplugins) {
        CallAfter([] {
            wxMessageBox(_L("Your system is missing H.264 codecs for GStreamer, which are required to play video. (Try installing the gstreamer1.0-plugins-bad or gstreamer1.0-libav packages, then restart Orca Slicer?)"), _L("Error"), wxOK);
        });
        m_error = 101;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    wxLog::EnableLogging(false);
#endif
    m_error = 0;
    m_loaded = false;
    wxMediaCtrl::Load(url);

#ifdef __WXGTK3__
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
#endif
}

void wxMediaCtrl2::Play() { wxMediaCtrl::Play(); }

void wxMediaCtrl2::Stop()
{
    wxMediaCtrl::Stop();
}

#ifdef __LINUX__
extern "C" int gst_bambu_last_error;
#endif

int wxMediaCtrl2::GetLastError() const
{
#ifdef __LINUX__
    return gst_bambu_last_error;
#else
    return m_error;
#endif
}

wxSize wxMediaCtrl2::GetVideoSize() const
{
#ifdef __LINUX__
    // Gstreamer doesn't give us a VideoSize until we're playing, which
    // confuses the MediaPlayCtrl into claiming that it is stuck
    // "Loading...".  Fake it out for now.
    return m_loaded ? wxSize(1280, 720) : wxSize{};
#else
    wxSize size = m_imp ? m_imp->GetVideoSize() : wxSize(0, 0);
    if (size.GetWidth() > 0)
        const_cast<wxSize&>(m_video_size) = size;
    return size;
#endif
}

wxSize wxMediaCtrl2::DoGetBestSize() const
{
    return {-1, -1};
}

#ifdef __WIN32__

WXLRESULT wxMediaCtrl2::MSWWindowProc(WXUINT   nMsg,
                                   WXWPARAM wParam,
                                   WXLPARAM lParam)
{
    if (nMsg == WM_USER + 1000) {
        wxString msg((wchar_t const *) lParam);
        if (wParam == 1) {
            if (msg.EndsWith("]")) {
                int n = msg.find_last_of('[');
                if (n != wxString::npos) {
                    long val = 0;
                    if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val))
                        m_error = (int) val;
                }
            } else if (msg.Contains("stat_log")) {
                wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
                evt.SetEventObject(this);
                evt.SetString(msg.Mid(msg.Find(' ') + 1));
                wxPostEvent(this, evt);
            }
        }
        BOOST_LOG_TRIVIAL(trace) << msg.ToUTF8().data();
        return 0;
    }
    return wxMediaCtrl::MSWWindowProc(nMsg, wParam, lParam);
}

#endif
