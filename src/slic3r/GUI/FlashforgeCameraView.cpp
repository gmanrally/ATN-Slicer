#include "FlashforgeCameraView.hpp"

#include <thread>
#include <chrono>

#include <boost/asio.hpp>

#include <wx/dcbuffer.h>
#include <wx/mstream.h>
#include <wx/image.h>
#include <wx/settings.h>

#include "I18N.hpp"

namespace Slic3r {
namespace GUI {

using boost::asio::ip::tcp;

static constexpr size_t MAX_BUFFER = 4 * 1024 * 1024;   // desync guard: drop the buffer past this
static constexpr int    RENDER_MS  = 66;                // ~15 fps decode/paint on the GUI thread

struct FlashforgeCameraView::StreamState
{
    std::mutex        mtx;
    std::string       latest;                 // most recent complete JPEG frame
    long              frame_id {0};           // bumped per frame so the GUI knows when to redraw
    std::string       error;                  // last transport error, for display
    std::atomic<bool> running {true};
    std::atomic<bool> got_frame {false};
    // NOTE: the socket is deliberately NOT stored here. It is owned entirely by the worker
    // thread (created, read, and destroyed on that thread). asio sockets are not thread-safe,
    // and a previous version closed this socket from Stop() (the GUI thread) to interrupt the
    // blocking read — that cross-thread close corrupted asio's internal state and access-
    // violated the worker (crash on the FlashForge camera Expand). The worker now self-
    // terminates via `running` + a receive timeout instead; nothing else touches the socket.
};

// Parse "http://host:port/path" -> (host, port, path). Defaults: port 80, path "/". Tolerant enough
// for the fixed cameraStreamUrl the printer emits; not a general URL parser.
static bool parse_url(const std::string& url, std::string& host, std::string& port, std::string& path)
{
    auto p = url.find("://");
    std::string rest = (p == std::string::npos) ? url : url.substr(p + 3);
    auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = authority.find(':');
    if (colon == std::string::npos) { host = authority; port = "80"; }
    else { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
    return !host.empty();
}

FlashforgeCameraView::FlashforgeCameraView(wxWindow* parent)
    : wxWindow(parent, wxID_ANY)
    , m_timer(this)
    , m_status_text(_L("Connecting to camera…"))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // we paint everything ourselves (buffered)
    SetBackgroundColour(*wxBLACK);
    Bind(wxEVT_PAINT, &FlashforgeCameraView::on_paint, this);
    Bind(wxEVT_SIZE, &FlashforgeCameraView::on_size, this);
    Bind(wxEVT_TIMER, &FlashforgeCameraView::on_timer, this);
}

FlashforgeCameraView::~FlashforgeCameraView() { Stop(); }

void FlashforgeCameraView::Start(const wxString& mjpeg_url)
{
    if (m_active_url == mjpeg_url && m_state && m_state->running)
        return;                              // already streaming this URL
    Stop();

    m_active_url    = mjpeg_url;
    m_status_text   = _L("Connecting to camera…");
    m_last_frame_id = -1;
    m_bitmap        = wxBitmap();

    auto state = std::make_shared<StreamState>();
    m_state = state;

    const std::string url = mjpeg_url.ToStdString();
    // Detached worker that OWNS its socket end-to-end (created, read, destroyed on this thread)
    // and self-terminates when state->running goes false. A bounded receive timeout makes the
    // blocking read return periodically so it notices Stop() promptly WITHOUT any other thread
    // touching the socket; a short reconnect loop rides out transient drops and the brief window
    // where another view is releasing a single-client camera's one connection (AD5X / 5M).
    std::thread([state, url]() {
        std::string host, port, path;
        if (!parse_url(url, host, port, path)) {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->error = "bad camera URL";
            state->running = false;
            return;
        }
        int idle_fails = 0;
        while (state->running) {
            bool got_any = false;
            boost::system::error_code ec;
            try {
                boost::asio::io_context io;
                tcp::resolver resolver(io);
                auto endpoints = resolver.resolve(host, port, ec);
                tcp::socket sock(io);
                if (!ec) boost::asio::connect(sock, endpoints, ec);
                if (!ec) {
                    // Bounded blocking reads: read_some returns after this long with no data, so
                    // the loop can re-check running (and reconnect) with no cross-thread close.
#ifdef _WIN32
                    DWORD tv = 250;
                    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv));
#else
                    timeval tv{0, 250000};
                    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
                    // HTTP/1.0 so the server does not use chunked transfer-encoding; we scan the
                    // raw body for JPEG markers, so we ignore the multipart boundary + HTTP headers
                    // (they are ASCII and never contain 0xFFD8).
                    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                                      "\r\nUser-Agent: OrcaSlicer\r\nAccept: */*\r\n\r\n";
                    boost::asio::write(sock, boost::asio::buffer(req), ec);
                    std::string buf;
                    std::vector<char> chunk(65536);
                    while (!ec && state->running) {
                        size_t n = sock.read_some(boost::asio::buffer(chunk), ec);
                        if (ec == boost::asio::error::timed_out ||
                            ec == boost::asio::error::try_again ||
                            ec == boost::asio::error::would_block) {
                            ec.clear();
                            continue;                 // read timeout — just re-check running
                        }
                        if (ec || n == 0)
                            break;                    // real disconnect
                        got_any = true;
                        buf.append(chunk.data(), n);
                        // Slice out every complete JPEG (SOI 0xFFD8 .. EOI 0xFFD9) in the buffer.
                        while (true) {
                            auto soi = buf.find("\xff\xd8");
                            if (soi == std::string::npos) { if (buf.size() > MAX_BUFFER) buf.clear(); break; }
                            auto eoi = buf.find("\xff\xd9", soi + 2);
                            if (eoi == std::string::npos) {
                                if (soi > 0) buf.erase(0, soi);   // drop leading non-JPEG (HTTP hdrs)
                                if (buf.size() > MAX_BUFFER) buf.clear();
                                break;
                            }
                            std::string frame = buf.substr(soi, eoi + 2 - soi);
                            buf.erase(0, eoi + 2);
                            std::lock_guard<std::mutex> lk(state->mtx);
                            state->latest = std::move(frame);
                            ++state->frame_id;
                            state->got_frame = true;
                        }
                    }
                }
                // sock destructs HERE, on this worker thread — the only thread that touches it.
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(state->mtx);
                if (state->error.empty()) state->error = e.what();
            } catch (...) {
            }
            if (!state->running)
                break;
            // Reconnect on a dropped stream; give up (leave "stream ended") only after a run of
            // frame-less failures long enough to cover a single-client handoff.
            idle_fails = got_any ? 0 : idle_fails + 1;
            if (idle_fails > 12)
                break;
            for (int i = 0; i < 2 && state->running; ++i)   // ~200ms backoff, responsive to Stop
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        state->running = false;
    }).detach();

    m_timer.Start(RENDER_MS);
}

void FlashforgeCameraView::Stop()
{
    m_timer.Stop();
    if (!m_state)
        return;
    // Signal only. The worker owns its socket and tears it down itself within the receive
    // timeout (~250 ms). We must NOT close it from here: asio sockets aren't thread-safe, and a
    // cross-thread close was access-violating the worker (crash on the camera Expand pop-out).
    m_state->running = false;
    m_state.reset();                          // worker keeps its own shared_ptr until it exits
    m_active_url.clear();
}

void FlashforgeCameraView::on_timer(wxTimerEvent&)
{
    if (!m_state)
        return;

    long        id = -1;
    std::string jpeg;
    std::string err;
    bool        running;
    {
        std::lock_guard<std::mutex> lk(m_state->mtx);
        id      = m_state->frame_id;
        err     = m_state->error;
        running = m_state->running;
        if (m_state->got_frame && id != m_last_frame_id)
            jpeg = m_state->latest;           // copy out the latest frame only if it changed
    }

    if (!jpeg.empty()) {
        wxMemoryInputStream mis(jpeg.data(), jpeg.size());
        wxImage img;
        if (img.LoadFile(mis, wxBITMAP_TYPE_JPEG)) {
            m_bitmap = wxBitmap(img);
            m_last_frame_id = id;
            Refresh(false);
            return;
        }
    }

    if (!m_bitmap.IsOk()) {
        // No frame yet: keep the user informed about connecting vs a dead stream.
        wxString want = running ? _L("Connecting to camera…")
                                : (err.empty() ? _L("Camera stream ended") : _L("Camera unavailable"));
        if (want != m_status_text) { m_status_text = want; Refresh(false); }
    }
}

void FlashforgeCameraView::on_size(wxSizeEvent& e) { Refresh(false); e.Skip(); }

void FlashforgeCameraView::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(*wxBLACK));
    dc.Clear();

    const wxSize cs = GetClientSize();
    if (m_bitmap.IsOk() && cs.x > 0 && cs.y > 0) {
        const double bw = m_bitmap.GetWidth(), bh = m_bitmap.GetHeight();
        const double s  = std::min(cs.x / bw, cs.y / bh);        // aspect-fit
        const int    w  = std::max(1, int(bw * s)), h = std::max(1, int(bh * s));
        wxImage img = m_bitmap.ConvertToImage().Scale(w, h, wxIMAGE_QUALITY_NORMAL);
        dc.DrawBitmap(wxBitmap(img), (cs.x - w) / 2, (cs.y - h) / 2, false);
    } else {
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        const wxSize ts = dc.GetTextExtent(m_status_text);
        dc.DrawText(m_status_text, (cs.x - ts.x) / 2, (cs.y - ts.y) / 2);
    }
}

} // namespace GUI
} // namespace Slic3r
