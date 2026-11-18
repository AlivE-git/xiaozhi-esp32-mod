#include "web_search_web_api.h"

#include "cjson_utils.h"
#include "features/web/web_server.h"
#include "features/websearch/web_search.h"

#include <esp_log.h>

#include <string>

#define TAG "WebSearchWebApi"

namespace {

esp_err_t HandleConfig(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        const websearch::Config config = websearch::Load();
        CJsonUniquePtr root(cJSON_CreateObject());
        if (root == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON_AddStringToObject(root.get(), "deployment_id", config.deployment_id.c_str());
        cJSON_AddStringToObject(root.get(), "token", config.token.c_str());
        cJSON_AddBoolToObject(root.get(), "configured", websearch::IsConfigured());
        CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        return web::SendError(req, 400, "Ожидался объект JSON");
    }
    cJSON* deployment_id = cJSON_GetObjectItem(root.get(), "deployment_id");
    cJSON* token = cJSON_GetObjectItem(root.get(), "token");
    if (!cJSON_IsString(deployment_id) || !cJSON_IsString(token)) {
        return web::SendError(req, 400, "Ожидались строки \"deployment_id\" и \"token\"");
    }

    std::string error;
    if (!websearch::Save(deployment_id->valuestring, token->valuestring, error)) {
        return web::SendError(req, 400, error);
    }
    // The token itself is never logged: only whether the pair is complete.
    ESP_LOGI(TAG, "Web search settings saved (configured=%d)",
             websearch::IsConfigured() ? 1 : 0);
    return web::SendJson(req, 200, "{\"ok\":true}");
}

const httpd_uri_t kRoutes[] = {
    {.uri = "/api/websearch", .method = HTTP_GET, .handler = HandleConfig, .user_ctx = nullptr},
    {.uri = "/api/websearch", .method = HTTP_POST, .handler = HandleConfig, .user_ctx = nullptr},
};

}  // namespace

void WebSearchWebApi::Register(WebServer& server) {
    for (const auto& route : kRoutes) {
        server.AddHandler(route);
    }
}
