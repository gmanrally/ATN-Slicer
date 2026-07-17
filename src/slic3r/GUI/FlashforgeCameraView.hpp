#ifndef slic3r_FlashforgeCameraView_hpp_
#define slic3r_FlashforgeCameraView_hpp_

// Live MJPEG camera view for Flashforge printers (AD5M / 5M Pro / AD5X).
//
// The printer serves a plain MJPEG-over-HTTP stream at http://<ip>:8080/?action=stream (the URL the
// detail endpoint reports as cameraStreamUrl). Unlike the Bambu camera -- which needs the proprietary
// network agent / cloud tunnel -- this is a standard mjpg-streamer feed on the LAN, so it can be
// rendered natively with no cloud and no extra dependency.
//
// Design: a worker thread pulls the stream over a raw asio socket into a bounded rolling buffer,
// slicing out complete JPEG frames by their SOI/EOI markers and keeping only the LATEST one (decoding
// on the GUI thread lags the network, so old frames are dropped, not queued). A GUI-side timer
// decodes that latest frame with wxImage and paints it aspect-fitted. The worker only ever touches a
// shared, mutex-guarded state block -- never this window -- so it is safe for the worker to outlive
// the view; a shared_ptr keeps the state alive until the worker exits.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <wx/window.h>
#include <wx/bitmap.h>
#include <wx/timer.h>

namespace Slic3r {
namespace GUI {

class FlashforgeCameraView : public wxWindow
{
public:
    explicit FlashforgeCameraView(wxWindow* parent);
    ~FlashforgeCameraView() override;

    // (Re)start streaming from an MJPEG URL. Calling with the URL already streaming is a no-op, so
    // the status panel can call it every poll without tearing the stream down.
    void Start(const wxString& mjpeg_url);
    void Stop();
    bool is_streaming_url(const wxString& url) const { return m_active_url == url; }

private:
    // Shared between the GUI side and the detached worker. Outlives either safely via shared_ptr.
    struct StreamState;

    void on_timer(wxTimerEvent&);
    void on_paint(wxPaintEvent&);
    void on_size(wxSizeEvent&);

    std::shared_ptr<StreamState> m_state;
    wxString                     m_active_url;
    wxTimer                      m_timer;

    wxBitmap m_bitmap;          // last decoded frame, scaled to the view
    wxString m_status_text;     // shown when there is no frame yet ("Connecting…" / error)
    long     m_last_frame_id {-1};
};

} // namespace GUI
} // namespace Slic3r

#endif
