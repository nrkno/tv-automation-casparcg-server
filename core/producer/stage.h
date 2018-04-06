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

#pragma once

#include "../fwd.h"
#include "../interaction/interaction_sink.h"
#include "../monitor/monitor.h"

#include <common/executor.h>
#include <common/forward.h>
#include <common/future_fwd.h>
#include <common/memory.h>
#include <common/tweener.h>

#include <boost/optional.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

FORWARD2(caspar, diagnostics, class graph);

namespace caspar { namespace core {

/**
 * Base class for the stage. Should be used when either stage or stage_delayed may be used
 */
class stage_base : public interaction_sink
{
  public:
    // Static Members

    virtual ~stage_base() {}

    typedef std::function<struct frame_transform(struct frame_transform)> transform_func_t;
    typedef std::tuple<int, transform_func_t, unsigned int, tweener>      transform_tuple_t;

    // Methods

    virtual std::future<void> apply_transforms(const std::vector<transform_tuple_t>& transforms) = 0;
    virtual std::future<void>
                                         apply_transform(int index, const transform_func_t& transform, unsigned int mix_duration, const tweener& tween) = 0;
    virtual std::future<void>            clear_transforms(int index)      = 0;
    virtual std::future<void>            clear_transforms()               = 0;
    virtual std::future<frame_transform> get_current_transform(int index) = 0;
    virtual std::future<void>            load(int                                    index,
                                              const spl::shared_ptr<frame_producer>& producer,
                                              bool                                   preview = false,
                                              const boost::optional<int32_t>& auto_play_delta = boost::optional<int32_t>()) = 0;
    virtual std::future<void>            pause(int index)                                                 = 0;
    virtual std::future<void>            resume(int index)                                                = 0;
    virtual std::future<void>            play(int index)                                                  = 0;
    virtual std::future<void>            stop(int index)                                                  = 0;
    virtual std::future<std::wstring>    call(int index, const std::vector<std::wstring>& params)         = 0;
    virtual std::future<void>            clear(int index)                                                 = 0;
    virtual std::future<void>            clear()                                                          = 0;
    virtual std::future<void> swap_layers(const std::shared_ptr<stage_base>& other, bool swap_transforms) = 0;
    virtual std::future<void> swap_layer(int index, int other_index, bool swap_transforms)                = 0;
    virtual std::future<void>
    swap_layer(int index, int other_index, const std::shared_ptr<stage_base>& other, bool swap_transforms) = 0;

    // Properties

    virtual std::future<std::shared_ptr<frame_producer>> foreground(int index) = 0;
    virtual std::future<std::shared_ptr<frame_producer>> background(int index) = 0;

    virtual std::future<boost::property_tree::wptree> info()          = 0;
    virtual std::future<boost::property_tree::wptree> info(int index) = 0;

    virtual std::future<boost::property_tree::wptree> delay_info()          = 0;
    virtual std::future<boost::property_tree::wptree> delay_info(int layer) = 0;

    virtual std::future<void> execute(std::function<void()> k) = 0;
};

/**
 * The normal stage implementation.
 */
class stage : public stage_base
{
    stage(const stage&);
    stage& operator=(const stage&);

  public:
    // Static Members

    typedef std::function<struct frame_transform(struct frame_transform)> transform_func_t;
    typedef std::tuple<int, transform_func_t, unsigned int, tweener>      transform_tuple_t;

    // Constructors

    explicit stage(int channel_index, spl::shared_ptr<caspar::diagnostics::graph> graph);

    // Methods

    std::map<int, draw_frame> operator()(const video_format_desc& format_desc);
	void drop_frame(const video_format_desc& format_desc);

    std::future<void>            apply_transforms(const std::vector<transform_tuple_t>& transforms) override;
    std::future<void>            apply_transform(int                     index,
                                                 const transform_func_t& transform,
                                                 unsigned int            mix_duration,
                                                 const tweener&          tween) override;
    std::future<void>            clear_transforms(int index) override;
    std::future<void>            clear_transforms() override;
    std::future<frame_transform> get_current_transform(int index) override;
    std::future<void>            load(int                                    index,
                                      const spl::shared_ptr<frame_producer>& producer,
                                      bool                                   preview = false,
                                      const boost::optional<int32_t>& auto_play_delta = boost::optional<int32_t>()) override;
    std::future<void>            pause(int index) override;
    std::future<void>            resume(int index) override;
    std::future<void>            play(int index) override;
    std::future<void>            stop(int index) override;
    std::future<std::wstring>    call(int index, const std::vector<std::wstring>& params) override;
    std::future<void>            clear(int index) override;
    std::future<void>            clear() override;
    std::future<void>            swap_layers(const std::shared_ptr<stage_base>& other, bool swap_transforms) override;
    std::future<void>            swap_layer(int index, int other_index, bool swap_transforms) override;
    std::future<void>
    swap_layer(int index, int other_index, const std::shared_ptr<stage_base>& other, bool swap_transforms) override;

    void add_layer_consumer(void* token, int layer, const spl::shared_ptr<write_frame_consumer>& layer_consumer);
    void remove_layer_consumer(void* token, int layer);

    monitor::subject& monitor_output();

    // frame_observable
    // void subscribe(const frame_observable::observer_ptr& o) override;
    // void unsubscribe(const frame_observable::observer_ptr& o) override;

    // interaction_sink

    void on_interaction(const interaction_event::ptr& event) override;

    // Properties

    std::future<std::shared_ptr<frame_producer>> foreground(int index) override;
    std::future<std::shared_ptr<frame_producer>> background(int index) override;

    std::future<boost::property_tree::wptree> info() override;
    std::future<boost::property_tree::wptree> info(int index) override;

    std::future<boost::property_tree::wptree> delay_info() override;
    std::future<boost::property_tree::wptree> delay_info(int layer) override;

    std::future<void> execute(std::function<void()> k) override;

    std::unique_lock<std::mutex> get_lock() const;

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

/**
 * A stage wrapper, that queues up stage operations until release() is called.
 * This is useful for batching commands.
 */
class stage_delayed final : public stage_base
{
  public:
    stage_delayed(std::shared_ptr<stage>& st, int index);

    size_t count_queued() const { return executor_.size(); }
    void   release() { waiter_.set_value(); }
    void   abort(){ executor_.clear(); }
    void   wait()
    {
        executor_.stop();
        executor_.join();
    }

    std::unique_lock<std::mutex> get_lock() const { return stage_->get_lock(); }

    std::future<void>            apply_transforms(const std::vector<transform_tuple_t>& transforms) override;
    std::future<void>            apply_transform(int                     index,
                                                 const transform_func_t& transform,
                                                 unsigned int            mix_duration,
                                                 const tweener&          tween) override;
    std::future<void>            clear_transforms(int index) override;
    std::future<void>            clear_transforms() override;
    std::future<frame_transform> get_current_transform(int index) override;
    std::future<void>            load(int                                    index,
                                      const spl::shared_ptr<frame_producer>& producer,
                                      bool                                   preview = false,
                                      const boost::optional<int32_t>& auto_play_delta = boost::optional<int32_t>()) override;
    std::future<void>            pause(int index) override;
    std::future<void>            resume(int index) override;
    std::future<void>            play(int index) override;
    std::future<void>            stop(int index) override;
    std::future<std::wstring>    call(int index, const std::vector<std::wstring>& params) override;
    std::future<void>            clear(int index) override;
    std::future<void>            clear() override;
    std::future<void>            swap_layers(const std::shared_ptr<stage_base>& other, bool swap_transforms) override;
    std::future<void>            swap_layer(int index, int other_index, bool swap_transforms) override;
    std::future<void>
    swap_layer(int index, int other_index, const std::shared_ptr<stage_base>& other, bool swap_transforms) override;

    // interaction_sink

    void on_interaction(const interaction_event::ptr& event) override { stage_->on_interaction(event); }

    // Properties

    std::future<std::shared_ptr<frame_producer>> foreground(int index) override;
    std::future<std::shared_ptr<frame_producer>> background(int index) override;

    std::future<boost::property_tree::wptree> info() override;
    std::future<boost::property_tree::wptree> info(int index) override;

    std::future<boost::property_tree::wptree> delay_info() override;
    std::future<boost::property_tree::wptree> delay_info(int layer) override;

    std::future<void> execute(std::function<void()> k) override;

  private:
    std::promise<void>      waiter_;
    std::shared_ptr<stage>& stage_;
    executor                executor_;
};

}} // namespace caspar::core