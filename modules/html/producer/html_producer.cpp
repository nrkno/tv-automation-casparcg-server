/*
* Copyright 2013 Sveriges Television AB http://casparcg.com/
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "html_producer.h"

#include <core/video_format.h>

#include <core/monitor/monitor.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_transform.h>
#include <core/producer/frame_producer.h>
#include <core/interaction/interaction_event.h>
#include <core/frame/frame.h>
#include <core/frame/pixel_format.h>
#include <core/frame/audio_channel_layout.h>
#include <core/frame/geometry.h>
#include <core/help/help_repository.h>
#include <core/help/help_sink.h>

#include <common/assert.h>
#include <common/env.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/diagnostics/graph.h>
#include <common/prec_timer.h>
#include <common/linq.h>
#include <common/os/filesystem.h>
#include <common/timer.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#pragma warning(push)
#pragma warning(disable: 4458)
#include <cef_task.h>
#include <cef_app.h>
#include <cef_client.h>
#include <cef_render_handler.h>
#pragma warning(pop)

#include <queue>

#include "../html.h"

#ifdef WIN32
#include <accelerator/d3d/d3d_device.h>
#include <accelerator/d3d/d3d_device_context.h>
#include <accelerator/d3d/d3d_texture2d.h>
#endif

#pragma comment (lib, "libcef.lib")
#pragma comment (lib, "libcef_dll_wrapper.lib")

namespace caspar { namespace html {

class html_client
	: public CefClient
	, public CefRenderHandler
	, public CefLifeSpanHandler
	, public CefLoadHandler
    , public CefDisplayHandler
{
	std::wstring							url_;
	spl::shared_ptr<diagnostics::graph>		graph_;
	caspar::timer							tick_timer_;
	caspar::timer							frame_timer_;
	caspar::timer							paint_timer_;

	spl::shared_ptr<core::frame_factory>	frame_factory_;
	core::video_format_desc					format_desc_;
    bool                                 	shared_texture_enable_;
	tbb::concurrent_queue<std::wstring>		javascript_before_load_;
	std::atomic<bool>						loaded_;
	std::atomic<bool>						removed_;
	std::queue<core::draw_frame>			frames_;
	mutable std::mutex						frames_mutex_;

	core::draw_frame						last_frame_;
	core::draw_frame						last_progressive_frame_;
	mutable std::mutex						last_frame_mutex_;

	CefRefPtr<CefBrowser>					browser_;
	
#ifdef WIN32
    std::shared_ptr<accelerator::d3d::d3d_device> const d3d_device_;
    std::shared_ptr<accelerator::d3d::d3d_texture2d>    d3d_shared_buffer_;
#endif

	executor								executor_;

public:

	html_client(
			spl::shared_ptr<core::frame_factory> frame_factory,
			const core::video_format_desc& format_desc,
            bool shared_texture_enable,
			const std::wstring& url)
		: url_(url)
		, frame_factory_(std::move(frame_factory))
		, format_desc_(format_desc)
        , shared_texture_enable_(shared_texture_enable)
		, last_frame_(core::draw_frame::empty())
		, last_progressive_frame_(core::draw_frame::empty())
#ifdef WIN32
        , d3d_device_(accelerator::d3d::d3d_device::get_device())
#endif
		, executor_(L"html_producer")
	{
		graph_->set_color("browser-tick-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_color("browser-dropped-frame", diagnostics::color(0.6f, 0.1f, 0.1f));
        graph_->set_color("overload", diagnostics::color(0.6f, 0.6f, 0.3f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		loaded_ = false;
		removed_ = false;
		executor_.begin_invoke([&]{ update(); });
	}

	core::draw_frame receive()
	{
		auto frame = last_frame();
		executor_.begin_invoke([&]{ update(); });
		return frame;
	}

	core::draw_frame last_frame() const
	{
		std::lock_guard<std::mutex> lock(last_frame_mutex_);
		return last_frame_;
	}

	void execute_javascript(const std::wstring& javascript)
	{
		if (!loaded_)
		{
			javascript_before_load_.push(javascript);
		}
		else
		{
			execute_queued_javascript();
			do_execute_javascript(javascript);
		}
	}

	bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
		CefRefPtr<CefFrame> frame,
		const CefString& target_url,
		const CefString& target_frame_name,
		WindowOpenDisposition target_disposition,
		bool user_gesture,
		const CefPopupFeatures& popupFeatures,
		CefWindowInfo& windowInfo,
		CefRefPtr<CefClient>& client,
		CefBrowserSettings& settings,
		bool* no_javascript_access) override
	{
		// This blocks popup windows from opening, as they dont make sense and hit an exception in get_browser_host upon closing
		return true;
	}
	

	CefRefPtr<CefBrowserHost> get_browser_host() const
	{
		if (browser_)
			return browser_->GetHost();
		return nullptr;
	}

	void close()
	{
		html::invoke([=]
		{
			if (browser_ != nullptr)
			{
				browser_->GetHost()->CloseBrowser(true);
			}
		});
	}

	void remove()
	{
		close();
		removed_ = true;
	}

	bool is_removed() const
	{
		return removed_;
	}

private:

	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
	{
		CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

		rect = CefRect(0, 0, format_desc_.square_width, format_desc_.square_height);
	}

	void OnPaint(
			CefRefPtr<CefBrowser> browser,
			PaintElementType type,
			const RectList &dirtyRects,
			const void *buffer,
			int width,
			int height)
	{
        if (shared_texture_enable_)
            return;

		graph_->set_value("browser-tick-time", paint_timer_.elapsed()
				* format_desc_.fps
				* format_desc_.field_count
				* 0.5);
		paint_timer_.restart();
		CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

		if (type != PET_VIEW)
			return;

		core::pixel_format_desc pixel_desc;
		pixel_desc.format = core::pixel_format::bgra;
		pixel_desc.planes.push_back(core::pixel_format_desc::plane(width, height, 4));

		auto frame = frame_factory_->create_frame(this, pixel_desc, core::audio_channel_layout::invalid());
		std::memcpy(frame.image_data().begin(), buffer, width * height * 4);

		{
            std::lock_guard<std::mutex> lock(frames_mutex_);
			frames_.push(core::draw_frame(std::move(frame)));

			size_t max_in_queue = format_desc_.field_count + 1;

			while (frames_.size() > max_in_queue)
			{
				frames_.pop();
				graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
			}
		}
	}
	
#ifdef WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                            PaintElementType      type,
                            const RectList&       dirtyRects,
                            void*                 shared_handle) override
    {
        try {
            if (!shared_texture_enable_)
                return;

			graph_->set_value("browser-tick-time", paint_timer_.elapsed()
				* format_desc_.fps
				* format_desc_.field_count
				* 0.5);
            paint_timer_.restart();
            CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

            if (type != PET_VIEW)
                return;

            if (d3d_shared_buffer_) {
                if (shared_handle != d3d_shared_buffer_->share_handle())
                    d3d_shared_buffer_.reset();
            }

            if (!d3d_shared_buffer_) {
                d3d_shared_buffer_ = d3d_device_->open_shared_texture(shared_handle);
                if (!d3d_shared_buffer_)
                    CASPAR_LOG(error) << print() << L" could not open shared texture!";
            }

            if (d3d_shared_buffer_ && d3d_shared_buffer_->format() == DXGI_FORMAT_B8G8R8A8_UNORM) {
                auto             frame = frame_factory_->import_d3d_texture(this, d3d_shared_buffer_);
                core::draw_frame dframe(std::move(frame));

                // Image need flip vertically
                // Top to bottom
                dframe.transform().image_transform.perspective.ul[1] = 1;
                dframe.transform().image_transform.perspective.ur[1] = 1;
                // Bottom to top
                dframe.transform().image_transform.perspective.ll[1] = 0;
                dframe.transform().image_transform.perspective.lr[1] = 0;

                {
                    std::lock_guard<std::mutex> lock(frames_mutex_);

                    frames_.push(dframe);
                    while (frames_.size() > 8) {
                        frames_.pop();
                        graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
                    }
                }
            }
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
#endif

	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
	{
		CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

		browser_ = browser;
	}

	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
	{
		CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

		removed_ = true;
		browser_ = nullptr;
	}

	bool DoClose(CefRefPtr<CefBrowser> browser) override
	{
		CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

		return false;
	}

	bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t    level,
						  const CefString&      message,
						  const CefString&      source,
						  int                   line) override
    {
		if (level == cef_log_severity_t::LOGSEVERITY_DEBUG)
			CASPAR_LOG(debug) << print() << L" Log: " << message.ToWString();
		else if (level == cef_log_severity_t::LOGSEVERITY_WARNING)
			CASPAR_LOG(warning) << print() << L" Log: " << message.ToWString();
		else if (level == cef_log_severity_t::LOGSEVERITY_ERROR)
			CASPAR_LOG(error) << print() << L" Log: " << message.ToWString();
		else if (level == cef_log_severity_t::LOGSEVERITY_FATAL)
			CASPAR_LOG(fatal) << print() << L" Log: " << message.ToWString();
		else
			CASPAR_LOG(info) << print() << L" Log: " << message.ToWString();
        return true;
    }

	CefRefPtr<CefRenderHandler> GetRenderHandler() override
	{
		return this;
	}

	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override
	{
		return this;
	}

	CefRefPtr<CefLoadHandler> GetLoadHandler() override {
		return this;
	}

	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

	void OnLoadEnd(
			CefRefPtr<CefBrowser> browser,
			CefRefPtr<CefFrame> frame,
			int httpStatusCode) override
	{
		loaded_ = true;
		execute_queued_javascript();
	}

	bool OnProcessMessageReceived(
			CefRefPtr<CefBrowser> browser,
			CefProcessId source_process,
			CefRefPtr<CefProcessMessage> message) override
	{
		auto name = message->GetName().ToString();

		if (name == REMOVE_MESSAGE_NAME)
		{
			remove();

			return true;
		}
		else if (name == LOG_MESSAGE_NAME)
		{
			auto args = message->GetArgumentList();
			auto severity =
				static_cast<boost::log::trivial::severity_level>(args->GetInt(0));
			auto msg = args->GetString(1).ToWString();

			BOOST_LOG_STREAM_WITH_PARAMS(
					log::logger::get(),
					(boost::log::keywords::severity = severity))
				<< print() << L" [renderer_process] " << msg;
		}

		return false;
	}

	void invoke_requested_animation_frames()
	{
		if (browser_)
			browser_->SendProcessMessage(
					CefProcessId::PID_RENDERER,
					CefProcessMessage::Create(TICK_MESSAGE_NAME));

		graph_->set_value("tick-time", tick_timer_.elapsed()
				* format_desc_.fps
				* format_desc_.field_count
				* 0.5);
		tick_timer_.restart();
	}

	bool try_pop(core::draw_frame& result)
	{
		{
            std::lock_guard<std::mutex> lock(frames_mutex_);

			if (!frames_.empty())
			{
				result = std::move(frames_.front());
				frames_.pop();

				return true;
			}

			return false;
		}
	}

	void update()
	{
		invoke_requested_animation_frames();

		const bool is_interlaced = format_desc_.field_mode != core::field_mode::progressive;
        if (is_interlaced) {
			prec_timer timer;
			timer.tick(0.0); // First tick just sets the current time
            timer.tick(1.0 / (format_desc_.fps * format_desc_.field_count));
            invoke_requested_animation_frames();
		}

		core::draw_frame frame1;
		if (!try_pop(frame1)) {
			if (is_interlaced) {
				std::lock_guard<std::mutex> lock(last_frame_mutex_);
				last_frame_ = last_progressive_frame_;
			}

			graph_->set_tag(diagnostics::tag_severity::SILENT, "browser-dropped-frame");
			return;
		}

		if (is_interlaced) {
			core::draw_frame frame2;
			if (!try_pop(frame2)) {
				{
					std::lock_guard<std::mutex> lock(last_frame_mutex_);
					last_progressive_frame_ = frame1;
					last_frame_ = frame1;
				}

				graph_->set_tag(diagnostics::tag_severity::SILENT, "browser-dropped-frame");
			} else {
				std::lock_guard<std::mutex> lock(last_frame_mutex_);
				last_progressive_frame_ = frame2;
				last_frame_ = core::draw_frame::interlace(frame1, frame2, format_desc_.field_mode);
			}
		} else {
			std::lock_guard<std::mutex> lock(last_frame_mutex_);
			last_frame_ = frame1;
		}
	}

	void do_execute_javascript(const std::wstring& javascript)
	{
		html::begin_invoke([=]
		{
			if (browser_ != nullptr)
				browser_->GetMainFrame()->ExecuteJavaScript(u8(javascript).c_str(), browser_->GetMainFrame()->GetURL(), 0);
		});
	}

	void execute_queued_javascript()
	{
		std::wstring javascript;

		while (javascript_before_load_.try_pop(javascript))
			do_execute_javascript(javascript);
	}

	std::wstring print() const
	{
		return L"html[" + url_ + L"]";
	}

	IMPLEMENT_REFCOUNTING(html_client);
};

class html_producer
	: public core::frame_producer_base
{
	core::monitor::subject	monitor_subject_;
	const std::wstring		url_;
	core::constraints		constraints_;

	CefRefPtr<html_client>	client_;

public:
	html_producer(
		const spl::shared_ptr<core::frame_factory>& frame_factory,
		const core::video_format_desc& format_desc,
		const std::wstring& url)
		: url_(url)
	{
		constraints_.width.set(format_desc.square_width);
		constraints_.height.set(format_desc.square_height);

		html::invoke([&]
		{
			const bool enable_gpu = env::properties().get(L"configuration.html.enable-gpu", false);
            bool       shared_texture_enable = false;
			
#ifdef WIN32
            shared_texture_enable = enable_gpu && accelerator::d3d::d3d_device::get_device();
#endif

			client_ = new html_client(frame_factory, format_desc, shared_texture_enable, url_);

			CefWindowInfo window_info;
			window_info.width = format_desc.square_width;
			window_info.height = format_desc.square_height;
			window_info.windowless_rendering_enabled = true;
            window_info.shared_texture_enabled = shared_texture_enable;

			CefBrowserSettings browser_settings;
			browser_settings.web_security = cef_state_t::STATE_DISABLED;
			browser_settings.webgl = enable_gpu ? cef_state_t::STATE_ENABLED : cef_state_t::STATE_DISABLED;
			double fps = format_desc.fps;
			if (format_desc.field_mode != core::field_mode::progressive) {
				fps *= 2.0;
			}
			browser_settings.windowless_frame_rate = int(ceil(fps));
			CefBrowserHost::CreateBrowser(window_info, client_.get(), url, browser_settings, nullptr);
		});
	}

	~html_producer()
	{
		if (client_)
			client_->close();
	}

	// frame_producer

	std::wstring name() const override
	{
		return L"html";
	}

	void on_interaction(const core::interaction_event::ptr& event) override
	{
		if (!client_ || client_->is_removed())
			return;

		auto host = client_->get_browser_host();
		if (!host)
			return;

		if (core::is<core::mouse_move_event>(event))
		{
			auto move = core::as<core::mouse_move_event>(event);
			int x = static_cast<int>(move->x * constraints_.width.get());
			int y = static_cast<int>(move->y * constraints_.height.get());

			CefMouseEvent e;
			e.x = x;
			e.y = y;
			host->SendMouseMoveEvent(e, false);
		}
		else if (core::is<core::mouse_button_event>(event))
		{
			auto button = core::as<core::mouse_button_event>(event);
			int x = static_cast<int>(button->x * constraints_.width.get());
			int y = static_cast<int>(button->y * constraints_.height.get());

			CefMouseEvent e;
			e.x = x;
			e.y = y;
			host->SendMouseClickEvent(
					e,
					static_cast<CefBrowserHost::MouseButtonType>(button->button),
					!button->pressed,
					1);
		}
		else if (core::is<core::mouse_wheel_event>(event))
		{
			auto wheel = core::as<core::mouse_wheel_event>(event);
			int x = static_cast<int>(wheel->x * constraints_.width.get());
			int y = static_cast<int>(wheel->y * constraints_.height.get());

			CefMouseEvent e;
			e.x = x;
			e.y = y;
			static const int WHEEL_TICKS_AMPLIFICATION = 40;
			host->SendMouseWheelEvent(
					e,
					0,                                               // delta_x
					wheel->ticks_delta * WHEEL_TICKS_AMPLIFICATION); // delta_y
		}
	}

	bool collides(double x, double y) const override
	{
		return client_ != nullptr && !client_->is_removed();
	}

	core::draw_frame receive_impl() override
	{
		if (client_)
		{
			if (client_->is_removed())
			{
				client_ = nullptr;
				return core::draw_frame::empty();
			}

			monitor_subject_
				<< core::monitor::message("/file/path") % url_;

			return client_->receive();
		}

		return core::draw_frame::empty();
	}

	std::future<std::wstring> call(const std::vector<std::wstring>& params) override
	{
		if (!client_)
			return make_ready_future(std::wstring(L""));

		auto javascript = params.at(0);

		client_->execute_javascript(javascript);

		return make_ready_future(std::wstring(L""));
	}

	std::wstring print() const override
	{
		return L"html[" + url_ + L"]";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"html-producer");

		if (client_) {
			info.add(L"filename", url_);
		}

		return info;
	}

	core::constraints& pixel_constraints() override
	{
		return constraints_;
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}
};

void describe_producer(core::help_sink& sink, const core::help_repository& repo)
{
	sink.short_description(L"Renders a web page in real time.");
	sink.syntax(L"{[html_filename:string]},{[HTML] [url:string]}");
	sink.para()->text(L"Embeds an actual web browser and renders the content in realtime.");
	sink.para()
		->text(L"HTML content can either be stored locally under the ")->code(L"templates")
		->text(L" folder or fetched directly via an URL. If a .html file is found with the name ")
		->code(L"html_filename")->text(L" under the ")->code(L"templates")->text(L" folder it will be rendered. If the ")
		->code(L"[HTML] url")->text(L" syntax is used instead, the URL will be loaded.");
	sink.para()->text(L"Examples:");
	sink.example(L">> PLAY 1-10 [HTML] http://www.casparcg.com");
	sink.example(L">> PLAY 1-10 folder/html_file");
}

spl::shared_ptr<core::frame_producer> create_cg_producer(
	const core::frame_producer_dependencies& dependencies,
	const std::vector<std::wstring>& params)
{
	std::wstring query = L"";
	std::wstring file;
	std::wstring fileSearch = params.at(0);
	const auto hasQuery = fileSearch.find_first_of(L"?");
	const auto hasFragment = fileSearch.find_first_of(L"#");
	if (std::wstring::npos != hasQuery)
	{
		file = fileSearch.substr(0, hasQuery);
		query = L"?" + fileSearch.substr(hasQuery + 1);
	}
	else if (std::wstring::npos != hasFragment) {
		file = fileSearch.substr(0, hasFragment);
		query = L"#" + fileSearch.substr(hasFragment + 1);
	}
	else
	{
		file = fileSearch;
	}

	const auto filename = env::template_folder() + file + L".html";
	const auto found_filename = find_case_insensitive(filename);
	const auto http_prefix = boost::algorithm::istarts_with(params.at(0), L"http:") || boost::algorithm::istarts_with(params.at(0), L"https:");

	if (!found_filename && !http_prefix)
		return core::frame_producer::empty();

	const auto url = found_filename
		? L"file://" + *found_filename
		: params.at(0);

	return core::create_destroy_proxy(spl::make_shared<html_producer>(
		dependencies.frame_factory,
		dependencies.format_desc,
		url));
}

spl::shared_ptr<core::frame_producer> create_producer(
		const core::frame_producer_dependencies& dependencies,
		const std::vector<std::wstring>& params)
{
	const auto filename			= env::template_folder() + params.at(0) + L".html";
	const auto found_filename	= find_case_insensitive(filename);
	const auto html_prefix		= boost::iequals(params.at(0), L"[HTML]");

	if (!found_filename && !html_prefix)
		return core::frame_producer::empty();

	const auto url = found_filename
		? L"file://" + *found_filename
		: params.at(1);

	if (!html_prefix && (!boost::algorithm::contains(url, ".") || boost::algorithm::ends_with(url, "_A") || boost::algorithm::ends_with(url, "_ALPHA")))
		return core::frame_producer::empty();

	return core::create_destroy_proxy(spl::make_shared<html_producer>(
			dependencies.frame_factory,
			dependencies.format_desc,
			url));
}

}}
