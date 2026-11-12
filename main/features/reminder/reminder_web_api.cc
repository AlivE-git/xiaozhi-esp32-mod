#include "reminder_web_api.h"

#include "cjson_utils.h"
#include "features/reminder/reminder_store.h"
#include "features/web/web_server.h"

#include <esp_log.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TAG "ReminderWebApi"

namespace {

std::string RemindersJson() {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{}";
    }
    cJSON* array = cJSON_AddArrayToObject(root.get(), "reminders");
    std::vector<reminder::Item> items;
    reminder::List(items);
    if (array != nullptr) {
        for (const auto& item : items) {
            cJSON* entry = cJSON_CreateObject();
            if (entry == nullptr) {
                break;
            }
            cJSON_AddStringToObject(entry, "text", item.text.c_str());
            cJSON_AddStringToObject(entry, "when", reminder::WhenText(item.due).c_str());
            cJSON_AddNumberToObject(entry, "due", static_cast<double>(item.due));
            cJSON_AddItemToArray(array, entry);
        }
    }
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t HandleDelete(httpd_req_t* req) {
    char index[16] = {0};
    // httpd_query_key_value wants the query string itself, not the full URI.
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "index", index, sizeof(index)) != ESP_OK ||
        index[0] == '\0') {
        return web::SendError(req, 400, "Не указан номер напоминания");
    }
    std::string error;
    if (!reminder::Remove(static_cast<size_t>(std::atoi(index)), error)) {
        return web::SendError(req, 404, error);
    }
    ESP_LOGI(TAG, "Removed reminder %s", index);
    return web::SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t HandleAdd(httpd_req_t* req) {
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* text_item = root != nullptr ? cJSON_GetObjectItem(root.get(), "text") : nullptr;
    if (!cJSON_IsString(text_item) || text_item->valuestring == nullptr ||
        text_item->valuestring[0] == '\0') {
        return web::SendError(req, 400, "Не указан текст напоминания");
    }
    const std::string text = text_item->valuestring;

    cJSON* at_item = cJSON_GetObjectItem(root.get(), "at");
    cJSON* date_item = cJSON_GetObjectItem(root.get(), "date");
    cJSON* minutes_item = cJSON_GetObjectItem(root.get(), "minutes");
    reminder::Item added;
    std::string error;
    const std::string at = cJSON_IsString(at_item) && at_item->valuestring != nullptr
                               ? at_item->valuestring
                               : "";
    const std::string date = cJSON_IsString(date_item) && date_item->valuestring != nullptr
                                 ? date_item->valuestring
                                 : "";
    if (!at.empty() || !date.empty()) {
        if (!reminder::AddAt(date, at, text, added, error)) {
            return web::SendError(req, 400, error);
        }
    } else {
        int minutes = 10;
        if (cJSON_IsNumber(minutes_item)) {
            minutes = static_cast<int>(minutes_item->valuedouble);
        }
        if (!reminder::AddIn(minutes > 0 ? minutes : 10, text, added, error)) {
            return web::SendError(req, 400, error);
        }
    }

    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddStringToObject(result.get(), "text", added.text.c_str());
    cJSON_AddStringToObject(result.get(), "when", reminder::WhenText(added.due).c_str());
    CJsonStringUniquePtr json(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, 201, json != nullptr ? json.get() : "{\"ok\":true}");
}

esp_err_t HandleReminders(httpd_req_t* req) {
    switch (req->method) {
        case HTTP_GET:
            return web::SendJson(req, 200, RemindersJson());
        case HTTP_POST:
            return HandleAdd(req);
        case HTTP_DELETE:
            return HandleDelete(req);
        default:
            return web::SendError(req, 400, "Unsupported method");
    }
}

// One handler for every method: httpd counts each uri/method pair separately and
// its slots are scarce, while HandleReminders() already dispatches on the method.
const httpd_uri_t kRoutes[] = {
    {.uri = "/api/reminders", .method = static_cast<httpd_method_t>(HTTP_ANY), .handler = HandleReminders,
     .user_ctx = nullptr},
};

}  // namespace

namespace reminder {

void ReminderWebApi::Register(WebServer& server) {
    for (const auto& route : kRoutes) {
        server.AddHandler(route);
    }
}

}  // namespace reminder
