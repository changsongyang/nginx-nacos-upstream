//
// Created by pc on 9/23/25.
//

#ifndef NGX_NACOS_PAYLOAD_H
#define NGX_NACOS_PAYLOAD_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <yaij/api/yajl_tree.h>

#define PAYLOAD_TYPE_LIST               \
    PC(NONE__START)                     \
    PC(ClientDetectionRequest)          \
    PC(ConnectionSetupRequest)          \
    PC(ConnectResetRequest)             \
    PC(HealthCheckRequest)              \
    PC(PushAckRequest)                  \
    PC(ServerCheckRequest)              \
    PC(ServerLoaderInfoRequest)         \
    PC(ServerReloadRequest)             \
    PC(InstanceRequest)                 \
    PC(BatchInstanceRequest)            \
    PC(NotifySubscriberRequest)         \
    PC(ServiceListRequest)              \
    PC(ServiceQueryRequest)             \
    PC(SubscribeServiceRequest)         \
    PC(ConfigBatchListenRequest)        \
    PC(ConfigChangeNotifyRequest)       \
    PC(ConfigQueryRequest)              \
    PC(SetupAckRequest)                 \
    PC(ClientDetectionResponse)         \
    PC(SetupAckResponse)                \
    PC(ConnectResetResponse)            \
    PC(ErrorResponse)                   \
    PC(HealthCheckResponse)             \
    PC(ServerCheckResponse)             \
    PC(ServerLoaderInfoResponse)        \
    PC(ServerReloadResponse)            \
    PC(InstanceResponse)                \
    PC(BatchInstanceResponse)           \
    PC(NotifySubscriberResponse)        \
    PC(QueryServiceResponse)            \
    PC(ServiceListResponse)             \
    PC(SubscribeServiceResponse)        \
    PC(ConfigChangeBatchListenResponse) \
    PC(ConfigChangeNotifyResponse)      \
    PC(ConfigQueryResponse)             \
    PC(NONE__END)

enum ngx_nacos_payload_type {
#define PC(x) x,
    PAYLOAD_TYPE_LIST
#undef PC
};

ngx_int_t ngx_nacos_check_response(ngx_log_t *log, yajl_val val);

#define NACOS_INTERNAL_REQUEST "{\"module\":\"internal\"}"

#endif  // NGX_NACOS_PAYLOAD_H
