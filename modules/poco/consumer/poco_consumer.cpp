/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
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

#include "poco_consumer.h"

#include <common/diagnostics/graph.h>
#include <common/gl/gl_check.h>
#include <common/log.h>
#include <common/memory.h>
#include <common/array.h>
#include <common/memshfl.h>
#include <common/utf.h>
#include <common/prec_timer.h>
#include <common/future.h>
#include <common/timer.h>
#include <common/param.h>
#include <common/os/general_protection_fault.h>
#include <common/scope_exit.h>

#include <core/video_format.h>
#include <core/frame/frame.h>
#include <core/consumer/frame_consumer.h>
#include <core/interaction/interaction_sink.h>
#include <core/help/help_sink.h>
#include <core/help/help_repository.h>

#include <boost/circular_buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>

#include <tbb/concurrent_queue.h>

#include <algorithm>
#include <vector>

#if defined(_MSC_VER)
// #include <windows.h>

#pragma warning (push)
#pragma warning (disable : 4244)
#endif

#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include "Poco/Net/ServerSocket.h"
#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPRequestHandlerFactory.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPResponse.h"
// #include "Poco/Util/ServerApplication.h"
// #include <iostream>

class HelloRequestHandler : public Poco::Net::HTTPRequestHandler
{
	void handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response)
	{
		response.setChunkedTransferEncoding(true);
		response.setContentType("text/html");

		response.send()
			<< "<html>"
			<< "<head><title>Hello from CasparCG</title></head>"
			<< "<body><h1>Hello from the CasparCG embedded web server</h1></body>"
			<< "</html>";
	}
};

class HelloRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
	Poco::Net::HTTPRequestHandler* createRequestHandler(const Poco::Net::HTTPServerRequest&)
	{
		CASPAR_LOG(warning) << "Creating HelloRequestsHandler";
		return new HelloRequestHandler;
	}
};

/* class WebServerApp : public ServerApplication
{
	void initialize(Application& self)
	{
		loadConfiguration();
		ServerApplication::initialize(self);
	}

	int main(const std::vector<std::string>&)
	{
		UInt16 port = static_cast<UInt16>(config().getUInt("port", 8080));

		HTTPServer srv(new HelloRequestHandlerFactory, port);
		srv.start();
		logger().information("HTTP Server started on port %hu.", port);
		waitForTerminationRequest();
		logger().information("Stopping HTTP Server...");
		srv.stop();

		return Application::EXIT_OK;
	}
}; */

namespace caspar { namespace poco {

struct configuration
{
	int				port		= 80;
	std::wstring	URI			= L"";
	bool			push		= false;
};

struct poco_consumer : boost::noncopyable
{
	const configuration									config_;
	core::video_format_desc								format_desc_;
	int													channel_index_;

	float												width_;
	float												height_;

	std::int64_t										pts_;

	spl::shared_ptr<diagnostics::graph>					graph_;
	caspar::timer										perf_timer_;
	caspar::timer										tick_timer_;
	caspar::prec_timer									wait_timer_;

	tbb::concurrent_bounded_queue<core::const_frame>	frame_buffer_;
	core::interaction_sink*								sink_;

	std::atomic<bool>									is_running_;
	std::atomic<int64_t>								current_presentation_age_;
	boost::thread										thread_;
public:
	poco_consumer(
			const configuration& config,
			const core::video_format_desc& format_desc,
			int channel_index,
			core::interaction_sink* sink)
		: config_(config)
		, format_desc_(format_desc)
		, channel_index_(channel_index)
		, pts_(0)
		, sink_(sink)
	{
		frame_buffer_.set_capacity(10);

		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		is_running_ = true;
		thread_ = boost::thread([this]{run();});
	}

	~poco_consumer()
	{
		is_running_ = false;
		frame_buffer_.clear();
		frame_buffer_.try_push(core::const_frame::empty());
		thread_.join();
	}

	void init()
	{

	}

	void uninit()
	{

	}

	void run()
	{
		ensure_gpf_handler_installed_for_thread("poco-consumer-thread");

		try
		{
			init();

			Poco::Net::HTTPServer srv(new HelloRequestHandlerFactory, Poco::Net::ServerSocket(3002), new Poco::Net::HTTPServerParams);
			srv.start();
			CASPAR_LOG(info) << print() << " Started HTTP server: port " << srv.port()
				<< " threads " << srv.currentThreads() << "/" << srv.maxThreads();

			while(is_running_)
			{
				try
				{
					core::const_frame frame;
					frame_buffer_.pop(frame);

					if (config_.push) {
						post_frame(frame);
					}

					/*perf_timer_.restart();
					render(frame);
					graph_->set_value("frame-time", perf_timer_.elapsed()*format_desc_.fps*0.5);

					window_.Display();*/

					current_presentation_age_ = frame.get_age_millis();
					graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
					tick_timer_.restart();
				}
				catch(...)
				{
					CASPAR_LOG_CURRENT_EXCEPTION();
					is_running_ = false;
				}
			}

			uninit();
			srv.stop();
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
	}

	void try_sleep_almost_until_vblank()
	{
		static const double THRESHOLD = 0.003;
		double threshold = THRESHOLD;

		auto frame_time = 1.0 / (format_desc_.fps * format_desc_.field_count);

		wait_timer_.tick(frame_time - threshold);
	}

	void wait_for_vblank_and_display()
	{
		try_sleep_almost_until_vblank();
		// window_.display();
		// Make sure that the next tick measures the duration from this point in time.
		wait_timer_.tick(0.0);
	}

	/* spl::shared_ptr<AVFrame> get_av_frame()
	{
		auto av_frame = ffmpeg::create_frame();

		av_frame->linesize[0]		= format_desc_.width*4;
		av_frame->format				= AVPixelFormat::AV_PIX_FMT_BGRA;
		av_frame->width				= format_desc_.width;
		av_frame->height				= format_desc_.height;
		av_frame->interlaced_frame	= format_desc_.field_mode != core::field_mode::progressive;
		av_frame->top_field_first	= format_desc_.field_mode == core::field_mode::upper ? 1 : 0;
		av_frame->pts				= pts_++;

		return av_frame;
	} */

	void post_frame(core::const_frame input_frame)
	{
		if (static_cast<size_t>(input_frame.image_data().size()) != format_desc_.size)
			return;

		perf_timer_.restart();
		const uint8_t* data = const_cast<uint8_t*>(input_frame.image_data().begin());

		render(data);
	}

	void render(const uint8_t* data)
	{
		Poco::Net::HTTPClientSession s("192.168.178.50", 3000);
		s.setKeepAlive(true);

		Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_POST, "/fred.jpg", Poco::Net::HTTPMessage::HTTP_1_1);
		req.setKeepAlive(true);
		req.setContentLength(1920 * 1080 * 4);
		req.setContentType("application/octet-stream");
		// s.setTimeout(Poco::Timespan(1000));
		std::ostream& ops = s.sendRequest(req);
		s.socket().setSendBufferSize(65536 * 4);
		// s.socket().setNoDelay(true);
		ops.write((const char*) data, 1920 * 1080 * 4);
		// ops.flush();

		Poco::Net::HTTPResponse response;
		std::istream& istr = s.receiveResponse(response);
		// CASPAR_LOG(info) << print() << response.getStatus() << L" before " << istr.eof();
		while (istr) {
			istr.get();
		}
		// CASPAR_LOG(info) << print() << response.getStatus() << L" after " << istr.eof();
		// s.reset();
	}


	std::future<bool> send(core::const_frame frame)
	{
		if(!frame_buffer_.try_push(frame)) // Might need to check that the last frame got out OK
			graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");

		return make_ready_future(is_running_.load());
	}

	std::wstring channel_and_format() const
	{
		return L"[" + boost::lexical_cast<std::wstring>(channel_index_) + L"|" + format_desc_.name + L"]";
	}

	std::wstring print() const
	{
		return config_.URI + L" " + channel_and_format();
	}
};


struct poco_consumer_proxy : public core::frame_consumer
{
	core::monitor::subject				monitor_subject_;
	const configuration					config_;
	std::unique_ptr<poco_consumer>		consumer_;
	core::interaction_sink*				sink_;

public:

	poco_consumer_proxy(const configuration& config, core::interaction_sink* sink)
		: config_(config)
		, sink_(sink)
	{
	}

	// frame_consumer

        void initialize(const core::video_format_desc& format_desc,
                        const core::audio_channel_layout&,
                        int                                     channel_index) override
        {
            consumer_.reset();
            consumer_.reset(new poco_consumer(config_, format_desc, channel_index, sink_));
        }

        int64_t presentation_frame_age_millis() const override
	{
		return consumer_ ? static_cast<int64_t>(consumer_->current_presentation_age_) : 0;
	}

	std::future<bool> send(core::frame_timecode timecode, core::const_frame frame) override
	{
		return consumer_->send(frame);
	}

	std::wstring print() const override
	{
		return consumer_ ? consumer_->print() : L"[poco_consumer]";
	}

	std::wstring name() const override
	{
		return L"poco";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"poco");
		info.add(L"uri", config_.URI);
		info.add(L"port", config_.port);
		info.add(L"push", config_.port);
		return info;
	}

	bool has_synchronization_clock() const override
	{
		return false;
	}

	int buffer_depth() const override
	{
		return 1;
	}

	int index() const override
	{
		return 600; // TODO what is this?
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}
};

void describe_consumer(core::help_sink& sink, const core::help_repository& repo)
{
	sink.short_description(L"Sends or serves the raw video contents of a channel using HTTP.");
	sink.syntax(
			L"POCO "
			L"{[uri:string]} "
			L"{PORT [port:int]} "
			L"{[push:PUSH]} ");
	sink.para()->text(L"Sends or serves the raw video contents of a channel using HTTP.");
	sink.definitions()
		->item(L"uri", L"HTTP address to PUSH content from or local path to serve stream from.")
		->item(L"port", L"Port to POST to or serve the stream from.")
		->item(L"push", L"Push the stream using HTTP POST rather than serving the stream for pulling.");
	sink.para()->text(L"Examples:");
	sink.example(L">> ADD 1 POCO http://otherserver.com/ PUSH", L"push the stream to otherserver.com on port 80");
	sink.example(L">> ADD 1 POCO casparc1/video PORT 3002", L"serve the stream at http://localhost:3002/casparc1/video");
}

spl::shared_ptr<core::frame_consumer> create_consumer(
		const std::vector<std::wstring>& params, core::interaction_sink* sink, std::vector<spl::shared_ptr<core::video_channel>> channels)
{
	if (params.size() < 1 || !boost::iequals(params.at(0), L"POCO"))
		return core::frame_consumer::empty();

	configuration config;

	if (params.size() > 1)
		config.URI = params.at(1);

	config.push =  !contains_param(L"PUSH", params);

	if (contains_param(L"PORT", params))
		config.port = boost::lexical_cast<int>(get_param(L"PORT", params));

	return spl::make_shared<poco_consumer_proxy>(config, sink);
}

spl::shared_ptr<core::frame_consumer> create_preconfigured_consumer(
		const boost::property_tree::wptree& ptree, core::interaction_sink* sink, std::vector<spl::shared_ptr<core::video_channel>> channels)
{
	configuration config;
	config.URI	= ptree.get(L"URI",	 config.URI);
	config.port = ptree.get(L"port", config.port);
	config.push	= ptree.get(L"push", config.push);

	return spl::make_shared<poco_consumer_proxy>(config, sink);
}

}}
