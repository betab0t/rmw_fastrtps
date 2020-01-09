// Copyright 2019 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rmw_fastrtps_dynamic_cpp/register_node.hpp"

#include <cassert>
#include <memory>

#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/qos_profiles.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"

#include "rmw_fastrtps_dynamic_cpp/identifier.hpp"
#include "rmw_fastrtps_dynamic_cpp/publisher.hpp"
#include "rmw_fastrtps_dynamic_cpp/subscription.hpp"

#include "rmw_fastrtps_shared_cpp/custom_participant_info.hpp"
#include "rmw_fastrtps_shared_cpp/participant.hpp"
#include "rmw_fastrtps_shared_cpp/publisher.hpp"
#include "rmw_fastrtps_shared_cpp/subscription.hpp"
#include "rmw_fastrtps_shared_cpp/rmw_context_impl.h"

#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "listener_thread.hpp"

using rmw_dds_common::msg::ParticipantEntitiesInfo;

static
rmw_ret_t
init_context_impl(rmw_context_t * context)
{
  rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
  rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();

  // This is currently not implemented in fastrtps
  subscription_options.ignore_local_publications = true;

  std::unique_ptr<rmw_dds_common::Context> common_context(
    new(std::nothrow) rmw_dds_common::Context());
  if (!common_context) {
    return RMW_RET_BAD_ALLOC;
  }

  std::unique_ptr<CustomParticipantInfo, std::function<void(CustomParticipantInfo *)>>
  participant_info(
    rmw_fastrtps_shared_cpp::create_participant(
      eprosima_fastrtps_identifier,
      context->options.domain_id,
      &context->options.security_options,
      (context->options.localhost_only == RMW_LOCALHOST_ONLY_ENABLED) ? 1 : 0,
      common_context.get()),
    [&](CustomParticipantInfo * participant_info) {
      if (RMW_RET_OK != rmw_fastrtps_shared_cpp::destroy_participant(participant_info)) {
        fprintf(
          stderr, "Failed to destroy participant after function: '"
          RCUTILS_STRINGIFY(__function__) "' failed.\n");
      }
    });
  if (!participant_info) {
    return RMW_RET_BAD_ALLOC;
  }

  rmw_qos_profile_t qos = rmw_qos_profile_default;

  qos.avoid_ros_namespace_conventions = false;  // Change this to true after testing
  qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos.depth = 1;
  qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;

  std::unique_ptr<rmw_publisher_t, std::function<void(rmw_publisher_t *)>>
  publisher(
    rmw_fastrtps_dynamic_cpp::create_publisher(
      participant_info.get(),
      rosidl_typesupport_cpp::get_message_type_support_handle<ParticipantEntitiesInfo>(),
      "_participant_info",
      &qos,
      &publisher_options,
      false,  // our fastrtps typesupport doesn't support keyed topics
      true),
    [&](rmw_publisher_t * pub) {
      if (RMW_RET_OK != rmw_fastrtps_shared_cpp::destroy_publisher(
        eprosima_fastrtps_identifier,
        participant_info.get(),
        pub))
      {
        fprintf(
          stderr, "Failed to destroy publisher after function: '"
          RCUTILS_STRINGIFY(__function__) "' failed.\n");
      }
    });
  if (!publisher) {
    return RMW_RET_BAD_ALLOC;
  }

  // If we would have support for keyed topics, this could be KEEP_LAST and depth 1.
  qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
  std::unique_ptr<rmw_subscription_t, std::function<void(rmw_subscription_t *)>>
  subscription(
    rmw_fastrtps_dynamic_cpp::create_subscription(
      participant_info.get(),
      rosidl_typesupport_cpp::get_message_type_support_handle<ParticipantEntitiesInfo>(),
      "_participant_info",
      &qos,
      &subscription_options,
      false,  // our fastrtps typesupport doesn't support keyed topics
      true),
    [&](rmw_subscription_t * sub) {
      if (RMW_RET_OK != rmw_fastrtps_shared_cpp::destroy_subscription(
        eprosima_fastrtps_identifier,
        participant_info.get(),
        sub))
      {
        fprintf(
          stderr, "Failed to destroy subscription after function: '"
          RCUTILS_STRINGIFY(__function__) "' failed.\n");
      }
    });
  if (!subscription) {
    return RMW_RET_BAD_ALLOC;
  }

  common_context->gid = rmw_fastrtps_shared_cpp::create_rmw_gid(
    eprosima_fastrtps_identifier, participant_info->participant->getGuid());
  common_context->pub = publisher.get();
  common_context->sub = subscription.get();

  context->impl->common = common_context.get();
  context->impl->participant_info = participant_info.get();

  if (RMW_RET_OK != rmw_fastrtps_dynamic_cpp::run_listener_thread(context)) {
    return RMW_RET_ERROR;
  }
  common_context->graph_cache.add_participant(common_context->gid);

  publisher.release();
  subscription.release();
  common_context.release();
  participant_info.release();
  return RMW_RET_OK;
}

rmw_ret_t
rmw_fastrtps_dynamic_cpp::register_node(rmw_context_t * context)
{
  assert(context);
  assert(context->impl);

  std::lock_guard<std::mutex> guard(context->impl->mutex);

  if (!context->impl->count) {
    rmw_ret_t ret = init_context_impl(context);
    if (RMW_RET_OK != ret) {
      return ret;
    }
  }
  context->impl->count++;
  return RMW_RET_OK;
}

rmw_ret_t
rmw_fastrtps_dynamic_cpp::unregister_node(rmw_context_t * context)
{
  assert(context);
  assert(context->impl);

  std::lock_guard<std::mutex> guard(context->impl->mutex);
  if (0u == --context->impl->count) {
    rmw_ret_t ret;
    ret = rmw_fastrtps_dynamic_cpp::join_listener_thread(context);
    if (RMW_RET_OK != ret) {
      return ret;
    }

    auto common_context = static_cast<rmw_dds_common::Context *>(context->impl->common);
    auto participant_info = static_cast<CustomParticipantInfo *>(context->impl->participant_info);

    common_context->graph_cache.remove_participant(common_context->gid);
    ret = rmw_fastrtps_shared_cpp::destroy_subscription(
      eprosima_fastrtps_identifier,
      participant_info,
      common_context->sub);
    if (RMW_RET_OK != ret) {
      return ret;
    }
    ret = rmw_fastrtps_shared_cpp::destroy_publisher(
      eprosima_fastrtps_identifier,
      participant_info,
      common_context->pub);
    if (RMW_RET_OK != ret) {
      return ret;
    }
    ret = rmw_fastrtps_shared_cpp::destroy_participant(participant_info);
    if (RMW_RET_OK != ret) {
      return ret;
    }
    delete common_context;
    context->impl->common = nullptr;
    context->impl->participant_info = nullptr;
  }
  return RMW_RET_OK;
}
