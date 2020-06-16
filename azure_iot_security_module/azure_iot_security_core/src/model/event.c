/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "asc_security_core/asc/asc_json.h"
#include "asc_security_core/asc/asc_span.h"

#include "asc_security_core/configuration.h"
#include "asc_security_core/iotsecurity_result.h"
#include "asc_security_core/logger.h"
#include "asc_security_core/message_schema_consts.h"
#include "asc_security_core/model/event.h"
#include "asc_security_core/object_pool.h"
#include "asc_security_core/utils/itime.h"
#include "asc_security_core/utils/iuuid.h"
#include "asc_security_core/utils/string_utils.h"


#define MAX_IPV6_STRING_LENGTH (sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255")
#define RECORD_TIME_PROPERTY_MAX_LENGTH 25
#define EVENT_ID_SIZE 37

#define MAX_TIME_AS_STRING_LENGTH 25

struct event {
    COLLECTION_INTERFACE(struct event);

    // buffer workspace
    uint8_t buffer[ASC_EVENT_MAX_SIZE];

    // JSON builder on top of the event buffer workspace
    asc_json_builder builder;

    EVENT_STATUS status;

    // event schema properties
    asc_span id;
    asc_span name;
    asc_span payload_schema_version;
    asc_span category;
    asc_span event_type;
    time_t local_time;
    bool is_empty;
};

OBJECT_POOL_DEFINITIONS(event_t, EVENT_OBJECT_POOL_COUNT)
LINKED_LIST_DEFINITIONS(event_t)

static IOTSECURITY_RESULT _event_set_status(event_t* event_ptr, EVENT_STATUS status);
IOTSECURITY_RESULT _event_populate_extra_details(asc_json_builder* builder, asc_pair* extra_details, uint8_t extra_details_length);

event_t* event_init(const char* payload_schema_version, const char* name, const char* category, const char* event_type, time_t local_time) {
    log_debug("Creating event, payload_schema_version=[%s], name=[%s], category=[%s], event_type=[%s], local_time=[%lu]",
        string_utils_value_or_empty(payload_schema_version),
        string_utils_value_or_empty(name),
        string_utils_value_or_empty(category),
        string_utils_value_or_empty(event_type),
        local_time
    );

    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;
    event_t* event_ptr = NULL;
    char event_id[EVENT_ID_SIZE];
    asc_json_builder* builder;

    if (string_utils_is_blank(payload_schema_version) ||
        string_utils_is_blank(name) ||
        string_utils_is_blank(category) ||
        string_utils_is_blank(event_type)
    ) {
        log_error("Failed to create a new event due to bad argument");
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        goto cleanup;
    }

    event_ptr = object_pool_get(event_t);
    if (event_ptr == NULL) {
        log_error("Failed to initialize event due to bad argument, event_ptr=[%p]", event_ptr);
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        goto cleanup;
    }
    memset(event_ptr, 0, sizeof(event_t));

    event_id[0] = '\0';
    if (iuuid_generate(event_id) != 0) {
        log_error("Failed to generate a new UUID");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    event_ptr->id = asc_span_from_str(event_id);
    event_ptr->name = asc_span_from_str((char*)name);
    event_ptr->is_empty = true;
    event_ptr->payload_schema_version = asc_span_from_str((char*)payload_schema_version);
    event_ptr->category = asc_span_from_str((char*)category);
    event_ptr->event_type = asc_span_from_str((char*)event_type);
    event_ptr->local_time = local_time;
    event_ptr->status = EVENT_STATUS_PROCESSING;

    builder = &event_ptr->builder;

    if (asc_json_builder_init(builder, ASC_SPAN_FROM_BUFFER(event_ptr->buffer)) != ASC_OK) {
        log_error("Failed to create a new event");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_token(builder, asc_json_token_object_start()) != ASC_OK) {
        log_error("Failed to initialize a new event, name=[%s]", name);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_ID_KEY), asc_json_token_string(asc_span_from_str((char*)event_id))) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_ID_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_NAME_KEY), asc_json_token_string(event_ptr->name)) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_NAME_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_PAYLOAD_SCHEMA_VERSION_KEY), asc_json_token_string(event_ptr->payload_schema_version)) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_PAYLOAD_SCHEMA_VERSION_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_CATEGORY_KEY), asc_json_token_string(event_ptr->category)) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_CATEGORY_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_TYPE_KEY), asc_json_token_string(event_ptr->event_type)) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_TYPE_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }
    
    {
        char time_str[RECORD_TIME_PROPERTY_MAX_LENGTH] = { 0 };
        // set event local_time
        struct tm localtime = { 0 };
        if (itime_localtime(&local_time, &localtime) == NULL) {
            log_error("Failed to set localtime");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
        if (itime_iso8601(&localtime, time_str) <= 0) {
            log_error("Failed to map localtime to iso8601");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_LOCAL_TIMESTAMP_KEY), asc_json_token_string(asc_span_from_str(time_str))) != ASC_OK) {
            log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_LOCAL_TIMESTAMP_KEY);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
    }


    {
        char time_str[RECORD_TIME_PROPERTY_MAX_LENGTH] = { 0 };
        // set event utc time
        struct tm utcnow = { 0 };
        if (itime_utcnow(&local_time, &utcnow) == NULL) {
            log_error("Failed to set utc time");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
        if (itime_iso8601(&utcnow, time_str) <= 0) {
            log_error("Failed to map localtime to iso8601");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_UTC_TIMESTAMP_KEY), asc_json_token_string(asc_span_from_str(time_str))) != ASC_OK) {
            log_error("Failed to set event property, name=[%s], property=[%s]", name, EVENT_LOCAL_TIMESTAMP_KEY);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)PAYLOAD_KEY), asc_json_token_array_start()) != ASC_OK) {
        log_error("Failed to set event property, name=[%s], property=[%s]", name, PAYLOAD_KEY);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to create an event, result=[%d]", result);

        _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);

        event_deinit(event_ptr);
        event_ptr = NULL;
    } else {
        log_debug("Event has been created successfully, result=[%d]", result);
    }

    return event_ptr;
}


void event_deinit(event_t* event_ptr) {
    if (event_ptr != NULL) {
        if (event_ptr->status != EVENT_STATUS_OK) {
            log_error("Deinitialize event with status=[%d]", event_ptr->status);
        }

        object_pool_free(event_t, event_ptr);
        event_ptr = NULL;
    }
}


IOTSECURITY_RESULT event_get_data(event_t* event_ptr, char* buffer, int32_t size) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("Failed to retrieve event data, result=[%d]", result);
        goto cleanup;
    }

    if (buffer == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("Buffer cannot be null, result=[%d]", result);
        goto cleanup;
    }

    // build the event if needed
    result = event_build(event_ptr);
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to build event, result=[%d]", result);
        goto cleanup;
    }

    if (event_ptr->status != EVENT_STATUS_OK) {
        result = IOTSECURITY_RESULT_EXCEPTION;
        log_error("Cannot retrieve event data, state=[%d]", event_ptr->status);
        goto cleanup;
    }

    if (asc_span_to_str(buffer, size, asc_json_builder_span_get(&event_ptr->builder)) != ASC_OK) {
        result = IOTSECURITY_RESULT_EXCEPTION;
        log_error("Failed to extract event data");
        goto cleanup;
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to retrieve event data, result=[%d]", result);

        _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);
    }

    return result;
}

EVENT_STATUS event_get_status(event_t* event_ptr) {
    EVENT_STATUS status = EVENT_STATUS_UNINITIALIZED;

    if (event_ptr != NULL) {
        status = event_ptr->status;
    }

    return status;
}


IOTSECURITY_RESULT event_build(event_t* event_ptr) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;
    asc_json_builder* builder;

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("Failed to build event, result=[%d]", result);
        goto cleanup;
    }

    switch (event_ptr->status) {
        case EVENT_STATUS_EXCEPTION:
            result = IOTSECURITY_RESULT_EXCEPTION;
            log_error("Cannot build the event, status=[%d]", event_ptr->status);
            goto cleanup;
        case EVENT_STATUS_OK:
            result = IOTSECURITY_RESULT_OK;
            goto cleanup;
        case EVENT_STATUS_PROCESSING:
        default:
            // continue processing and build the event
            break;
    }

    builder = &event_ptr->builder;

    builder->_internal.json._internal.capacity += (int32_t)EVENT_END_SIZE;

    if (asc_json_builder_append_token(builder, asc_json_token_array_end()) != ASC_OK) {
        result = IOTSECURITY_RESULT_EXCEPTION;
        log_error("Failed to close json array, property=[%s]", PAYLOAD_KEY);
        goto cleanup;
    }

    if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EVENT_IS_EMPTY_KEY), asc_json_token_boolean(event_ptr->is_empty)) != ASC_OK) {
        result = IOTSECURITY_RESULT_EXCEPTION;
        log_error("Failed to set event property=[%s]", EVENT_IS_EMPTY_KEY);
        goto cleanup;
    }

    if (asc_json_builder_append_token(builder, asc_json_token_object_end()) != ASC_OK) {
        log_error("Failed to close object=[event]");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    result = _event_set_status(event_ptr, EVENT_STATUS_OK);
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to set event status, result=[%d]", result);
        goto cleanup;
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to build event, result=[%d]", result);

        _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);
    }

    return result;
}


asc_span event_get_id(event_t* event_ptr) {
    asc_span span = ASC_SPAN_NULL;

    if (event_ptr != NULL) {
        span = event_ptr->id;
    }

    return span;
}


asc_span event_get_name(event_t* event_ptr) {
    asc_span span = ASC_SPAN_NULL;

    if (event_ptr != NULL) {
        span = event_ptr->name;
    }

    return span;
}


asc_span event_get_payload_schema_version(event_t* event_ptr) {
    asc_span span = ASC_SPAN_NULL;

    if (event_ptr != NULL) {
        span = event_ptr->payload_schema_version;
    }

    return span;
}


asc_span event_get_category(event_t* event_ptr) {
    asc_span span = ASC_SPAN_NULL;

    if (event_ptr != NULL) {
        span = event_ptr->category;
    }

    return span;
}


asc_span event_get_type(event_t* event_ptr) {
    asc_span span = ASC_SPAN_NULL;

    if (event_ptr != NULL) {
        span = event_ptr->event_type;
    }

    return span;
}


time_t event_get_local_time(event_t* event_ptr) {
    time_t t = 0;

    if (event_ptr != NULL) {
        t = event_ptr->local_time;
    }

    return t;
}


bool event_is_empty(event_t* event_ptr) {
    bool is_empty = true;

    if (event_ptr != NULL) {
        is_empty = event_ptr->is_empty;
    }

    return is_empty;
}


uint32_t event_get_length(event_t* event_ptr) {
    if (event_ptr != NULL) {
        return (uint32_t)asc_span_length(asc_json_builder_span_get((asc_json_builder const*)&event_ptr->builder));
    }

    return 0;
}



uint32_t event_get_capacity(event_t* event_ptr) {
    if (event_ptr != NULL) {
        return (uint32_t)asc_span_capacity(asc_json_builder_span_get((asc_json_builder const*)&event_ptr->builder));
    }

    return 0;
}


bool event_can_append(event_t* event_ptr, asc_span payload) {
    return (uint32_t)asc_span_length(payload) + event_get_length(event_ptr) < event_get_capacity(event_ptr) - (uint32_t)EVENT_END_SIZE;
}


IOTSECURITY_RESULT _event_populate_extra_details(asc_json_builder* builder, asc_pair* extra_details, uint8_t extra_details_length) {
    log_debug("Start populating extra details to the event");
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    if (extra_details != NULL && extra_details_length > 0) {
        // create temporary asc_json_builder in order to attach EXTRA_DETAILS property to the event iff extra_details is not empty
        uint8_t buffer[SCHEMA_EXTRA_DETAILS_BUFFER_MAX_SIZE];
        memset(buffer, 0, SCHEMA_EXTRA_DETAILS_BUFFER_MAX_SIZE);

        asc_json_builder extra_details_builder = { 0 };

        if (asc_json_builder_init(&extra_details_builder, ASC_SPAN_FROM_BUFFER(buffer)) != ASC_OK) {
            log_error("Failed to create a extra details builder");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_token(&extra_details_builder, asc_json_token_object_start()) != ASC_OK) {
            log_error("Failed to set event extra details object start");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        // iterate over all the extra_details properties and add them to a buffer iff they were initialized correctly
        for (int i = 0; i < extra_details_length; i++) {
            asc_pair entry = extra_details[i];

            if (asc_span_length(entry.key) == 0 || asc_span_length(entry.value) == 0) {
                continue;
            }

            if (asc_json_builder_append_object(&extra_details_builder, entry.key, asc_json_token_string(entry.value)) != ASC_OK) {
                log_error("Failed to set event extra details property");
                result = IOTSECURITY_RESULT_EXCEPTION;
                goto cleanup;
            }
        }

        if (asc_json_builder_append_token(&extra_details_builder, asc_json_token_object_end()) != ASC_OK) {
            log_error("Failed to close extra details object close");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        // append extra_details to event builder iff extra details is not empty
        if (asc_span_length(extra_details_builder._internal.json) > (int32_t)sizeof("{}") - 1) {
            if (asc_json_builder_append_object(builder, asc_span_from_str((char*)EXTRA_DETAILS_KEY), asc_json_token_object(extra_details_builder._internal.json)) != ASC_OK) {
                log_error("Failed to set event extra details");
                result = IOTSECURITY_RESULT_EXCEPTION;
                goto cleanup;
            }
        }
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("Failed to set event extra details, result=[%d]", result);
    }

    return result;
}


#ifdef COLLECTOR_SYSTEM_INFORMATION_ENABLED
IOTSECURITY_RESULT event_append_system_information(event_t* event_ptr, system_information_t* data_ptr) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;
    asc_span os_name = ASC_SPAN_NULL;
    asc_span os_version = ASC_SPAN_NULL;
    asc_span os_architecture = ASC_SPAN_NULL;
    asc_span hostname = ASC_SPAN_NULL;
    uint32_t memory_total_physical_in_kb = 0;
    uint32_t memory_free_physical_in_kb = 0;

    if (data_ptr != NULL) {
        os_name = schema_system_information_get_os_name(data_ptr);
        os_version = schema_system_information_get_os_version(data_ptr);
        os_architecture = schema_system_information_get_os_architecture(data_ptr);
        hostname = schema_system_information_get_hostname(data_ptr);
        memory_total_physical_in_kb = schema_system_information_get_memory_total_physical_in_kb(data_ptr);
        memory_free_physical_in_kb = schema_system_information_get_memory_free_physical_in_kb(data_ptr);

        log_info("event_append_system_information, os_name=[%.*s], os_version=[%.*s], os_architecture=[%.*s], hostname=[%.*s], memory_total_physical_in_kb=[%d], memory_free_physical_in_kb=[%d]",
            asc_span_length(os_name), asc_span_ptr(os_name),
            asc_span_length(os_version), asc_span_ptr(os_version),
            asc_span_length(os_architecture), asc_span_ptr(os_architecture),
            asc_span_length(hostname), asc_span_ptr(hostname),
            memory_total_physical_in_kb,
            memory_free_physical_in_kb
        );
    } else {
        log_debug("event_append_system_information: data_ptr=[%p]", data_ptr);
    }

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("event_append_system_information bad argument exception");
        goto cleanup;
    }

    {
        // create temporary asc_json_builder in order to attach payload to the event iff no error occured
        uint8_t buffer[ASC_PAYLOAD_MAX_SIZE] = {0};
        memset(buffer, 0, ASC_PAYLOAD_MAX_SIZE);
    
        asc_json_builder builder = { 0 };

        if (asc_json_builder_init(&builder, ASC_SPAN_FROM_BUFFER(buffer)) != ASC_OK) {
            log_error("Failed to create payload builder");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_token(&builder, asc_json_token_object_start()) != ASC_OK) {
            log_error("Failed to initialize SystemInformation record JSON Value");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_OS_NAME_KEY), asc_json_token_string(os_name)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", SYSTEM_INFORMATION_OS_NAME_KEY, asc_span_length(os_name), asc_span_ptr(os_name));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_OS_VERSION_KEY), asc_json_token_string(os_version)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", SYSTEM_INFORMATION_OS_VERSION_KEY, asc_span_length(os_version), asc_span_ptr(os_version));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_OS_ARCHITECTURE_KEY), asc_json_token_string(os_architecture)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", SYSTEM_INFORMATION_OS_ARCHITECTURE_KEY, asc_span_length(os_architecture), asc_span_ptr(os_architecture));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_HOST_NAME_KEY), asc_json_token_string(hostname)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", SYSTEM_INFORMATION_HOST_NAME_KEY, asc_span_length(hostname), asc_span_ptr(hostname));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_TOTAL_PHYSICAL_MEMORY_KEY), asc_json_token_number(memory_total_physical_in_kb)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%d]", SYSTEM_INFORMATION_TOTAL_PHYSICAL_MEMORY_KEY, memory_total_physical_in_kb);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)SYSTEM_INFORMATION_FREE_PHYSICAL_MEMORY_KEY), asc_json_token_number(memory_free_physical_in_kb)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%d]", SYSTEM_INFORMATION_FREE_PHYSICAL_MEMORY_KEY, memory_free_physical_in_kb);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        result = _event_populate_extra_details(&builder, schema_system_information_get_extra_details(data_ptr), SYSTEM_INFORMATION_SCHEMA_EXTRA_DETAILS_ENTRIES);
        if (result != IOTSECURITY_RESULT_OK) {
            log_error("Failed to populate extra details to the event, result=[%d]", result);
            goto cleanup;
        }

        if (asc_json_builder_append_token(&builder, asc_json_token_object_end()) != ASC_OK) {
            log_error("Failed to close object");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (event_can_append(event_ptr, builder._internal.json)) {
            // append payload to the event
            if (asc_json_builder_append_token(&event_ptr->builder, asc_json_token_object(builder._internal.json)) != ASC_OK) {
                log_error("Failed to append payload to event");
                result = IOTSECURITY_RESULT_EXCEPTION;

                // event might be corrupted
                _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);

                goto cleanup;
            }

            // Set the event as non empty
            event_ptr->is_empty = false;
        } else {
            // payload exceeds event capacity
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("event_append_system_information failed, result=[%d]", result);
    }

    return result;
}
#endif


#ifdef COLLECTOR_CONNECTION_CREATE_ENABLED
static IOTSECURITY_RESULT _event_append_connection_create_with_direction(event_t* event_ptr, connection_create_t* data_ptr, const char* direction) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    // create temporary asc_json_builder in order to attach payload to the event iff no error occured
    uint8_t buffer[ASC_PAYLOAD_MAX_SIZE] = { 0 };
    memset(buffer, 0, ASC_PAYLOAD_MAX_SIZE);
    asc_json_builder builder = { 0 };
    char tmp_buffer[MAX_IPV6_STRING_LENGTH] = { 0 };
    TRANSPORT_PROTOCOL transport_protocol;
    asc_span protocol_value;
    asc_span direction_value;

    if (asc_json_builder_init(&builder, ASC_SPAN_FROM_BUFFER(buffer)) != ASC_OK) {
        log_error("Failed to create a new payload");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_token(&builder, asc_json_token_object_start()) != ASC_OK) {
        log_error("Failed to initialize ConnectionCreate record JSON Value");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    // Serialize local ip address
    schema_connection_create_serialize_local_ip(data_ptr, tmp_buffer);
    if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_LOCAL_ADDRESS_KEY), asc_json_token_string(asc_span_from_str(tmp_buffer))) != ASC_OK) {
        log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_LOCAL_ADDRESS_KEY, tmp_buffer);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    // Serialize remote ip address
    memset(tmp_buffer, 0, MAX_IPV6_STRING_LENGTH);
    schema_connection_create_serialize_remote_ip(data_ptr, tmp_buffer);
    if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_REMOTE_ADDRESS_KEY), asc_json_token_string(asc_span_from_str(tmp_buffer))) != ASC_OK) {
        log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_REMOTE_ADDRESS_KEY, tmp_buffer);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    // Serialize protocol type
    transport_protocol = schema_connection_create_get_transport_protocol(data_ptr);
    protocol_value = asc_span_from_str((char*)transport_protocol_to_str(transport_protocol));
    if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_PROTOCOL_KEY), asc_json_token_string(protocol_value)) != ASC_OK) {
        log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_PROTOCOL_KEY, tmp_buffer);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (transport_protocol != TRANSPORT_PROTOCOL_ICMP) {
        // Serialize local port
        memset(tmp_buffer, 0, MAX_IPV6_STRING_LENGTH);
        snprintf(tmp_buffer, MAX_IPV6_STRING_LENGTH, "%u", schema_connection_create_get_local_port(data_ptr));
        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_LOCAL_PORT_KEY), asc_json_token_string(asc_span_from_str(tmp_buffer))) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_LOCAL_PORT_KEY, tmp_buffer);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        // Serialize remote port
        memset(tmp_buffer, 0, MAX_IPV6_STRING_LENGTH);
        snprintf(tmp_buffer, MAX_IPV6_STRING_LENGTH, "%u", schema_connection_create_get_remote_port(data_ptr));
        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_REMOTE_PORT_KEY), asc_json_token_string(asc_span_from_str(tmp_buffer))) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_REMOTE_PORT_KEY, tmp_buffer);
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
    }

    // Serialize connection direction
    direction_value = asc_span_from_str((char*)direction);
    if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)CONNECTION_CREATE_DIRECTION_KEY), asc_json_token_string(direction_value)) != ASC_OK) {
        log_error("Failed to set string key=[%s], value=[%s]", CONNECTION_CREATE_DIRECTION_KEY, tmp_buffer);
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (asc_json_builder_append_token(&builder, asc_json_token_object_end()) != ASC_OK) {
        log_error("Failed to close object");
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

    if (event_can_append(event_ptr, builder._internal.json)) {
        // append payload to the event
        if (asc_json_builder_append_array_item(&event_ptr->builder, asc_json_token_object(asc_json_builder_span_get(&builder))) != ASC_OK) {
            log_error("Failed to append payload to event");
            result = IOTSECURITY_RESULT_EXCEPTION;

            // event might be corrupted
            _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);

            goto cleanup;
        }

        event_ptr->is_empty = false;
    } else {
        // payload exceeds event capacity
        result = IOTSECURITY_RESULT_EXCEPTION;
        goto cleanup;
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("_event_append_connection_create_with_direction failed, result=[%d]", result);
    }

    return result;
}


IOTSECURITY_RESULT event_append_connection_create(event_t* event_ptr, connection_create_t* data_ptr) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("event_append_connection_create bad argument exception");
        goto cleanup;
    }

    schema_connection_create_log_info(data_ptr);

    if (schema_connection_create_get_bytes_in(data_ptr) > 0) {
        result = _event_append_connection_create_with_direction(event_ptr, data_ptr, CONNECTION_CREATE_DIRECTION_INBOUND_NAME);
        if (result != IOTSECURITY_RESULT_OK) {
            log_error("event_append_connection_create with direction=[%s] failed, result=[%d]", CONNECTION_CREATE_DIRECTION_INBOUND_NAME, result);
            goto cleanup;
        }
    }


    if (schema_connection_create_get_bytes_out(data_ptr) > 0) {
        result = _event_append_connection_create_with_direction(event_ptr, data_ptr, CONNECTION_CREATE_DIRECTION_OUTBOUND_NAME);
        if (result != IOTSECURITY_RESULT_OK) {
            log_error("event_append_connection_create with direction=[%s] failed, result=[%d]", CONNECTION_CREATE_DIRECTION_OUTBOUND_NAME, result);
            goto cleanup;
        }
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("event_append_connection_create failed, result=[%d]", result);
    }

    return result;
}
#endif


#ifdef COLLECTOR_LISTENING_PORTS_ENABLED
IOTSECURITY_RESULT event_append_listening_ports(event_t* event_ptr, listening_ports_t* data_ptr) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;
    asc_span protocol = ASC_SPAN_NULL;
    asc_span local_address = ASC_SPAN_NULL;
    asc_span local_port = ASC_SPAN_NULL;
    asc_span remote_address = ASC_SPAN_NULL;
    asc_span remote_port = ASC_SPAN_NULL;

    if (data_ptr != NULL) {
        protocol = schema_listening_ports_get_protocol(data_ptr);
        local_address = schema_listening_ports_get_local_address(data_ptr);
        local_port = schema_listening_ports_get_local_port(data_ptr);
        remote_address = schema_listening_ports_get_remote_address(data_ptr);
        remote_port = schema_listening_ports_get_remote_port(data_ptr);

        log_info("event_append_listening_ports, protocol=[%.*s], local_address=[%.*s], local_port=[%.*s], remote_address=[%.*s], remote_port=[%.*s]",
            asc_span_length(protocol), asc_span_ptr(protocol),
            asc_span_length(local_address), asc_span_ptr(local_address),
            asc_span_length(local_port), asc_span_ptr(local_port),
            asc_span_length(remote_address), asc_span_ptr(remote_address),
            asc_span_length(remote_port), asc_span_ptr(remote_port)
        );
    } else {
        log_debug("event_append_listening_ports: data_ptr=[%p]", data_ptr);
    }

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("event_append_listening_ports bad argument exception");
        goto cleanup;
    }

    if (data_ptr != NULL) {
        // create temporary asc_json_builder in order to attach payload to the event iff no error occured
        uint8_t buffer[ASC_PAYLOAD_MAX_SIZE];
        memset(buffer, 0, ASC_PAYLOAD_MAX_SIZE);
        asc_json_builder builder = { 0 };

        if (asc_json_builder_init(&builder, ASC_SPAN_FROM_BUFFER(buffer)) != ASC_OK) {
            log_error("Failed to create payload builder");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_token(&builder, asc_json_token_object_start()) != ASC_OK) {
            log_error("Failed to initialize ListeningPorts record JSON Value");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)LISTENING_PORTS_PROTOCOL_KEY), asc_json_token_string(protocol)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", LISTENING_PORTS_PROTOCOL_KEY, asc_span_length(protocol), asc_span_ptr(protocol));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)LISTENING_PORTS_LOCAL_ADDRESS_KEY), asc_json_token_string(local_address)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", LISTENING_PORTS_LOCAL_ADDRESS_KEY, asc_span_length(local_address), asc_span_ptr(local_address));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)LISTENING_PORTS_LOCAL_PORT_KEY), asc_json_token_string(local_port)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", LISTENING_PORTS_LOCAL_PORT_KEY, asc_span_length(local_port), asc_span_ptr(local_port));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)LISTENING_PORTS_REMOTE_ADDRESS_KEY), asc_json_token_string(remote_address)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", LISTENING_PORTS_REMOTE_ADDRESS_KEY, asc_span_length(remote_address), asc_span_ptr(remote_address));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (asc_json_builder_append_object(&builder, asc_span_from_str((char*)LISTENING_PORTS_REMOTE_PORT_KEY), asc_json_token_string(remote_port)) != ASC_OK) {
            log_error("Failed to set string key=[%s], value=[%.*s]", LISTENING_PORTS_REMOTE_PORT_KEY, asc_span_length(remote_port), asc_span_ptr(remote_port));
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        result = _event_populate_extra_details(&builder, schema_listening_ports_get_extra_details(data_ptr), SCHEMA_LISTENING_PORTS_EXTRA_DETAILS_ENTRIES);
        if (result != IOTSECURITY_RESULT_OK) {
            log_error("Failed to populate extra details to the event, result=[%d]", result);
            goto cleanup;
        }

        if (asc_json_builder_append_token(&builder, asc_json_token_object_end()) != ASC_OK) {
            log_error("Failed to close object");
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }

        if (event_can_append(event_ptr, builder._internal.json)) {
            // append payload to the event
            if (asc_json_builder_append_token(&event_ptr->builder, asc_json_token_object(builder._internal.json)) != ASC_OK) {
                log_error("Failed to append payload to event");
                result = IOTSECURITY_RESULT_EXCEPTION;

                // event might be corrupted
                _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);

                goto cleanup;
            }

            // Set the event as non empty
            event_ptr->is_empty = false;
        } else {
            // payload exceeds event capacity
            result = IOTSECURITY_RESULT_EXCEPTION;
            goto cleanup;
        }
    }

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("event_append_listening_ports failed, result=[%d]", result);
    }

    return result;
}
#endif

#ifdef COLLECTOR_HEARTBEAT_ENABLED
IOTSECURITY_RESULT event_append_heartbeat(event_t* event_ptr, schema_heartbeat_t* data_ptr) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    if (event_ptr == NULL) {
        result = IOTSECURITY_RESULT_BAD_ARGUMENT;
        log_error("event_append_heartbeat bad argument exception");
        goto cleanup;
    }

    // Set the event as empty
    event_ptr->is_empty = true;

cleanup:
    if (result != IOTSECURITY_RESULT_OK) {
        log_error("event_append_system_information failed, result=[%d]", result);

        _event_set_status(event_ptr, EVENT_STATUS_EXCEPTION);
    }

    return result;
}
#endif


static IOTSECURITY_RESULT _event_set_status(event_t* event_ptr, EVENT_STATUS status) {
    IOTSECURITY_RESULT result = IOTSECURITY_RESULT_OK;

    if (event_ptr != NULL) {
        event_ptr->status = status;
    }

    return result;
}

