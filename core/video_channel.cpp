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

#include "StdAfx.h"

#include "video_channel.h"

#include "video_format.h"

#include "producer/stage.h"
#include "mixer/mixer.h"
#include "consumer/output.h"
#include "frame/data_frame.h"
#include "frame/frame_factory.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/concurrency/lock.h>
#include <common/concurrency/executor.h>

#include <tbb/spin_mutex.h>

#include <boost/property_tree/ptree.hpp>

#include <string>

namespace caspar { namespace core {

struct video_channel::impl sealed : public frame_factory
{
	reactive::basic_subject<spl::shared_ptr<const data_frame>> frame_subject_;
	spl::shared_ptr<monitor::subject>						   event_subject_;

	const int										index_;

	mutable tbb::spin_mutex							format_desc_mutex_;
	core::video_format_desc							format_desc_;
	
	const spl::shared_ptr<diagnostics::graph>		graph_;

	caspar::core::output							output_;
	caspar::core::mixer								mixer_;
	caspar::core::stage								stage_;	

	executor										executor_;
public:
	impl(int index, const core::video_format_desc& format_desc, spl::shared_ptr<image_mixer> image_mixer)  
		: event_subject_(new monitor::subject(monitor::path() % "channel" % index))
		, index_(index)
		, format_desc_(format_desc)
		, output_(graph_, format_desc, index)
		, mixer_(graph_, std::move(image_mixer))
		, stage_(graph_)
		, executor_(L"video_channel")
	{
		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));	
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		stage_.subscribe(event_subject_);

		executor_.begin_invoke([=]{tick();});

		CASPAR_LOG(info) << print() << " Successfully Initialized.";
	}
	
	// frame_factory
						
	virtual spl::shared_ptr<write_frame> create_frame(const void* tag, const core::pixel_format_desc& desc) override
	{		
		return mixer_.create_frame(tag, desc);
	}
	
	virtual core::video_format_desc video_format_desc() const
	{
		return lock(format_desc_mutex_, [&]
		{
			return format_desc_;
		});
	}
	
	// video_channel
	
	void video_format_desc(const core::video_format_desc& format_desc)
	{
		lock(format_desc_mutex_, [&]
		{
			format_desc_ = format_desc;
		});
	}

	void tick()
	{
		try
		{
			auto format_desc = video_format_desc();

			boost::timer frame_timer;

			// Produce
			
			auto stage_frames = stage_(format_desc);

			// Mix
			
			auto mixed_frame  = mixer_(std::move(stage_frames), format_desc);

			// Consume
			
			frame_subject_ << mixed_frame;
			
			output_(std::move(mixed_frame), format_desc);
		
			graph_->set_value("tick-time", frame_timer.elapsed()*format_desc.fps*0.5);

			*event_subject_ << monitor::event("debug/profiler")  % frame_timer.elapsed() % (1.0/format_desc_.fps);
			*event_subject_ << monitor::event("format")		% u8(format_desc.name);
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}

		executor_.begin_invoke([=]{tick();});
	}
			
	std::wstring print() const
	{
		return L"video_channel[" + boost::lexical_cast<std::wstring>(index_) + L"|" +  video_format_desc().name + L"]";
	}

	boost::property_tree::wptree info() const
	{
		boost::property_tree::wptree info;

		auto stage_info  = stage_.info();
		auto mixer_info  = mixer_.info();
		auto output_info = output_.info();

		info.add(L"video-mode", format_desc_.name);
		info.add_child(L"stage", stage_info.get());
		info.add_child(L"mixer", mixer_info.get());
		info.add_child(L"output", output_info.get());
   
		return info;			   
	}
};

video_channel::video_channel(int index, const core::video_format_desc& format_desc, spl::shared_ptr<image_mixer> image_mixer) : impl_(new impl(index, format_desc, image_mixer)){}
const stage& video_channel::stage() const { return impl_->stage_;} 
stage& video_channel::stage() { return impl_->stage_;} 
const mixer& video_channel::mixer() const{ return impl_->mixer_;} 
mixer& video_channel::mixer() { return impl_->mixer_;} 
const output& video_channel::output() const { return impl_->output_;} 
output& video_channel::output() { return impl_->output_;} 
spl::shared_ptr<frame_factory> video_channel::frame_factory() { return impl_;} 
core::video_format_desc video_channel::video_format_desc() const{return impl_->video_format_desc();}
void core::video_channel::video_format_desc(const core::video_format_desc& format_desc){impl_->video_format_desc(format_desc);}
boost::property_tree::wptree video_channel::info() const{return impl_->info();}
void video_channel::subscribe(const frame_observable::observer_ptr& o) {impl_->frame_subject_.subscribe(o);}
void video_channel::unsubscribe(const frame_observable::observer_ptr& o) {impl_->frame_subject_.unsubscribe(o);}		
void video_channel::subscribe(const monitor::observable::observer_ptr& o) {impl_->event_subject_->subscribe(o);}
void video_channel::unsubscribe(const monitor::observable::observer_ptr& o) {impl_->event_subject_->unsubscribe(o);}

}}