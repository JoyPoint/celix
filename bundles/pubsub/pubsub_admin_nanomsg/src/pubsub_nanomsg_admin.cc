/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */

#include <string>
#include <vector>
#include <functional>
#include <memory.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <pubsub_endpoint.h>

#include "pubsub_utils.h"
#include "pubsub_nanomsg_admin.h"
#include "pubsub_psa_nanomsg_constants.h"
/*
//#define L_DEBUG(...) \
//    logHelper_log(psa->log, OSGI_LOGSERVICE_DEBUG, __VA_ARGS__)
//#define L_INFO(...) \
//    logHelper_log(psa->log, OSGI_LOGSERVICE_INFO, __VA_ARGS__)
//#define L_WARN(...) \
//    logHelper_log(psa->log, OSGI_LOGSERVICE_WARNING, __VA_ARGS__)
//#define L_ERROR(...) \
//    logHelper_log(psa->log, OSGI_LOGSERVICE_ERROR, __VA_ARGS__)
*/
#define L_DEBUG printf
#define L_INFO printf
#define L_WARN printf
#define L_ERROR printf



static celix_status_t nanoMsg_getIpAddress(const char *interface, char **ip);

pubsub_nanomsg_admin::pubsub_nanomsg_admin(celix_bundle_context_t *_ctx, log_helper_t *logHelper):
    ctx{_ctx},
    log{logHelper} {
    verbose = celix_bundleContext_getPropertyAsBool(ctx, PUBSUB_NANOMSG_VERBOSE_KEY, PUBSUB_NANOMSG_VERBOSE_DEFAULT);
    fwUUID = celix_bundleContext_getProperty(ctx, OSGI_FRAMEWORK_FRAMEWORK_UUID, nullptr);

    char *ip = nullptr;
    const char *confIp = celix_bundleContext_getProperty(ctx, PUBSUB_NANOMSG_PSA_IP_KEY , nullptr);
    if (confIp != nullptr) {
        ip = strndup(confIp, 1024);
    }

    if (ip == nullptr) {
        //TODO try to get ip from subnet (CIDR)
    }

    if (ip == nullptr) {
        //try to get ip from itf
        const char *interface = celix_bundleContext_getProperty(ctx, PUBSUB_NANOMSG_PSA_ITF_KEY, nullptr);
        nanoMsg_getIpAddress(interface, &ip);
    }

    if (ip == nullptr) {
        L_WARN("[PSA_NANOMSG] Could not determine IP address for PSA, using default ip (%s)", PUBSUB_NANOMSG_DEFAULT_IP);
        ip = strndup(PUBSUB_NANOMSG_DEFAULT_IP, 1024);
    }

    ipAddress = ip;
    if (verbose) {
        L_INFO("[PSA_NANOMSG] Using %s for service annunciation", ip);
    }


    long _basePort = celix_bundleContext_getPropertyAsLong(ctx, PSA_NANOMSG_BASE_PORT, PSA_NANOMSG_DEFAULT_BASE_PORT);
    long _maxPort = celix_bundleContext_getPropertyAsLong(ctx, PSA_NANOMSG_MAX_PORT, PSA_NANOMSG_DEFAULT_MAX_PORT);
    basePort = (unsigned int)_basePort;
    maxPort = (unsigned int)_maxPort;
    if (verbose) {
        L_INFO("[PSA_NANOMSG] Using base till max port: %li till %li", _basePort, _maxPort);
    }


    defaultScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_NANOMSG_DEFAULT_SCORE_KEY, PSA_NANOMSG_DEFAULT_SCORE);
    qosSampleScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_NANOMSG_QOS_SAMPLE_SCORE_KEY, PSA_NANOMSG_DEFAULT_QOS_SAMPLE_SCORE);
    qosControlScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_NANOMSG_QOS_CONTROL_SCORE_KEY, PSA_NANOMSG_DEFAULT_QOS_CONTROL_SCORE);
}

pubsub_nanomsg_admin::~pubsub_nanomsg_admin() {
    //note assuming al psa register services and service tracker are removed.
    {
        std::lock_guard<std::mutex> lock(topicSenders.mutex);
        for (auto kv : topicSenders.map) {
            auto *sender = kv.second;
            pubsub_nanoMsgTopicSender_destroy(sender);
        }
    }

    {
        std::lock_guard<std::mutex> lock(topicReceivers.mutex);
        for (auto kv: topicReceivers.map) {
            delete kv.second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(discoveredEndpoints.mutex);
        for (auto entry : discoveredEndpoints.map) {
            auto *ep = entry.second;
            celix_properties_destroy(ep);
        }
    }

    {
        std::lock_guard<std::mutex> lock(serializers.mutex);
        // todo: do not use pointer but type in map
        for(auto kv: serializers.map) {
            free(kv.second);
        }
    }

    free(ipAddress);

}

void pubsub_nanomsg_admin::start() {
    adminService.handle = this;
    adminService.matchPublisher = [](void *handle, long svcRequesterBndId, const celix_filter_t *svcFilter, double *score, long *serializerSvcId) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->matchPublisher(svcRequesterBndId, svcFilter,score, serializerSvcId);
    };
    adminService.matchSubscriber = [](void *handle, long svcProviderBndId, const celix_properties_t *svcProperties, double *score, long *serializerSvcId) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->matchSubscriber(svcProviderBndId, svcProperties, score, serializerSvcId);
    };
    adminService.matchEndpoint = [](void *handle, const celix_properties_t *endpoint, bool *match) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->matchEndpoint(endpoint, match);
    };
    adminService.setupTopicSender = [](void *handle, const char *scope, const char *topic, long serializerSvcId, celix_properties_t **publisherEndpoint) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->setupTopicSender(scope, topic, serializerSvcId, publisherEndpoint);
    };
    adminService.teardownTopicSender = [](void *handle, const char *scope, const char *topic) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->teardownTopicSender(scope, topic);
    };
    adminService.setupTopicReceiver = [](void *handle, const char *scope, const char *topic, long serializerSvcId, celix_properties_t **subscriberEndpoint) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->setupTopicReceiver(std::string(scope), std::string(topic),serializerSvcId, subscriberEndpoint);
    };

    adminService.teardownTopicReceiver = [] (void *handle, const char *scope, const char *topic) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->teardownTopicReceiver(scope, topic);
    };
    adminService.addEndpoint = [](void *handle, const celix_properties_t *endpoint) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->addEndpoint(endpoint);
    };
    adminService.removeEndpoint = [](void *handle, const celix_properties_t *endpoint) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->removeEndpoint(endpoint);
    };

    celix_properties_t *props = celix_properties_create();
    celix_properties_set(props, PUBSUB_ADMIN_SERVICE_TYPE, PUBSUB_NANOMSG_ADMIN_TYPE);

    adminSvcId = celix_bundleContext_registerService(ctx, static_cast<void*>(&adminService), PUBSUB_ADMIN_SERVICE_NAME, props);


    celix_service_tracking_options_t opts{};
    opts.filter.serviceName = PUBSUB_SERIALIZER_SERVICE_NAME;
    opts.filter.ignoreServiceLanguage = true;
    opts.callbackHandle = this;
    opts.addWithProperties = [](void *handle, void *svc, const celix_properties_t *props) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        me->addSerializerSvc(svc, props);
    };
    opts.removeWithProperties = [](void *handle, void *svc, const celix_properties_t *props) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        me->removeSerializerSvc(svc, props);
    };
    serializersTrackerId = celix_bundleContext_trackServicesWithOptions(ctx, &opts);

    //register shell command service
    cmdSvc.handle = this;
    cmdSvc.executeCommand = [](void *handle, char * commandLine, FILE *outStream, FILE *errorStream) {
        auto me = static_cast<pubsub_nanomsg_admin*>(handle);
        return me->executeCommand(commandLine, outStream, errorStream);
    };

    celix_properties_t* shellProps = celix_properties_create();
    celix_properties_set(shellProps, OSGI_SHELL_COMMAND_NAME, "psa_nanomsg");
    celix_properties_set(shellProps, OSGI_SHELL_COMMAND_USAGE, "psa_nanomsg");
    celix_properties_set(shellProps, OSGI_SHELL_COMMAND_DESCRIPTION, "Print the information about the TopicSender and TopicReceivers for the nanomsg PSA");
    cmdSvcId = celix_bundleContext_registerService(ctx, &cmdSvc, OSGI_SHELL_COMMAND_SERVICE_NAME, shellProps);

}

void pubsub_nanomsg_admin::stop() {
    celix_bundleContext_unregisterService(ctx, adminSvcId);
    celix_bundleContext_unregisterService(ctx, cmdSvcId);
    celix_bundleContext_stopTracker(ctx, serializersTrackerId);
}

void pubsub_nanomsg_admin::addSerializerSvc(void *svc, const celix_properties_t *props) {
    const char *serType = celix_properties_get(props, PUBSUB_SERIALIZER_TYPE_KEY, nullptr);
    long svcId = celix_properties_getAsLong(props, OSGI_FRAMEWORK_SERVICE_ID, -1L);

    if (serType == nullptr) {
        L_INFO("[PSA_NANOMSG] Ignoring serializer service without %s property", PUBSUB_SERIALIZER_TYPE_KEY);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(serializers.mutex);
        auto it = serializers.map.find(svcId);
        if (it == serializers.map.end()) {
            auto entry = static_cast<psa_nanomsg_serializer_entry_t*>(calloc(1, sizeof(psa_nanomsg_serializer_entry_t)));
            entry->serType = serType;
            entry->svcId = svcId;
            entry->svc = static_cast<pubsub_serializer_service_t*>(svc);
            serializers.map[svcId] = entry;
        }
    }
}


void pubsub_nanomsg_admin::removeSerializerSvc(void */*svc*/, const celix_properties_t *props) {
    long svcId = celix_properties_getAsLong(props, OSGI_FRAMEWORK_SERVICE_ID, -1L);

    //remove serializer
    // 1) First find entry and
    // 2) loop and destroy all topic sender using the serializer and
    // 3) loop and destroy all topic receivers using the serializer
    // Note that it is the responsibility of the topology manager to create new topic senders/receivers

    std::lock_guard<std::mutex> lock(serializers.mutex);

    psa_nanomsg_serializer_entry_t* entry = nullptr;
    auto kvsm = serializers.map.find(svcId);
    if (kvsm != serializers.map.end()) {
        entry = kvsm->second;
    }
    serializers.map.erase(svcId);
    if (entry != nullptr) {
        {
            std::lock_guard<std::mutex> senderLock(topicSenders.mutex);
                for (auto kv: topicSenders.map) {
                auto *sender = kv.second;
                if (sender != nullptr && entry->svcId == pubsub_nanoMsgTopicSender_serializerSvcId(sender)) {
                    char *key = kv.first;
                    topicSenders.map.erase(kv.first);
                    pubsub_nanoMsgTopicSender_destroy(sender);
                    free(key);
                }
            }
        }

        {
            std::lock_guard<std::mutex> receiverLock(topicReceivers.mutex);
            for (auto kv : topicReceivers.map){
                auto *receiver = kv.second;
                if (receiver != nullptr && entry->svcId == receiver->serializerSvcId()) {
                    auto key = kv.first;
                    topicReceivers.map.erase(key);
                    delete receiver;
                }
            }
        }

        free(entry);
    }
}

celix_status_t pubsub_nanomsg_admin::matchPublisher(long svcRequesterBndId, const celix_filter_t *svcFilter,
                                                  double *outScore, long *outSerializerSvcId) {
    L_DEBUG("[PSA_NANOMSG] pubsub_nanoMsgAdmin_matchPublisher");
    celix_status_t  status = CELIX_SUCCESS;
    double score = pubsub_utils_matchPublisher(ctx, svcRequesterBndId, svcFilter->filterStr, PUBSUB_NANOMSG_ADMIN_TYPE,
            qosSampleScore, qosControlScore, defaultScore, outSerializerSvcId);
    *outScore = score;

    return status;
}

celix_status_t pubsub_nanomsg_admin::matchSubscriber(long svcProviderBndId,
                                                   const celix_properties_t *svcProperties, double *outScore,
                                                   long *outSerializerSvcId) {
    L_DEBUG("[PSA_NANOMSG] pubsub_nanoMsgAdmin_matchSubscriber");
    celix_status_t  status = CELIX_SUCCESS;
    double score = pubsub_utils_matchSubscriber(ctx, svcProviderBndId, svcProperties, PUBSUB_NANOMSG_ADMIN_TYPE,
            qosSampleScore, qosControlScore, defaultScore, outSerializerSvcId);
    if (outScore != nullptr) {
        *outScore = score;
    }
    return status;
}

celix_status_t pubsub_nanomsg_admin::matchEndpoint(const celix_properties_t *endpoint, bool *outMatch) {
    L_DEBUG("[PSA_NANOMSG] pubsub_nanoMsgAdmin_matchEndpoint");
    celix_status_t  status = CELIX_SUCCESS;
    bool match = pubsub_utils_matchEndpoint(ctx, endpoint, PUBSUB_NANOMSG_ADMIN_TYPE, nullptr);
    if (outMatch != nullptr) {
        *outMatch = match;
    }
    return status;
}

celix_status_t pubsub_nanomsg_admin::setupTopicSender(const char *scope, const char *topic,
                                                    long serializerSvcId, celix_properties_t **outPublisherEndpoint) {
    celix_status_t status = CELIX_SUCCESS;

    //1) Create TopicSender
    //2) Store TopicSender
    //3) Connect existing endpoints
    //4) set outPublisherEndpoint

    celix_properties_t *newEndpoint = nullptr;

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    pubsub_nanomsg_topic_sender_t *sender = nullptr;
    std::lock_guard<std::mutex> serializerLock(serializers.mutex);
    std::lock_guard<std::mutex> topicSenderLock(topicSenders.mutex);
    sender = topicSenders.map.find(key)->second;
    if (sender == nullptr) {
        psa_nanomsg_serializer_entry_t *serEntry = nullptr;
        auto kv = serializers.map.find(serializerSvcId);
        if (kv != serializers.map.end()) {
            serEntry = kv->second;
        }
        if (serEntry != nullptr) {
            sender = pubsub_nanoMsgTopicSender_create(ctx, log, scope, topic, serializerSvcId, serEntry->svc, ipAddress,
                                                      basePort, maxPort);
        }
        if (sender != nullptr) {
            const char *psaType = PUBSUB_NANOMSG_ADMIN_TYPE;
            const char *serType = serEntry->serType;
            newEndpoint = pubsubEndpoint_create(fwUUID, scope, topic, PUBSUB_PUBLISHER_ENDPOINT_TYPE, psaType, serType,
                                                nullptr);
            celix_properties_set(newEndpoint, PUBSUB_NANOMSG_URL_KEY, pubsub_nanoMsgTopicSender_url(sender));
            //if available also set container name
            const char *cn = celix_bundleContext_getProperty(ctx, "CELIX_CONTAINER_NAME", nullptr);
            if (cn != nullptr) {
                celix_properties_set(newEndpoint, "container_name", cn);
            }
            topicSenders.map[key] = sender;
        } else {
            L_ERROR("[PSA NANOMSG] Error creating a TopicSender");
            free(key);
        }
    } else {
        free(key);
        L_ERROR("[PSA_NANOMSG] Cannot setup already existing TopicSender for scope/topic %s/%s!", scope, topic);
    }
    if (sender != nullptr && newEndpoint != nullptr) {
        //TODO connect endpoints to sender, NOTE is this needed for a nanomsg topic sender?
    }

    if (newEndpoint != nullptr && outPublisherEndpoint != nullptr) {
        *outPublisherEndpoint = newEndpoint;
    }

    return status;
}

celix_status_t pubsub_nanomsg_admin::teardownTopicSender(const char *scope, const char *topic) {
    celix_status_t  status = CELIX_SUCCESS;

    //1) Find and remove TopicSender from map
    //2) destroy topic sender

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    std::lock_guard<std::mutex> topicSenderLock(topicSenders.mutex);
    auto kv = topicSenders.map.find(key);
    if (kv != topicSenders.map.end()) {
        char *mapKey = kv->first;
        pubsub_nanomsg_topic_sender_t *sender = kv->second;
        free(mapKey);
        //TODO disconnect endpoints to sender. note is this needed for a nanomsg topic sender?
        pubsub_nanoMsgTopicSender_destroy(sender);
    } else {
        L_ERROR("[PSA NANOMSG] Cannot teardown TopicSender with scope/topic %s/%s. Does not exists", scope, topic);
    }
    free(key);

    return status;
}

celix_status_t pubsub_nanomsg_admin::setupTopicReceiver(const std::string &scope, const std::string &topic,
                                                      long serializerSvcId, celix_properties_t **outSubscriberEndpoint) {

    celix_properties_t *newEndpoint = nullptr;

    std::string key = pubsubEndpoint_createScopeTopicKey(scope.c_str(), topic.c_str());
    pubsub::nanomsg::topic_receiver * receiver = nullptr;
    {
        std::lock_guard<std::mutex> serializerLock(serializers.mutex);
        std::lock_guard<std::mutex> topicReceiverLock(topicReceivers.mutex);
         auto trkv = topicReceivers.map.find(key);
         if (trkv != topicReceivers.map.end()) {
             receiver = trkv->second;
         }
        if (receiver == nullptr) {
            auto kvs = serializers.map.find(serializerSvcId);
            if (kvs != serializers.map.end()) {
                auto serEntry = kvs->second;
                receiver = new pubsub::nanomsg::topic_receiver(ctx, log, scope, topic, serializerSvcId, serEntry->svc);
            } else {
                L_ERROR("[PSA_NANOMSG] Cannot find serializer for TopicSender %s/%s", scope.c_str(), topic.c_str());
            }
            if (receiver != nullptr) {
                const char *psaType = PUBSUB_NANOMSG_ADMIN_TYPE;
                const char *serType = kvs->second->serType;
                newEndpoint = pubsubEndpoint_create(fwUUID, scope.c_str(), topic.c_str(), PUBSUB_SUBSCRIBER_ENDPOINT_TYPE, psaType,
                                                    serType, nullptr);
                //if available also set container name
                const char *cn = celix_bundleContext_getProperty(ctx, "CELIX_CONTAINER_NAME", nullptr);
                if (cn != nullptr) {
                    celix_properties_set(newEndpoint, "container_name", cn);
                }
                topicReceivers.map[key] = receiver;
            } else {
                L_ERROR("[PSA NANOMSG] Error creating a TopicReceiver.");
            }
        } else {
            L_ERROR("[PSA_NANOMSG] Cannot setup already existing TopicReceiver for scope/topic %s/%s!", scope.c_str(), topic.c_str());
        }
    }
    if (receiver != nullptr && newEndpoint != nullptr) {
        std::lock_guard<std::mutex> discEpLock(discoveredEndpoints.mutex);
        for (auto entry : discoveredEndpoints.map) {
            auto *endpoint = entry.second;
            const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, nullptr);
            if (type != nullptr && strncmp(PUBSUB_PUBLISHER_ENDPOINT_TYPE, type, strlen(PUBSUB_PUBLISHER_ENDPOINT_TYPE)) == 0) {
                connectEndpointToReceiver(receiver, endpoint);
            }
        }
    }

    if (newEndpoint != nullptr && outSubscriberEndpoint != nullptr) {
        *outSubscriberEndpoint = newEndpoint;
    }

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}

celix_status_t pubsub_nanomsg_admin::teardownTopicReceiver(const char *scope, const char *topic) {
    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    std::lock_guard<std::mutex> topicReceiverLock(topicReceivers.mutex);
    auto entry = topicReceivers.map.find(key);
    free(key);
    if (entry != topicReceivers.map.end()) {
        auto receiverKey = entry->first;
        pubsub::nanomsg::topic_receiver *receiver = entry->second;
        topicReceivers.map.erase(receiverKey);

        delete receiver;
    }

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}

celix_status_t pubsub_nanomsg_admin::connectEndpointToReceiver(pubsub::nanomsg::topic_receiver *receiver,
                                                                    const celix_properties_t *endpoint) {
    //note can be called with discoveredEndpoint.mutex lock
    celix_status_t status = CELIX_SUCCESS;

    auto scope = receiver->scope();
    auto topic = receiver->topic();

    std::string eScope = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_SCOPE, "");
    std::string eTopic = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_NAME, "");
    const char *url = celix_properties_get(endpoint, PUBSUB_NANOMSG_URL_KEY, nullptr);

    if (url == nullptr) {
//        L_WARN("[PSA NANOMSG] Error got endpoint without a nanomsg url (admin: %s, type: %s)", admin , type);
        status = CELIX_BUNDLE_EXCEPTION;
    } else {
        if ((eScope == scope) && (eTopic == topic)) {
            receiver->connectTo(url);
        }
    }

    return status;
}

celix_status_t pubsub_nanomsg_admin::addEndpoint(const celix_properties_t *endpoint) {
    const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, nullptr);

    if (type != nullptr && strncmp(PUBSUB_PUBLISHER_ENDPOINT_TYPE, type, strlen(PUBSUB_PUBLISHER_ENDPOINT_TYPE)) == 0) {
        std::lock_guard<std::mutex> threadLock(topicReceivers.mutex);
        for (auto entry: topicReceivers.map) {
            pubsub::nanomsg::topic_receiver *receiver = entry.second;
            connectEndpointToReceiver(receiver, endpoint);
        }
    }

    std::lock_guard<std::mutex> discEpLock(discoveredEndpoints.mutex);
    celix_properties_t *cpy = celix_properties_copy(endpoint);
    //TODO : check if properties are never deleted before map.
    const char *uuid = celix_properties_get(cpy, PUBSUB_ENDPOINT_UUID, nullptr);
    discoveredEndpoints.map[uuid] = cpy;

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}


celix_status_t pubsub_nanomsg_admin::disconnectEndpointFromReceiver(pubsub::nanomsg::topic_receiver *receiver,
                                                                            const celix_properties_t *endpoint) {
    //note can be called with discoveredEndpoint.mutex lock
    celix_status_t status = CELIX_SUCCESS;

    auto scope = receiver->scope();
    auto topic = receiver->topic();

    auto eScope = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_SCOPE, "");
    auto eTopic = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_NAME, "");
    const char *url = celix_properties_get(endpoint, PUBSUB_NANOMSG_URL_KEY, nullptr);

    if (url == nullptr) {
        L_WARN("[PSA NANOMSG] Error got endpoint without nanomsg url");
        status = CELIX_BUNDLE_EXCEPTION;
    } else {
        if ((eScope == scope) && (eTopic == topic)) {
            receiver->disconnectFrom(url);
        }
    }

    return status;
}

celix_status_t pubsub_nanomsg_admin::removeEndpoint(const celix_properties_t *endpoint) {
    const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, nullptr);

    if (type != nullptr && strncmp(PUBSUB_PUBLISHER_ENDPOINT_TYPE, type, strlen(PUBSUB_PUBLISHER_ENDPOINT_TYPE)) == 0) {
        std::lock_guard<std::mutex> topicReceiverLock(topicReceivers.mutex);
        for (auto entry : topicReceivers.map) {
            pubsub::nanomsg::topic_receiver *receiver = entry.second;
            disconnectEndpointFromReceiver(receiver, endpoint);
        }
    }
    {
        std::lock_guard<std::mutex> discEpLock(discoveredEndpoints.mutex);
        const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, nullptr);
        discoveredEndpoints.map.erase(uuid);
    }
    return CELIX_SUCCESS;;
}

celix_status_t pubsub_nanomsg_admin::executeCommand(char *commandLine __attribute__((unused)), FILE *out,
                                                  FILE *errStream __attribute__((unused))) {
    celix_status_t  status = CELIX_SUCCESS;

    fprintf(out, "\n");
    fprintf(out, "Topic Senders:\n");
    {
        std::lock_guard<std::mutex> serializerLock(serializers.mutex);
        std::lock_guard<std::mutex> topicSenderLock(topicSenders.mutex);
        for (auto kvts: topicSenders.map) {
            pubsub_nanomsg_topic_sender_t *sender = kvts.second;
            long serSvcId = pubsub_nanoMsgTopicSender_serializerSvcId(sender);
            auto kvs = serializers.map.find(serSvcId);
            const char *serType = kvs->second == nullptr ? "!Error!" : kvs->second->serType;
            const char *scope = pubsub_nanoMsgTopicSender_scope(sender);
            const char *topic = pubsub_nanoMsgTopicSender_topic(sender);
            const char *url = pubsub_nanoMsgTopicSender_url(sender);
            fprintf(out, "|- Topic Sender %s/%s\n", scope, topic);
            fprintf(out, "   |- serializer type = %s\n", serType);
            fprintf(out, "   |- url             = %s\n", url);
        }
    }

    {
        fprintf(out, "\n");
        fprintf(out, "\nTopic Receivers:\n");
        std::lock_guard<std::mutex> serializerLock(serializers.mutex);
        std::lock_guard<std::mutex> topicReceiverLock(topicReceivers.mutex);
        for (auto entry : topicReceivers.map) {
            pubsub::nanomsg::topic_receiver *receiver = entry.second;
            long serSvcId = receiver->serializerSvcId();
            auto kv =  serializers.map.find(serSvcId);
            const char *serType = kv->second == nullptr ? "!Error!" : kv->second->serType;
            auto scope = receiver->scope();
            auto topic = receiver->topic();

            std::vector<std::string> connected{};
            std::vector<std::string> unconnected{};
            receiver->listConnections(connected, unconnected);

            fprintf(out, "|- Topic Receiver %s/%s\n", scope.c_str(), topic.c_str());
            fprintf(out, "   |- serializer type = %s\n", serType);
            for (auto url : connected) {
                fprintf(out, "   |- connected url   = %s\n", url.c_str());
            }
            for (auto url : unconnected) {
                fprintf(out, "   |- unconnected url = %s\n", url.c_str());
            }
        }
    }
    fprintf(out, "\n");

    return status;
}

#ifndef ANDROID
static celix_status_t nanoMsg_getIpAddress(const char *interface, char **ip) {
    celix_status_t status = CELIX_BUNDLE_EXCEPTION;

    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) != -1)
    {
        for (ifa = ifaddr; ifa != nullptr && status != CELIX_SUCCESS; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr)
                continue;

            if ((getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in), host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) && (ifa->ifa_addr->sa_family == AF_INET)) {
                if (interface == nullptr) {
                    *ip = strdup(host);
                    status = CELIX_SUCCESS;
                }
                else if (strcmp(ifa->ifa_name, interface) == 0) {
                    *ip = strdup(host);
                    status = CELIX_SUCCESS;
                }
            }
        }

        freeifaddrs(ifaddr);
    }

    return status;
}
#endif