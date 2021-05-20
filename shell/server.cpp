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

#include "stdafx.h"

#include "default_audio_config.h"
#include "included_modules.h"
#include "server.h"

#include <accelerator/accelerator.h>
#include <accelerator/ogl/util/device.h>

#include <common/env.h>
#include <common/except.h>
#include <common/memory.h>
#include <common/polling_filesystem_monitor.h>
#include <common/ptree.h>
#include <common/utf.h>

#include <core/consumer/output.h>
#include <core/consumer/syncto/syncto_consumer.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/graph_to_log_sink.h>
#include <core/diagnostics/osd_graph.h>
#include <core/diagnostics/subject_diagnostics.h>
#include <core/frame/audio_channel_layout.h>
#include <core/help/help_repository.h>
#include <core/mixer/image/image_mixer.h>
#include <core/mixer/mixer.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/color/color_producer.h>
#include <core/producer/frame_producer.h>
#include <core/producer/media_info/in_memory_media_info_repository.h>
#include <core/producer/media_info/media_info.h>
#include <core/producer/media_info/media_info_repository.h>
#include <core/producer/scene/scene_producer.h>
#include <core/producer/scene/xml_scene_producer.h>
#include <core/producer/stage.h>
#include <core/producer/text/text_producer.h>
#include <core/system_info_provider.h>
#include <core/thumbnail_generator.h>
#include <core/video_channel.h>
#include <core/video_format.h>

#include <modules/image/consumer/image_consumer.h>

#include <protocol/amcp/AMCPCommandScheduler.h>
#include <protocol/amcp/AMCPCommandsImpl.h>
#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/amcp/amcp_command_repository.h>
#include <protocol/cii/CIIProtocolStrategy.h>
#include <protocol/clk/CLKProtocolStrategy.h>
#include <protocol/log/tcp_logger_protocol_strategy.h>
#include <protocol/osc/client.h>
#include <protocol/util/AsyncEventServer.h>
#include <protocol/util/strategy_adapters.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/thread.hpp>

#include "protocol/util/tokenize.h"
#include <boost/format.hpp>
#include <future>

namespace caspar {

using namespace core;
using namespace protocol;

std::shared_ptr<boost::asio::io_service> create_running_io_service()
{
    auto service = std::make_shared<boost::asio::io_service>();
    // To keep the io_service::run() running although no pending async
    // operations are posted.
    auto work      = std::make_shared<boost::asio::io_service::work>(*service);
    auto weak_work = std::weak_ptr<boost::asio::io_service::work>(work);
    auto thread    = std::make_shared<boost::thread>([service, weak_work] {
        ensure_gpf_handler_installed_for_thread("asio-thread");

        while (auto strong = weak_work.lock()) {
            try {
                service->run();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }

        CASPAR_LOG(info) << "[asio] Global io_service uninitialized.";
    });

    return std::shared_ptr<boost::asio::io_service>(service.get(), [service, work, thread](void*) mutable {
        CASPAR_LOG(info) << "[asio] Shutting down global io_service.";
        work.reset();
        service->stop();
        if (thread->get_id() != boost::this_thread::get_id())
            thread->join();
        else
            thread->detach();
    });
}

struct server::impl : boost::noncopyable
{
    std::shared_ptr<boost::asio::io_service>               io_service_ = create_running_io_service();
    spl::shared_ptr<monitor::subject>                      monitor_subject_;
    spl::shared_ptr<monitor::subject>                      diag_subject_ = core::diagnostics::get_or_create_subject();
	video_format_repository								   video_format_repository_;
    accelerator::accelerator                               accelerator_;
    spl::shared_ptr<help_repository>                       help_repo_;
    std::shared_ptr<amcp::amcp_command_repository>         amcp_command_repo_;
    std::shared_ptr<amcp::amcp_command_repository_wrapper> amcp_command_repo_wrapper_;
    std::shared_ptr<amcp::command_context_factory>         amcp_context_factory_;
    std::shared_ptr<amcp::AMCPCommandScheduler>            amcp_command_scheduler_;
    std::vector<spl::shared_ptr<IO::AsyncEventServer>>     async_servers_;
    std::shared_ptr<IO::AsyncEventServer>                  primary_amcp_server_;
    std::shared_ptr<osc::client>                           osc_client_ = std::make_shared<osc::client>(io_service_);
    std::vector<std::shared_ptr<void>>                     predefined_osc_subscriptions_;
    std::vector<spl::shared_ptr<video_channel>>            channels_;
    spl::shared_ptr<media_info_repository>                 media_info_repo_;
    boost::thread                                          initial_media_info_thread_;
    spl::shared_ptr<system_info_provider_repository>       system_info_provider_repo_;
    spl::shared_ptr<core::cg_producer_registry>            cg_registry_;
    spl::shared_ptr<core::frame_producer_registry>         producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry>         consumer_registry_;
    std::atomic<bool>                                      running_;
    std::shared_ptr<thumbnail_generator>                   thumbnail_generator_;
    std::promise<bool>&                                    shutdown_server_now_;

    explicit impl(std::promise<bool>& shutdown_server_now)
        : video_format_repository_()
		, accelerator_(env::properties().get(L"configuration.accelerator", L"auto"), video_format_repository_)
        , media_info_repo_(create_in_memory_media_info_repository())
        , producer_registry_(spl::make_shared<core::frame_producer_registry>(help_repo_))
        , consumer_registry_(spl::make_shared<core::frame_consumer_registry>(help_repo_))
        , shutdown_server_now_(shutdown_server_now)
    {
        running_ = false;
        core::diagnostics::register_graph_to_log_sink();
        caspar::core::diagnostics::osd::register_sink();
        diag_subject_->attach_parent(monitor_subject_);

        module_dependencies dependencies(
            system_info_provider_repo_, cg_registry_, media_info_repo_, producer_registry_, consumer_registry_);

        initialize_modules(dependencies);
        core::text::init(dependencies);
        core::init_cg_proxy_as_producer(dependencies);
        core::scene::init(dependencies);
        core::syncto::init(dependencies);
        help_repo_->register_item({L"producer"}, L"Color Producer", &core::describe_color_producer);
    }

    void start()
    {
        running_ = true;

		setup_video_modes(env::properties());
		CASPAR_LOG(info) << L"Initialized video modes.";

        setup_audio_config(env::properties());
        CASPAR_LOG(info) << L"Initialized audio config.";

        auto xml_channels = setup_channels(env::properties());
        CASPAR_LOG(info) << L"Initialized channels.";

		preallocate_buffers(env::properties());

        setup_thumbnail_generation(env::properties());
        CASPAR_LOG(info) << L"Initialized thumbnail generator.";

        setup_amcp_command_repo();
        CASPAR_LOG(info) << L"Initialized command repository.";

        setup_channel_producers(xml_channels);
        CASPAR_LOG(info) << L"Initialized channel predefined producers.";

        setup_controllers(env::properties());
        CASPAR_LOG(info) << L"Initialized controllers.";

        setup_osc(env::properties());
        CASPAR_LOG(info) << L"Initialized osc.";

        start_initial_media_info_scan();
        CASPAR_LOG(info) << L"Started initial media information retrieval.";
    }

    ~impl()
    {
        if (running_) {
            running_ = false;
            initial_media_info_thread_.join();
        }

        std::weak_ptr<boost::asio::io_service> weak_io_service = io_service_;
        io_service_.reset();
        osc_client_.reset();
        thumbnail_generator_.reset();
        amcp_command_repo_wrapper_.reset();
        amcp_command_repo_.reset();
        amcp_context_factory_.reset();
        amcp_command_scheduler_.reset();
        primary_amcp_server_.reset();
        async_servers_.clear();
        destroy_producers_synchronously();
        destroy_consumers_synchronously();
        channels_.clear();

        while (weak_io_service.lock())
            boost::this_thread::sleep_for(boost::chrono::milliseconds(100));

        uninitialize_modules();
        core::diagnostics::osd::shutdown();
    }

	void setup_video_modes(const boost::property_tree::wptree& pt)
	{
		using boost::property_tree::wptree;

		auto videomodes_config = pt.get_child_optional(L"configuration.video-modes");
		if (videomodes_config) {
			for (auto& xml_channel : pt | witerate_children(L"configuration.video-modes") | welement_context_iteration)
			{
				ptree_verify_element_name(xml_channel, L"video-mode");

				const std::wstring id = xml_channel.second.get(L"id", L"");
				if (id == L"")
					CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode id: " + id));

				const int width = xml_channel.second.get<int>(L"width", 0);
				const int height = xml_channel.second.get<int>(L"height", 0);
				if (width == 0 || height == 0)
					CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid dimensions: " + boost::lexical_cast<std::wstring>(width) + L"x" + boost::lexical_cast<std::wstring>(height)));

				const int timescale = xml_channel.second.get<int>(L"time-scale", 60000);
				const int duration = xml_channel.second.get<int>(L"duration", 1000);
				if (timescale == 0 || duration == 0)
					CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid framerate: " + boost::lexical_cast<std::wstring>(timescale) + L"/" + boost::lexical_cast<std::wstring>(duration)));

				std::vector<int> cadence;
				int cadence_sum = 0;

				const std::wstring cadence_str = xml_channel.second.get(L"cadence", L"");
				std::set<std::wstring> cadence_parts;
				boost::split(cadence_parts, cadence_str, boost::is_any_of(L", "));

				for (auto& cad : cadence_parts)
				{
					if (cad == L"")
						continue;

					const int c = std::stoi(cad);
					cadence.push_back(c);
					cadence_sum += c;
				}

				if (cadence.size() == 0)
				{
					const int c = static_cast<int>(48000 / (static_cast<double>(timescale) / duration) + 0.5);
					cadence.push_back(c);
					cadence_sum += c;
				}

				const auto new_format = video_format_desc(video_format::custom, width, height, width, height, field_mode::progressive, timescale, duration, id, cadence); // TODO - fields, cadence
	//			if (cadence_sum != new_format.audio_sample_rate)
	//				CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Audio cadence sum doesn't match sample rate. "));

				const auto existing = video_format_repository_.find(id);
				if (existing.format != video_format::invalid)
					CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Video-mode already exists: " + id));

				video_format_repository_.store(new_format);
			}
		}
	}

	void preallocate_buffers(const boost::property_tree::wptree& pt)
	{
		using boost::property_tree::wptree;

		auto device = accelerator_.get_ogl_device();
		auto prealloc_config = pt.get_child_optional(L"configuration.opengl.preallocate");
		if (device && prealloc_config) {
			std::vector<caspar::array<uint8_t>> buffers;
			std::vector<std::shared_ptr<accelerator::ogl::texture>> textures;

			int allocation_count = 0;

			for (auto& xml_preallocate : pt | witerate_children(L"configuration.opengl.preallocate") | welement_context_iteration) {
				const auto attrs = xml_preallocate.second.get_child(L"<xmlattr>");

				const auto width = attrs.get(L"width", 0);
				const auto height = attrs.get(L"height", 0);
				const auto depth = attrs.get(L"depth", 0);
				const auto count = attrs.get(L"count", 0);
				const auto mipmapped = attrs.get(L"mipmapped", false);

				if (width == 0 || height == 0 || depth == 0) {
					CASPAR_LOG(warning) << L"Invalid preallocated buffer size: " << width << L"x" << height << L"(" << depth << L")";
				} else if (count > 0) {
					device->allocate_buffers(count, width, height, depth, mipmapped, false);
					allocation_count += count;
				}
			}

			// TODO - optional/configurable?
			for (auto& ch : channels_) {
				device->allocate_buffers(10, ch->video_format_desc().width, ch->video_format_desc().height, 4, false, true);
				allocation_count += 10;
			}

			CASPAR_LOG(info) << L"Preallocated " << allocation_count << L" buffers";
		}
	}

    void setup_audio_config(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        auto default_config = get_default_audio_config();

        // Start with the defaults
        audio_channel_layout_repository::get_default()->register_all_layouts(
            default_config.get_child(L"audio.channel-layouts"));
        audio_mix_config_repository::get_default()->register_all_configs(
            default_config.get_child(L"audio.mix-configs"));

        // Merge with user configuration (adds to or overwrites the defaults)
        auto custom_channel_layouts = pt.get_child_optional(L"configuration.audio.channel-layouts");
        auto custom_mix_configs     = pt.get_child_optional(L"configuration.audio.mix-configs");

        if (custom_channel_layouts) {
            CASPAR_SCOPED_CONTEXT_MSG("/configuration/audio/channel-layouts");
            audio_channel_layout_repository::get_default()->register_all_layouts(*custom_channel_layouts);
        }

        if (custom_mix_configs) {
            CASPAR_SCOPED_CONTEXT_MSG("/configuration/audio/mix-configs");
            audio_mix_config_repository::get_default()->register_all_configs(*custom_mix_configs);
        }
    }

    std::vector<boost::property_tree::wptree> setup_channels(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        std::vector<wptree> xml_channels;

        for (auto& xml_channel : pt | witerate_children(L"configuration.channels") | welement_context_iteration) {
            xml_channels.push_back(xml_channel.second);
            ptree_verify_element_name(xml_channel, L"channel");

            auto format_desc_str = xml_channel.second.get(L"video-mode", L"PAL");
            auto format_desc     = video_format_repository_.find(format_desc_str);
            if (format_desc.format == video_format::invalid)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode: " + format_desc_str));

            auto channel_layout_str = xml_channel.second.get(L"channel-layout", L"stereo");
            auto channel_layout = core::audio_channel_layout_repository::get_default()->get_layout(channel_layout_str);
            if (!channel_layout)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Unknown channel-layout: " + channel_layout_str));

            auto channel_id = static_cast<int>(channels_.size() + 1);
            auto channel    = spl::make_shared<video_channel>(
                channel_id, format_desc, *channel_layout, accelerator_.create_image_mixer(channel_id));

            channel->monitor_output().attach_parent(monitor_subject_);
            channel->mixer().set_straight_alpha_output(xml_channel.second.get(L"straight-alpha-output", false));
            channels_.push_back(channel);
        }

        for (auto& channel : channels_) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel->index();

            for (auto& xml_consumer :
                 xml_channels.at(channel->index() - 1) | witerate_children(L"consumers") | welement_context_iteration) {
                auto name = xml_consumer.first;

                try {
                    if (name != L"<xmlcomment>")
                        channel->output().add(consumer_registry_->create_consumer(
                            name, xml_consumer.second, channel->stage().get(), channels_));
                } catch (const user_error& e) {
                    CASPAR_LOG_CURRENT_EXCEPTION_AT_LEVEL(debug);
                    CASPAR_LOG(error) << get_message_and_context(e) << " Turn on log level debug for stacktrace.";
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
            }
        }

        // Dummy diagnostics channel
        if (env::properties().get(L"configuration.channel-grid", false)) {
            auto channel_id = static_cast<int>(channels_.size() + 1);
            channels_.push_back(spl::make_shared<video_channel>(
                channel_id,
                video_format_repository_.find_format(core::video_format::x576p2500),
                *core::audio_channel_layout_repository::get_default()->get_layout(L"stereo"),
                accelerator_.create_image_mixer(channel_id)));
            channels_.back()->monitor_output().attach_parent(monitor_subject_);
        }

        return xml_channels;
    }

    void setup_osc(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        using namespace boost::asio::ip;

        monitor_subject_->attach_parent(osc_client_->sink());

        auto default_port                 = pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
        auto disable_send_to_amcp_clients = pt.get(L"configuration.osc.disable-send-to-amcp-clients", false);
        auto predefined_clients           = pt.get_child_optional(L"configuration.osc.predefined-clients");

        if (predefined_clients) {
            for (auto& predefined_client :
                 pt | witerate_children(L"configuration.osc.predefined-clients") | welement_context_iteration) {
                ptree_verify_element_name(predefined_client, L"predefined-client");

                const auto address = ptree_get<std::wstring>(predefined_client.second, L"address");
                const auto port    = ptree_get<unsigned short>(predefined_client.second, L"port");

                boost::system::error_code ec;
                auto                      ipaddr = address_v4::from_string(u8(address), ec);
                if (!ec)
                    predefined_osc_subscriptions_.push_back(
                        osc_client_->get_subscription_token(udp::endpoint(ipaddr, port)));
                else
                    CASPAR_LOG(warning) << "Invalid OSC client. Must be valid ipv4 address: " << address;
            }
        }

        if (!disable_send_to_amcp_clients && primary_amcp_server_)
            primary_amcp_server_->add_client_lifecycle_object_factory(
                [=](const std::string& ipv4_address) -> std::pair<std::wstring, std::shared_ptr<void>> {
                    using namespace boost::asio::ip;

                    return std::make_pair(std::wstring(L"osc_subscribe"),
                                          osc_client_->get_subscription_token(
                                              udp::endpoint(address_v4::from_string(ipv4_address), default_port)));
                });
    }

    void setup_thumbnail_generation(const boost::property_tree::wptree& pt)
    {
        if (!pt.get(L"configuration.thumbnails.generate-thumbnails", true))
            return;

        auto scan_interval_millis = pt.get(L"configuration.thumbnails.scan-interval-millis", 5000);

        polling_filesystem_monitor_factory monitor_factory(io_service_, scan_interval_millis);
        thumbnail_generator_.reset(new thumbnail_generator(
            monitor_factory,
            env::media_folder(),
            env::thumbnail_folder(),
            pt.get(L"configuration.thumbnails.width", 256),
            pt.get(L"configuration.thumbnails.height", 144),
			video_format_repository_,
            video_format_repository_.find(pt.get(L"configuration.thumbnails.video-mode", L"720p2500")),
            accelerator_.create_image_mixer(0),
            pt.get(L"configuration.thumbnails.generate-delay-millis", 2000),
            &image::write_cropped_png,
            media_info_repo_,
            producer_registry_,
            cg_registry_,
            pt.get(L"configuration.thumbnails.mipmap", true)));
    }

    static std::vector<amcp::channel_context>
    build_channel_contexts(const std::vector<spl::shared_ptr<core::video_channel>>& channels)
    {
        std::vector<amcp::channel_context> res;

        int index = 0;
        for (const auto& channel : channels) {
            const std::wstring lifecycle_key = L"lock" + std::to_wstring(index);
            res.emplace_back(channel, channel->stage(), lifecycle_key);
            ++index;
        }

        return res;
    }

    void setup_channel_producers(const std::vector<boost::property_tree::wptree>& xml_channels)
    {
        auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

        for (auto& channel : channels_) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel->index();

            auto xml_channel = xml_channels.at(channel->index() - 1);

            if (xml_channel.get_child_optional(L"producers")) {
                for (auto& xml_producer : xml_channel | witerate_children(L"producers") | welement_context_iteration) {
                    ptree_verify_element_name(xml_producer, L"producer");

                    const std::wstring command = xml_producer.second.get_value(L"");
                    const auto         attrs   = xml_producer.second.get_child(L"<xmlattr>");
                    const int id      = attrs.get(L"id", -1);
                    
                    try {

                        std::list<std::wstring> tokens{L"PLAY", (boost::wformat(L"%i-%i") % channel->index() % id).str()};
                        IO::tokenize(command, tokens);
                        auto cmd = amcp_command_repo_->parse_command(console_client, tokens, L"");

                        std::wstring res = cmd->Execute(amcp_command_repo_->channels()).get();
                        console_client->send(std::move(res), false);
                    } catch (const user_error& e) {
                        CASPAR_LOG_CURRENT_EXCEPTION_AT_LEVEL(debug);
                        CASPAR_LOG(error) << get_message_and_context(e) << " Turn on log level debug for stacktrace.";
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }

            const std::wstring source = xml_channel.get(L"timecode", L"free");
            if (boost::iequals(source, "clock")) {
                channel->timecode()->set_system_time(); // TODO - offset/timezone
            } else if (boost::iequals(source, "layer")) {
                const int layer = xml_channel.get(L"timecode_layer", 0);

                // Run it on the stage to ensure the producer creation has completed fully
                channel->stage()->execute([channel, layer]()
                {
                    const std::shared_ptr<frame_producer> producer = channel->stage()->foreground(layer).get();
                    if (!channel->timecode()->set_weak_source(producer)) {
                        CASPAR_LOG(error)
                            << L"timecode[" << channel->index() << L"] failed to set timecode from layer " << layer;
                    }
                });
            } else {
                channel->timecode()->clear_source();
            }
        }
    }

    void setup_amcp_command_repo()
    {
        amcp_command_scheduler_ = std::make_shared<amcp::AMCPCommandScheduler>();

        amcp_command_repo_ =
            std::make_shared<amcp::amcp_command_repository>(build_channel_contexts(channels_), help_repo_);

        auto ctx = std::make_shared<amcp::amcp_command_static_context>(thumbnail_generator_,
                                                                       media_info_repo_,
                                                                       system_info_provider_repo_,
                                                                       video_format_repository_,
                                                                       cg_registry_,
                                                                       help_repo_,
                                                                       producer_registry_,
                                                                       consumer_registry_,
                                                                       spl::make_shared_ptr(amcp_command_scheduler_),
                                                                       amcp_command_repo_,
                                                                       accelerator_.get_ogl_device(),
                                                                       shutdown_server_now_);

        amcp_context_factory_ = std::make_shared<amcp::command_context_factory>(ctx);

        amcp_command_repo_wrapper_ =
            std::make_shared<amcp::amcp_command_repository_wrapper>(amcp_command_repo_, amcp_context_factory_);

        amcp::register_commands(amcp_command_repo_wrapper_);
    }

    void setup_controllers(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        for (auto& xml_controller : pt | witerate_children(L"configuration.controllers") | welement_context_iteration) {
            auto name     = xml_controller.first;
            auto protocol = ptree_get<std::wstring>(xml_controller.second, L"protocol");

            if (name == L"tcp") {
                auto port              = ptree_get<unsigned int>(xml_controller.second, L"port");
                auto asyncbootstrapper = spl::make_shared<IO::AsyncEventServer>(
                    io_service_,
                    create_protocol(protocol, L"TCP Port " + boost::lexical_cast<std::wstring>(port)),
                    port);
                async_servers_.push_back(asyncbootstrapper);

                if (!primary_amcp_server_ && boost::iequals(protocol, L"AMCP"))
                    primary_amcp_server_ = asyncbootstrapper;
            } else
                CASPAR_LOG(warning) << "Invalid controller: " << name;
        }
    }

    IO::protocol_strategy_factory<char>::ptr create_protocol(const std::wstring& name,
                                                             const std::wstring& port_description) const
    {
        using namespace IO;

        if (boost::iequals(name, L"AMCP"))
            return amcp::create_char_amcp_strategy_factory(port_description,
                                                           spl::make_shared_ptr(amcp_command_repo_),
                                                           spl::make_shared_ptr(amcp_command_scheduler_));
        else if (boost::iequals(name, L"CII"))
            return wrap_legacy_protocol(
                "\r\n", spl::make_shared<cii::CIIProtocolStrategy>(video_format_repository_, channels_, cg_registry_, producer_registry_));
        else if (boost::iequals(name, L"CLOCK"))
            return spl::make_shared<to_unicode_adapter_factory>(
                "ISO-8859-1",
                spl::make_shared<CLK::clk_protocol_strategy_factory>(video_format_repository_, channels_, cg_registry_, producer_registry_));
        else if (boost::iequals(name, L"LOG"))
            return spl::make_shared<protocol::log::tcp_logger_protocol_strategy_factory>();

        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid protocol: " + name));
    }

    void start_initial_media_info_scan()
    {
        initial_media_info_thread_ = boost::thread([this] {
            try {
                ensure_gpf_handler_installed_for_thread("initial media scan");

                for (boost::filesystem::wrecursive_directory_iterator iter(env::media_folder()), end; iter != end;
                     ++iter) {
                    if (running_) {
                        if (boost::filesystem::is_regular_file(iter->path())) {
                            CASPAR_LOG(trace) << L"Retrieving information for file " << iter->path().wstring();
                            media_info_repo_->get(iter->path().wstring());
                        }
                    } else {
                        CASPAR_LOG(info) << L"Initial media information retrieval aborted.";
                        return;
                    }
                }

                CASPAR_LOG(info) << L"Initial media information retrieval finished.";
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }
};

server::server(std::promise<bool>& shutdown_server_now)
    : impl_(new impl(shutdown_server_now))
{
}
void                                                   server::start() { impl_->start(); }
spl::shared_ptr<core::system_info_provider_repository> server::get_system_info_provider_repo() const
{
    return impl_->system_info_provider_repo_;
}
spl::shared_ptr<protocol::amcp::amcp_command_repository> server::get_amcp_command_repository() const
{
    return spl::make_shared_ptr(impl_->amcp_command_repo_);
}
spl::shared_ptr<amcp::AMCPCommandScheduler> server::get_amcp_command_scheduler() const
{
    return spl::make_shared_ptr(impl_->amcp_command_scheduler_);
}
core::monitor::subject& server::monitor_output() { return *impl_->monitor_subject_; }
} // namespace caspar
