#include "../StdAfx.h"

#include "port.h"

#include "frame_consumer.h"
#include "../frame/frame.h"
#include <boost/lexical_cast.hpp>

#include <future>

namespace caspar { namespace core {

struct port::impl
{
	int									index_;
	spl::shared_ptr<monitor::subject>	monitor_subject_ = spl::make_shared<monitor::subject>("/port/" + boost::lexical_cast<std::string>(index_));
	spl::shared_ptr<frame_consumer>		consumer_;
	int									channel_index_;
public:
	impl(int index, int channel_index, spl::shared_ptr<frame_consumer> consumer)
		: index_(index)
		, consumer_(std::move(consumer))
		, channel_index_(channel_index)
	{
		consumer_->monitor_output().attach_parent(monitor_subject_);
	}

	void change_channel_format(const core::video_format_desc&           format_desc,
                                   const audio_channel_layout&              channel_layout)
	{
		consumer_->initialize(format_desc, channel_layout, channel_index_);
	}

	std::future<bool> send(frame_timecode timecode, const_frame frame)
	{
		*monitor_subject_ << monitor::message("/type") % consumer_->name();
		return consumer_->send(timecode, std::move(frame));
	}
	std::wstring print() const
	{
		return consumer_->print();
	}

	int index() const
	{
		return index_;
	}

	int buffer_depth() const
	{
		return consumer_->buffer_depth();
	}

	bool has_synchronization_clock() const
	{
		return consumer_->has_synchronization_clock();
	}

        std::future<bool> ready_to_send() const
        {
            return consumer_->ready_to_send();
        }

        int dropped_last_frame() const
	{
		return consumer_->dropped_last_frame();
	}

	boost::property_tree::wptree info() const
	{
		return consumer_->info();
	}

	int64_t presentation_frame_age_millis() const
	{
		return consumer_->presentation_frame_age_millis();
	}

	spl::shared_ptr<const frame_consumer> consumer() const
	{
		return consumer_;
	}
};

port::port(int index, int channel_index, spl::shared_ptr<frame_consumer> consumer) : impl_(new impl(index, channel_index, std::move(consumer))){}
port::port(port&& other) : impl_(std::move(other.impl_)){}
port::~port(){}
port& port::operator=(port&& other){impl_ = std::move(other.impl_); return *this;}
std::future<bool> port::send(frame_timecode timecode, const_frame frame) { return impl_->send(timecode, std::move(frame)); }
monitor::subject& port::monitor_output() { return *impl_->monitor_subject_; }
void              port::change_channel_format(const core::video_format_desc&          format_desc,
                                 const audio_channel_layout&             channel_layout)
{
    impl_->change_channel_format(format_desc, channel_layout);
}
int                          port::buffer_depth() const { return impl_->buffer_depth(); }
std::wstring port::print() const{ return impl_->print();}
bool port::has_synchronization_clock() const { return impl_->has_synchronization_clock(); }
std::future<bool> port::ready_to_send() const { return impl_->ready_to_send(); }
int port::dropped_last_frame() const { return impl_->dropped_last_frame(); }
boost::property_tree::wptree port::info() const{return impl_->info();}
int64_t port::presentation_frame_age_millis() const{ return impl_->presentation_frame_age_millis(); }
spl::shared_ptr<const frame_consumer> port::consumer() const { return impl_->consumer(); }
}}
