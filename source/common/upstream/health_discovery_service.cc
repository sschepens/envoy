#include "common/upstream/health_discovery_service.h"

#include "envoy/stats/scope.h"

#include "common/protobuf/protobuf.h"

namespace Envoy {
namespace Upstream {

HdsDelegate::HdsDelegate(Stats::Scope& scope, Grpc::AsyncClientPtr async_client,
                         Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                         Envoy::Stats::Store& stats, Ssl::ContextManager& ssl_context_manager,
                         Runtime::RandomGenerator& random, ClusterInfoFactory& info_factory,
                         AccessLog::AccessLogManager& access_log_manager, ClusterManager& cm,
                         const LocalInfo::LocalInfo& local_info)
    : stats_{ALL_HDS_STATS(POOL_COUNTER_PREFIX(scope, "hds_delegate."))},
      service_method_(*Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
          "envoy.service.discovery.v2.HealthDiscoveryService.StreamHealthCheck")),
      async_client_(std::move(async_client)), dispatcher_(dispatcher), runtime_(runtime),
      store_stats(stats), ssl_context_manager_(ssl_context_manager), random_(random),
      info_factory_(info_factory), access_log_manager_(access_log_manager), cm_(cm),
      local_info_(local_info) {
  health_check_request_.mutable_health_check_request()->mutable_node()->MergeFrom(
      local_info_.node());
  backoff_strategy_ = std::make_unique<JitteredBackOffStrategy>(RetryInitialDelayMilliseconds,
                                                                RetryMaxDelayMilliseconds, random_);
  hds_retry_timer_ = dispatcher.createTimer([this]() -> void { establishNewStream(); });
  hds_stream_response_timer_ = dispatcher.createTimer([this]() -> void { sendResponse(); });

  // TODO(lilika): Add support for other types of healthchecks
  health_check_request_.mutable_health_check_request()
      ->mutable_capability()
      ->add_health_check_protocols(envoy::service::discovery::v2::Capability::HTTP);
  health_check_request_.mutable_health_check_request()
      ->mutable_capability()
      ->add_health_check_protocols(envoy::service::discovery::v2::Capability::TCP);

  establishNewStream();
}

void HdsDelegate::setHdsRetryTimer() {
  const auto retry_ms = std::chrono::milliseconds(backoff_strategy_->nextBackOffMs());
  ENVOY_LOG(warn, "HdsDelegate stream/connection failure, will retry in {} ms.", retry_ms.count());

  hds_retry_timer_->enableTimer(retry_ms);
}

void HdsDelegate::setHdsStreamResponseTimer() {
  hds_stream_response_timer_->enableTimer(std::chrono::milliseconds(server_response_ms_));
}

void HdsDelegate::establishNewStream() {
  ENVOY_LOG(debug, "Establishing new gRPC bidi stream for {}", service_method_.DebugString());
  stream_ = async_client_->start(service_method_, *this);
  if (stream_ == nullptr) {
    ENVOY_LOG(warn, "Unable to establish new stream");
    handleFailure();
    return;
  }

  ENVOY_LOG(debug, "Sending HealthCheckRequest {} ", health_check_request_.DebugString());
  stream_->sendMessage(health_check_request_, false);
  stats_.responses_.inc();
  backoff_strategy_->reset();
}

void HdsDelegate::handleFailure() {
  stats_.errors_.inc();
  setHdsRetryTimer();
}

// TODO(lilika): Add support for the same endpoint in different clusters/ports
envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse
HdsDelegate::sendResponse() {
  envoy::service::discovery::v2::HealthCheckRequestOrEndpointHealthResponse response;
  for (const auto& cluster : hds_clusters_) {
    for (const auto& hosts : cluster.second->prioritySet().hostSetsPerPriority()) {
      for (const auto& host : hosts->hosts()) {
        auto* endpoint = response.mutable_endpoint_health_response()->add_endpoints_health();
        Network::Utility::addressToProtobufAddress(
            *host->address(), *endpoint->mutable_endpoint()->mutable_address());
        // TODO(lilika): Add support for more granular options of envoy::api::v2::core::HealthStatus
        if (host->health() == Host::Health::Healthy) {
          endpoint->set_health_status(envoy::api::v2::core::HealthStatus::HEALTHY);
        } else {
          if (host->getActiveHealthFailureType() == Host::ActiveHealthFailureType::TIMEOUT) {
            endpoint->set_health_status(envoy::api::v2::core::HealthStatus::TIMEOUT);
          } else if (host->getActiveHealthFailureType() ==
                     Host::ActiveHealthFailureType::UNHEALTHY) {
            endpoint->set_health_status(envoy::api::v2::core::HealthStatus::UNHEALTHY);
          } else if (host->getActiveHealthFailureType() == Host::ActiveHealthFailureType::UNKNOWN) {
            endpoint->set_health_status(envoy::api::v2::core::HealthStatus::UNHEALTHY);
          } else {
            NOT_REACHED_GCOVR_EXCL_LINE;
          }
        }
      }
    }
  }
  ENVOY_LOG(debug, "Sending EndpointHealthResponse to server {}", response.DebugString());
  stream_->sendMessage(response, false);
  stats_.responses_.inc();
  setHdsStreamResponseTimer();
  return response;
}

void HdsDelegate::onCreateInitialMetadata(Http::HeaderMap& metadata) {
  UNREFERENCED_PARAMETER(metadata);
}

void HdsDelegate::onReceiveInitialMetadata(Http::HeaderMapPtr&& metadata) {
  UNREFERENCED_PARAMETER(metadata);
}

void HdsDelegate::processMessage(
    std::unique_ptr<envoy::service::discovery::v2::HealthCheckSpecifier>&& message) {
  ENVOY_LOG(debug, "New health check response message {} ", message->DebugString());
  ASSERT(message);

  std::unordered_set<std::string> clusters_to_remove;
  for (const auto& current_cluster : hds_clusters_) {
    clusters_to_remove.insert(current_cluster.first);
  }

  for (const auto& cluster_health_check : message->cluster_health_checks()) {
    const auto cluster_name = cluster_health_check.cluster_name();
    static const envoy::api::v2::core::BindConfig bind_config;
    envoy::api::v2::Cluster cluster_config;

    // Keep this cluster
    clusters_to_remove.erase(cluster_name);

    cluster_config.set_name(cluster_name);
    cluster_config.mutable_connect_timeout()->set_seconds(ClusterTimeoutSeconds);
    cluster_config.mutable_per_connection_buffer_limit_bytes()->set_value(
        ClusterConnectionBufferLimitBytes);

    // Add endpoints to cluster
    for (const auto& locality_endpoints : cluster_health_check.locality_endpoints()) {
      for (const auto& endpoint : locality_endpoints.endpoints()) {
        cluster_config.add_hosts()->MergeFrom(endpoint.address());
      }
    }

    // TODO(lilika): Add support for optional per-endpoint health checks

    // Add healthchecks to cluster
    for (auto& health_check : cluster_health_check.health_checks()) {
      cluster_config.add_health_checks()->MergeFrom(health_check);
    }

    auto existing_cluster = hds_clusters_.find(cluster_name);
    if (existing_cluster != hds_clusters_.end()) {
      // ENVOY_LOG(debug, "Update existing HdsCluster config {}", cluster_config.DebugString());
      // For now if update returns true we recreate the HdsCluster
      // This happens when the health checks have changed
      ENVOY_LOG(debug, "Found existing HdsCluster {}", cluster_name);
      if (!existing_cluster->second->update(cluster_config)) {
        ENVOY_LOG(debug, "Not modifying cluster {}", cluster_name);
        continue;
      }
      ENVOY_LOG(debug, "Recreating cluster {}", cluster_name);
      hds_clusters_.erase(cluster_name);
    }

    ENVOY_LOG(debug, "New HdsCluster config {} ", cluster_config.DebugString());

    // Create HdsCluster
    hds_clusters_[cluster_name] = HdsClusterPtr{
        new HdsCluster(runtime_, cluster_config, bind_config, store_stats, ssl_context_manager_,
                       false, info_factory_, cm_, local_info_, dispatcher_, random_)};

    // auto new_cluster = hds_clusters_.emplace(std::make_pair(
    //     cluster_name,
    //     new HdsCluster(runtime_, cluster_config, bind_config, store_stats, ssl_context_manager_,
    //                    false, info_factory_, cm_, local_info_, dispatcher_, random_)));

    // new_cluster.first->second->startHealthchecks(access_log_manager_, runtime_, random_,
    //                                             dispatcher_);

    hds_clusters_[cluster_name]->startHealthchecks(access_log_manager_, runtime_, random_,
                                                   dispatcher_);
  }

  for (const auto& cluster_name : clusters_to_remove) {
    hds_clusters_.erase(cluster_name);
    ENVOY_LOG(debug, "hds: remove cluster '{}'", cluster_name);
  }
}

void HdsDelegate::onReceiveMessage(
    std::unique_ptr<envoy::service::discovery::v2::HealthCheckSpecifier>&& message) {
  stats_.requests_.inc();
  ENVOY_LOG(debug, "New health check response message {} ", message->DebugString());

  // Set response
  auto server_response_ms = PROTOBUF_GET_MS_REQUIRED(*message, interval);

  // Process the HealthCheckSpecifier message
  processMessage(std::move(message));

  if (server_response_ms_ != server_response_ms) {
    server_response_ms_ = server_response_ms;
    setHdsStreamResponseTimer();
  }
}

void HdsDelegate::onReceiveTrailingMetadata(Http::HeaderMapPtr&& metadata) {
  UNREFERENCED_PARAMETER(metadata);
}

void HdsDelegate::onRemoteClose(Grpc::Status::GrpcStatus status, const std::string& message) {
  ENVOY_LOG(warn, "gRPC config stream closed: {}, {}", status, message);
  hds_stream_response_timer_->disableTimer();
  stream_ = nullptr;
  server_response_ms_ = 0;
  handleFailure();
}

HdsCluster::HdsCluster(Runtime::Loader& runtime, const envoy::api::v2::Cluster& cluster,
                       const envoy::api::v2::core::BindConfig& bind_config, Stats::Store& stats,
                       Ssl::ContextManager& ssl_context_manager, bool added_via_api,
                       ClusterInfoFactory& info_factory, ClusterManager& cm,
                       const LocalInfo::LocalInfo& local_info, Event::Dispatcher& dispatcher,
                       Runtime::RandomGenerator& random)
    : runtime_(runtime), cluster_(cluster), bind_config_(bind_config), stats_(stats),
      ssl_context_manager_(ssl_context_manager), added_via_api_(added_via_api),
      initial_hosts_(new HostVector()) {
  ENVOY_LOG(debug, "Creating an HdsCluster");
  priority_set_.getOrCreateHostSet(0);

  info_ =
      info_factory.createClusterInfo(runtime_, cluster_, bind_config_, stats_, ssl_context_manager_,
                                     added_via_api_, cm, local_info, dispatcher, random);

  for (const auto& host : cluster.hosts()) {
    initial_hosts_->emplace_back(new HostImpl(
        info_, "", Network::Address::resolveProtoAddress(host),
        envoy::api::v2::core::Metadata::default_instance(), 1,
        envoy::api::v2::core::Locality().default_instance(),
        envoy::api::v2::endpoint::Endpoint::HealthCheckConfig().default_instance(), 0));
  }
  initialize([] {});
}

ClusterSharedPtr HdsCluster::create() { NOT_IMPLEMENTED_GCOVR_EXCL_LINE; }

ClusterInfoConstSharedPtr ProdClusterInfoFactory::createClusterInfo(
    Runtime::Loader& runtime, const envoy::api::v2::Cluster& cluster,
    const envoy::api::v2::core::BindConfig& bind_config, Stats::Store& stats,
    Ssl::ContextManager& ssl_context_manager, bool added_via_api, ClusterManager& cm,
    const LocalInfo::LocalInfo& local_info, Event::Dispatcher& dispatcher,
    Runtime::RandomGenerator& random) {

  Envoy::Stats::ScopePtr scope = stats.createScope(fmt::format("cluster.{}.", cluster.name()));

  Envoy::Server::Configuration::TransportSocketFactoryContextImpl factory_context(
      ssl_context_manager, *scope, cm, local_info, dispatcher, random, stats);

  // TODO(JimmyCYJ): Support SDS for HDS cluster.
  Network::TransportSocketFactoryPtr socket_factory =
      Upstream::createTransportSocketFactory(cluster, factory_context);

  return std::make_unique<ClusterInfoImpl>(cluster, bind_config, runtime, std::move(socket_factory),
                                           std::move(scope), added_via_api);
}

void HdsCluster::startHealthchecks(AccessLog::AccessLogManager& access_log_manager,
                                   Runtime::Loader& runtime, Runtime::RandomGenerator& random,
                                   Event::Dispatcher& dispatcher) {

  for (auto& health_check : cluster_.health_checks()) {
    health_checkers_.push_back(Upstream::HealthCheckerFactory::create(
        health_check, *this, runtime, random, dispatcher, access_log_manager));
    health_checkers_.back()->start();
  }
}

void HdsCluster::initialize(std::function<void()> callback) {
  initialization_complete_callback_ = callback;
  for (const auto& host : *initial_hosts_) {
    host->healthFlagSet(Host::HealthFlag::FAILED_ACTIVE_HC);
  }

  auto& first_host_set = priority_set_.getOrCreateHostSet(0);

  first_host_set.updateHosts(
      HostSetImpl::partitionHosts(initial_hosts_, HostsPerLocalityImpl::empty()), {},
      *initial_hosts_, {}, absl::nullopt);
}

// This method should update the HdsCluster in place, but for now it will
// return true when the HdsCluster endpoints have changed
bool HdsCluster::update(const envoy::api::v2::Cluster& cluster) {
  auto& first_host_set = priority_set_.getOrCreateHostSet(0);

  if (static_cast<int>(first_host_set.hosts().size()) != cluster.hosts().size()) {
    return true;
  }

  std::unordered_set<std::string> current_hosts;
  for (const auto& host : first_host_set.hosts()) {
    current_hosts.insert(host->address()->asString());
  }

  for (const auto& host : cluster.hosts()) {
    auto address = Network::Address::resolveProtoAddress(host);
    if (current_hosts.count(address->asString()) == 0) {
      return true;
    }
  }

  return false;
}

void HdsCluster::setOutlierDetector(const Outlier::DetectorSharedPtr&) {
  NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
}

} // namespace Upstream
} // namespace Envoy
