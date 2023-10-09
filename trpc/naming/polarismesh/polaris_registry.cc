//
//
// Tencent is pleased to support the open source community by making tRPC available.
//
// Copyright (C) 2023 THL A29 Limited, a Tencent company.
// All rights reserved.
//
// If you have downloaded a copy of the tRPC source code from Tencent,
// please note that tRPC source code is licensed under the  Apache 2.0 License,
// A copy of the Apache 2.0 License is included in this file.
//
//

#include "trpc/naming/polarismesh/polaris_registry.h"

#include <utility>

#include "yaml-cpp/yaml.h"

#include "trpc/common/config/trpc_config.h"
#include "trpc/naming/polarismesh/common.h"
#include "trpc/naming/polarismesh/trpc_share_context.h"
#include "trpc/naming/registry_factory.h"
#include "trpc/util/log/logging.h"

namespace trpc {

int PolarisRegistry::Init() noexcept {
  if (init_) {
    TRPC_FMT_DEBUG("Already init");
    return 0;
  }

  if (plugin_config_.name.empty()) {
    trpc::naming::PolarisNamingConfig config;
    if (!TrpcConfig::GetInstance()->GetPluginConfig<trpc::naming::RegistryConfig>("registry", "polarismesh",
                                                                                  config.registry_config)) {
      TRPC_FMT_ERROR("get registry polaris config error, use default");
    }
    SetPolarisSelectorConf(config);
    plugin_config_ = config;
  }

  heartbeat_interval_ = plugin_config_.registry_config.heartbeat_interval;
  // The old version of HeartBeat_timeout_ (1s) has not actually taken effect.
  // Now heartBeat_timeOut_ is effective, and 1S is added here to be compatible (equivalent to waiting for the extra
  // time to be established)
  heartbeat_timeout_ = plugin_config_.registry_config.heartbeat_timeout + 1000;
  for (const auto& service_config : plugin_config_.registry_config.services_config) {
    polaris::ServiceKey service_key{service_config.namespace_, service_config.name};
    services_config_.insert(std::make_pair(service_key, service_config));
  }

  if (trpc::TrpcShareContext::GetInstance()->Init(plugin_config_) != 0) {
    return -1;
  }

  auto context = trpc::TrpcShareContext::GetInstance()->GetPolarisContext();
  provider_api_ = std::unique_ptr<polaris::ProviderApi>(polaris::ProviderApi::Create(context.get()));

  if (!provider_api_) {
    TRPC_FMT_ERROR("Create ProviderApi failed");
    return -1;
  }

  init_ = true;
  return 0;
}

void PolarisRegistry::Destroy() noexcept {
  if (!init_) {
    TRPC_FMT_DEBUG("No init yet");
    return;
  }

  services_config_.clear();
  provider_api_ = nullptr;
  trpc::TrpcShareContext::GetInstance()->Destroy();
  init_ = false;
}

PolarisRegistryInfo PolarisRegistry::SetupPolarisRegistryInfo(const RegistryInfo& info) {
  PolarisRegistryInfo polaris_registry_info;
  ConvertToPolarisRegistryInfo(info, polaris_registry_info);
  polaris_registry_info.service_name = info.name;
  polaris_registry_info.service_namespace = trpc::TrpcConfig::GetInstance()->GetGlobalConfig().env_namespace;
  if (polaris_registry_info.service_namespace.empty()) {
    polaris_registry_info.service_namespace = GetStringFromMetadata(info.meta, "namespace", "");
  }
  polaris_registry_info.timeout = heartbeat_timeout_;

  // Make up the default information
  if (polaris_registry_info.service_token.empty()) {
    if (GetTokenFromRegistryConfig(polaris_registry_info) != 0) {
      TRPC_FMT_ERROR("token is empty");
      return polaris_registry_info;
    }
  }
  // Get META information from the configuration
  GetMetadataFromRegistryConfig(polaris_registry_info);

  return polaris_registry_info;
}

int PolarisRegistry::Register(const RegistryInfo* info) {
  if (!init_) {
    TRPC_FMT_ERROR("No init yet");
    return -1;
  }

  if (info == nullptr) {
    TRPC_FMT_ERROR("Input parameter is empty");
    return -1;
  }

  PolarisRegistryInfo polaris_registry_info = SetupPolarisRegistryInfo(*info);

  // When the business does not indicate the health check, whether the health check will be turned on based on the
  // global configuration as the subject
  if (info->meta.find("enable_health_check") == info->meta.end()) {
    polaris_registry_info.enable_health_check =
        TrpcConfig::GetInstance()->GetGlobalConfig().heartbeat_config.enable_heartbeat == true ? 1 : 0;
  }

  polaris::InstanceRegisterRequest register_req(polaris_registry_info.service_namespace,
                                                polaris_registry_info.service_name, polaris_registry_info.service_token,
                                                polaris_registry_info.host, polaris_registry_info.port);
  register_req.SetTimeout(polaris_registry_info.timeout);
  register_req.SetProtocol(polaris_registry_info.protocol);
  register_req.SetWeight(polaris_registry_info.weight);
  register_req.SetPriority(polaris_registry_info.priority);
  register_req.SetVersion(polaris_registry_info.version);
  register_req.SetMetadata(polaris_registry_info.metadata);
  register_req.SetHealthCheckFlag(polaris_registry_info.enable_health_check);
  register_req.SetHealthCheckType(static_cast<polaris::HealthCheckType>(polaris_registry_info.health_check_type));
  register_req.SetTtl(polaris_registry_info.ttl);

  polaris::ReturnCode ret = provider_api_->Register(register_req, polaris_registry_info.instance_id);
  if (ret == polaris::ReturnCode::kReturnOk || ret == polaris::ReturnCode::kReturnExistedResource) {
    const_cast<RegistryInfo*>(info)->meta["instance_id"] = polaris_registry_info.instance_id;
    return 0;
  }

  TRPC_FMT_ERROR("Register failed, sdk returnCode:{}, service_name:{}, service_namespace:{}", static_cast<int32_t>(ret),
                 polaris_registry_info.service_name, polaris_registry_info.service_namespace);
  return -1;
}

int PolarisRegistry::Unregister(const RegistryInfo* info) {
  if (!init_) {
    TRPC_FMT_ERROR("No init yet");
    return -1;
  }

  if (info == nullptr) {
    TRPC_FMT_ERROR("Input parameter is empty");
    return -1;
  }

  PolarisRegistryInfo polaris_registry_info = SetupPolarisRegistryInfo(*info);

  std::shared_ptr<polaris::InstanceDeregisterRequest> deregister_ptr = nullptr;
  if (polaris_registry_info.instance_id.empty()) {
    deregister_ptr = std::make_shared<polaris::InstanceDeregisterRequest>(
        polaris_registry_info.service_namespace, polaris_registry_info.service_name,
        polaris_registry_info.service_token, polaris_registry_info.host, polaris_registry_info.port);
  } else {
    deregister_ptr = std::make_shared<polaris::InstanceDeregisterRequest>(polaris_registry_info.service_token,
                                                                          polaris_registry_info.instance_id);
  }
  deregister_ptr->SetTimeout(polaris_registry_info.timeout);

  polaris::ReturnCode ret = provider_api_->Deregister(*deregister_ptr);

  if (ret == polaris::ReturnCode::kReturnOk) {
    return 0;
  }

  TRPC_FMT_ERROR("Deregister failed, sdk returnCode:{}, service_name:{}, service_namespace:{}",
                 static_cast<int32_t>(ret), polaris_registry_info.service_name,
                 polaris_registry_info.service_namespace);
  return -1;
}

int PolarisRegistry::HeartBeat(const RegistryInfo* info) {
  TRPC_FMT_DEBUG("HeartBeat Start...");
  if (!init_) {
    TRPC_FMT_ERROR("No init yet");
    return -1;
  }

  if (info == nullptr) {
    TRPC_FMT_ERROR("Input parameter is empty");
    return -1;
  }

  std::string service_namespace = trpc::TrpcConfig::GetInstance()->GetGlobalConfig().env_namespace;
  if (service_namespace.empty()) {
    service_namespace = GetStringFromMetadata(info->meta, "namespace", "");
  }
  std::string service_name = info->name;
  polaris::ServiceKey service_key{service_namespace, service_name};
  auto it = services_config_.find(service_key);
  if (it == services_config_.end()) {
    TRPC_FMT_ERROR("find service in registry config faild, service_name:{}, service_namespace:{}", service_key.name_,
                   service_key.namespace_);
    return -1;
  }

  polaris::ReturnCode ret = polaris::ReturnCode::kReturnOk;
  if (it->second.instance_id.empty()) {
    polaris::InstanceHeartbeatRequest heartbeat_req(service_namespace, service_name, it->second.token, info->host,
                                                    info->port);
    heartbeat_req.SetTimeout(heartbeat_timeout_);
    ret = provider_api_->Heartbeat(heartbeat_req);
  } else {
    polaris::InstanceHeartbeatRequest heartbeat_req(it->second.token, it->second.instance_id);
    heartbeat_req.SetTimeout(heartbeat_timeout_);
    ret = provider_api_->Heartbeat(heartbeat_req);
  }

  if (ret == polaris::ReturnCode::kReturnOk) {
    return 0;
  }

  TRPC_FMT_ERROR("Heartbeat failed, sdk returnCode:{}, service_name:{}, service_namespace:{}",
                 static_cast<int32_t>(ret), service_key.name_, service_key.namespace_);
  return -1;
}

Future<> PolarisRegistry::AsyncHeartBeat(const RegistryInfo* info) {
  TRPC_FMT_DEBUG("AsyncHeartBeat Start...");
  if (!init_) {
    std::string error_str("No init yet");
    TRPC_LOG_ERROR(error_str);
    return MakeExceptionFuture<>(CommonException(error_str.c_str()));
  }

  if (info == nullptr) {
    std::string error_str("Input parameter is empty");
    TRPC_LOG_ERROR(error_str);
    return MakeExceptionFuture<>(CommonException(error_str.c_str()));
  }

  std::string service_namespace = trpc::TrpcConfig::GetInstance()->GetGlobalConfig().env_namespace;
  if (service_namespace.empty()) {
    service_namespace = GetStringFromMetadata(info->meta, "namespace", "");
  }
  std::string service_name = info->name;
  polaris::ServiceKey service_key{service_namespace, service_name};
  auto it = services_config_.find(service_key);
  if (it == services_config_.end()) {
    std::stringstream error;
    error << "find service in registry config faild"
          << ", service_name:" << service_key.name_ << ", service_namespace:" << service_key.namespace_;
    TRPC_LOG_ERROR(error.str());
    return MakeExceptionFuture<>(CommonException(error.str().c_str()));
  }

  polaris::ReturnCode ret = polaris::ReturnCode::kReturnOk;
  PolarisHeartbeatCallback* callback = new PolarisHeartbeatCallback(service_key);
  if (it->second.instance_id.empty()) {
    polaris::InstanceHeartbeatRequest heartbeat_req(service_namespace, service_name, it->second.token, info->host,
                                                    info->port);
    heartbeat_req.SetTimeout(heartbeat_timeout_);
    // The call is returned immediately, and the follow -up process is executed in the callback (currently only the need
    // for printing logs)
    ret = provider_api_->AsyncHeartbeat(heartbeat_req, callback);
  } else {
    polaris::InstanceHeartbeatRequest heartbeat_req(it->second.token, it->second.instance_id);
    heartbeat_req.SetTimeout(heartbeat_timeout_);
    // The call is returned immediately, and the follow -up process is executed in the callback (currently only the need
    // for printing logs)
    ret = provider_api_->AsyncHeartbeat(heartbeat_req, callback);
  }

  if (ret != polaris::ReturnCode::kReturnOk) {
    std::stringstream error;
    error << "AsyncHeartBeat failed, sdk returnCode:" << ret << ", service_name:" << service_key.name_
          << ", service_namespace:" << service_key.namespace_;
    TRPC_LOG_ERROR(error.str());
    return MakeExceptionFuture<>(CommonException(error.str().c_str()));
  }

  return trpc::MakeReadyFuture<>();
}

int PolarisRegistry::GetTokenFromRegistryConfig(PolarisRegistryInfo& polaris_registry_info) {
  // Try to get token from the registry configuration
  polaris::ServiceKey service_key{polaris_registry_info.service_namespace, polaris_registry_info.service_name};
  auto it = services_config_.find(service_key);
  if (it == services_config_.end()) {
    TRPC_FMT_ERROR("find token in registry config faild, service_name:{}, service_namespace:{}",
                   polaris_registry_info.service_name, polaris_registry_info.service_namespace);
    return -1;
  }
  polaris_registry_info.service_token = it->second.token;

  return 0;
}

void PolarisRegistry::GetMetadataFromRegistryConfig(PolarisRegistryInfo& polaris_registry_info) {
  // Try to get token from the registry configuration
  polaris::ServiceKey service_key{polaris_registry_info.service_namespace, polaris_registry_info.service_name};
  auto it = services_config_.find(service_key);
  if (it != services_config_.end()) {
    polaris_registry_info.metadata = it->second.metadata;
  }
}

}  // namespace trpc
