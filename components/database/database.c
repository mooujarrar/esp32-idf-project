#include <stdio.h>
#include "database.h"

ESP_EVENT_DEFINE_BASE(DB_EVENTS);

static db_event_handle_t db_handler;

static esp_err_t read_time_entry(nvs_handle_t* ptr, const char* key, db_data_array_t* db_data) {
    esp_err_t err;
    time_blob_t* valuePtr = (time_blob_t*) malloc(sizeof(time_blob_t));
    size_t length = sizeof(time_blob_t);
    err = nvs_get_blob(*ptr, key, valuePtr, &length);
    if (err != ESP_OK) return err;
    db_data->size++;
    db_data->array = (db_data_t) realloc(db_data->array, db_data->size * sizeof(db_data_entry_t));
    char* tag = (char*) malloc(sizeof(valuePtr->tag));
    strcpy(tag, valuePtr->tag); 
    db_data_entry_t new_data_entry = {
        .time = key,
        .card_tag = tag,
        .direction = valuePtr->direction
    };
    db_data->array[(db_data->size) - 1] = new_data_entry;
    //printf("time '%s', tag '%s', direction '%d'\n", key, valuePtr->tag, valuePtr->direction);
    free(valuePtr);
    return ESP_OK;
}

static esp_err_t db_save_time_entry(time_blob_t* time_entry_ptr)
{
    nvs_handle_t time_table_handle;
    esp_err_t err;

    char buf[40];
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    err = ds3231_get_time(&ds3231, &timeinfo);
    if (err != ESP_OK) return err;
    snprintf(buf, sizeof buf - 1, "%" PRIu64, (u_int64_t)mktime(&timeinfo));

    char* timePtr = (char*) malloc(sizeof(buf));
    strcpy(timePtr, buf);

    // Open
    err = nvs_open(TIME_STORAGE_NAMESPACE, NVS_READWRITE, &time_table_handle);
    if (err != ESP_OK) return err;
    // Save
    err = nvs_set_blob(time_table_handle, timePtr, time_entry_ptr, sizeof(time_blob_t));
    if (err != ESP_OK) return err;

    // Commit written value.
    // After setting any values, nvs_commit() must be called to ensure changes are written
    // to flash storage. Implementations may write to storage at other times,
    // but this is not guaranteed.
    err = nvs_commit(time_table_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(time_table_handle);
    return ESP_OK;
}

static esp_err_t db_dispatch_event(db_event_handle_t db, db_event_t event, void* data)
{
    if(!db) {
        return ESP_ERR_INVALID_ARG;
    }

    db_event_data_t e_data = {
        .db = db,
        .ptr = data,
    };
    esp_err_t err;
    if(ESP_OK != (err = esp_event_post_to(db->event_handle, DB_EVENTS, event, &e_data, sizeof(db_event_data_t), portMAX_DELAY))) {
        return err;
    }

    return esp_event_loop_run(db->event_handle, 0);
}

esp_err_t db_save_tag(uint64_t tag)
{
    nvs_handle_t tags_table_handle;
    esp_err_t err;

    // Open
    err = nvs_open(TAGS_STORAGE_NAMESPACE, NVS_READWRITE, &tags_table_handle);
    if (err != ESP_OK) return err;

    char picc_tag[25];
    snprintf(picc_tag, sizeof picc_tag - 1, "%" PRIu64, tag);

    // Read
    int8_t val = 0;
    err = nvs_get_i8(tags_table_handle, picc_tag, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    // tag found we should clear it => the employee is quitting
    else if (err == ESP_OK) {
        err = nvs_erase_key(tags_table_handle, picc_tag);
        if (err != ESP_OK) return err;
        else {
            time_blob_t* time_entry = (time_blob_t*) malloc (sizeof(time_blob_t));
            time_entry->direction = OUT;
            memcpy(time_entry->tag, picc_tag, 25);
            db_save_time_entry(time_entry);
            free(time_entry);
        }
    }
    // tag not found => the employee is entering
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_i8(tags_table_handle, picc_tag, 0);
        if (err != ESP_OK) return err;
        else {
            time_blob_t* time_entry = (time_blob_t*) malloc (sizeof(time_blob_t));
            time_entry->direction = IN;
            memcpy(time_entry->tag, picc_tag, 25);
            db_save_time_entry(time_entry);
            free(time_entry);
        }
    } 

    // Commit written value.
    // After setting any values, nvs_commit() must be called to ensure changes are written
    // to flash storage. Implementations may write to storage at other times,
    // but this is not guaranteed.
    err = nvs_commit(tags_table_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(tags_table_handle);
    return ESP_OK;
}

esp_err_t db_read_attendance() {
    nvs_handle_t time_table_handle;
    esp_err_t err;

    // Open
    err = nvs_open(TIME_STORAGE_NAMESPACE, NVS_READWRITE, &time_table_handle);
    if (err != ESP_OK) return err;

    // Example of listing all the key-value pairs of any type under specified partition and namespace
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, TIME_STORAGE_NAMESPACE, NVS_TYPE_ANY, &it);
    db_data_array_t db_data = {
        .array = NULL,
        .size = 0
    };
    while(res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info); // Can omit error check if parameters are guaranteed to be non-NULL
        err = read_time_entry(&time_table_handle, info.key, &db_data);
        if (err != ESP_OK) return err;
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    db_dispatch_event(db_handler, DB_EVENT_TABLE_READ, &db_data);

    // Close
    nvs_close(time_table_handle);
    return ESP_OK;
}

esp_err_t db_register_events(db_event_handle_t db, db_event_t event, esp_event_handler_t event_handler, void* event_handler_arg)
{
    db_handler = db;
    if(!db) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_event_handler_register_with(db->event_handle, DB_EVENTS, event, event_handler, event_handler_arg);
}

esp_err_t db_unregister_events(db_event_handle_t db, db_event_t event, esp_event_handler_t event_handler)
{
    db_handler = NULL;
    if(!db) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_event_handler_unregister_with(db->event_handle, DB_EVENTS, event, event_handler);
}