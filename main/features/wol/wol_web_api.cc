#include "wol_web_api.h"

#include "cjson_utils.h"
#include "features/web/web_server.h"
#include "wol.h"

#include <esp_log.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define TAG "WolWebApi"

namespace {

std::string ComputersJson(const std::vector<wol::Computer>& computers) {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr list(cJSON_CreateArray());
    if (root == nullptr || list == nullptr) {
        return "";
    }
    for (const auto& computer : computers) {
        CJsonUniquePtr item(cJSON_CreateObject());
        if (item == nullptr) {
            return "";
        }
        cJSON_AddStringToObject(item.get(), "key", computer.key.c_str());
        cJSON_AddStringToObject(item.get(), "name", computer.name.c_str());
        cJSON_AddStringToObject(item.get(), "mac", computer.mac.c_str());
        cJSON_AddStringToObject(item.get(), "broadcast", computer.broadcast.c_str());
        cJSON_AddNumberToObject(item.get(), "port", computer.port);
        cJSON_AddBoolToObject(item.get(), "default", computer.is_default);
        cJSON_AddItemToArray(list.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "computers", list.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "";
}

// Reads the "key" query parameter of the request URI, empty when absent.
std::string QueryKey(httpd_req_t* req) {
    char key[64] = {0};
    const char* query = std::strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    // httpd_query_key_value wants the query string itself, not the whole URI.
    if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK) {
        return "";
    }
    return key;
}

// Fills a computer from a JSON object; key and unmentioned fields stay as they are.
bool ReadComputer(const cJSON* root, wol::Computer& computer, std::string& error) {
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* mac = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(name) || !cJSON_IsString(mac)) {
        error = "Ожидались строки \"name\" и \"mac\"";
        return false;
    }
    computer.name = name->valuestring;
    computer.mac = mac->valuestring;
    if (cJSON* broadcast = cJSON_GetObjectItem(root, "broadcast");
        cJSON_IsString(broadcast)) {
        computer.broadcast = broadcast->valuestring;
    }
    if (cJSON* port = cJSON_GetObjectItem(root, "port"); cJSON_IsNumber(port)) {
        computer.port = static_cast<int>(port->valuedouble);
    }
    // The flag arrives with the starred computer only; the form does not send it.
    if (cJSON* is_default = cJSON_GetObjectItem(root, "default"); cJSON_IsBool(is_default)) {
        computer.is_default = cJSON_IsTrue(is_default);
    }
    return true;
}

// Replaces the whole list, used by the import button of the page. Keys are
// generated here, so a file from another device cannot break the saved list.
bool ReadImportedComputers(const cJSON* array, std::vector<wol::Computer>& computers,
                           std::string& error) {
    computers.clear();
    int position = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        ++position;
        if (!cJSON_IsObject(item)) {
            error = "Запись " + std::to_string(position) + " — не объект JSON";
            return false;
        }
        wol::Computer imported;
        if (!ReadComputer(item, imported, error)) {
            error = "Запись " + std::to_string(position) + ": " + error;
            return false;
        }
        imported.key = wol::NextKey(computers);
        computers.push_back(std::move(imported));
    }
    return true;
}

esp_err_t SendOk(httpd_req_t* req) {
    return web::SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t HandleComputers(httpd_req_t* req) {
    std::vector<wol::Computer> computers = wol::LoadComputers();

    if (req->method == HTTP_GET) {
        return web::SendJson(req, 200, ComputersJson(computers));
    }

    if (req->method == HTTP_DELETE) {
        const std::string key = QueryKey(req);
        if (key.empty()) {
            return web::SendError(req, 400, "Missing key query parameter");
        }
        const auto found =
            std::find_if(computers.begin(), computers.end(),
                         [&key](const auto& computer) { return computer.key == key; });
        if (found == computers.end()) {
            return web::SendError(req, 404, "Computer not found");
        }
        computers.erase(found);
        std::string error;
        if (!wol::SaveComputers(computers, error)) {
            return web::SendError(req, 500, error);
        }
        return SendOk(req);
    }

    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || (!cJSON_IsObject(root.get()) && !cJSON_IsArray(root.get()))) {
        return web::SendError(req, 400, "Ожидался объект JSON");
    }

    // POST with a "computers" array (or a bare array) replaces the whole list,
    // the same way the RSS tab imports its feeds.
    if (req->method == HTTP_POST) {
        const cJSON* list = cJSON_IsArray(root.get())
                                ? root.get()
                                : cJSON_GetObjectItem(root.get(), "computers");
        if (cJSON_IsArray(list)) {
            std::string error;
            if (!ReadImportedComputers(list, computers, error)) {
                return web::SendError(req, 400, error);
            }
            if (!wol::SaveComputers(computers, error)) {
                return web::SendError(req, 400, error);
            }
            ESP_LOGI(TAG, "Imported %d computers", static_cast<int>(computers.size()));
            return web::SendJson(req, 200,
                                 "{\"ok\":true,\"count\":" +
                                     std::to_string(computers.size()) + "}");
        }
    }

    if (!cJSON_IsObject(root.get())) {
        return web::SendError(req, 400, "Ожидался объект JSON");
    }

    std::string error;
    if (req->method == HTTP_PUT) {
        const std::string key = QueryKey(req);
        if (key.empty()) {
            return web::SendError(req, 400, "Missing key query parameter");
        }
        const auto found =
            std::find_if(computers.begin(), computers.end(),
                         [&key](const auto& computer) { return computer.key == key; });
        if (found == computers.end()) {
            return web::SendError(req, 404, "Computer not found");
        }
        wol::Computer updated = *found;
        if (!ReadComputer(root.get(), updated, error)) {
            return web::SendError(req, 400, error);
        }
        *found = updated;
        if (!wol::SaveComputers(computers, error)) {
            return web::SendError(req, 400, error);
        }
        return SendOk(req);
    }

    // POST: add a computer with a fresh key.
    wol::Computer added;
    added.key = wol::NextKey(computers);
    if (!ReadComputer(root.get(), added, error)) {
        return web::SendError(req, 400, error);
    }
    computers.push_back(added);
    if (!wol::SaveComputers(computers, error)) {
        return web::SendError(req, 400, error);
    }
    ESP_LOGI(TAG, "Added computer %s", added.key.c_str());
    return SendOk(req);
}

esp_err_t HandleWake(httpd_req_t* req) {
    const std::string key = QueryKey(req);
    wol::Computer computer;
    std::string error;
    if (!wol::Wake(key, computer, error)) {
        ESP_LOGW(TAG, "Wake request failed: %s", error.c_str());
        return web::SendError(req, 400, error);
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return SendOk(req);
    }
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddStringToObject(root.get(), "name", computer.name.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    if (text == nullptr) {
        return SendOk(req);
    }
    return web::SendJson(req, 200, text.get());
}

const httpd_uri_t kRoutes[] = {
    {.uri = "/api/wol", .method = HTTP_GET, .handler = HandleComputers, .user_ctx = nullptr},
    {.uri = "/api/wol", .method = HTTP_POST, .handler = HandleComputers, .user_ctx = nullptr},
    {.uri = "/api/wol", .method = HTTP_PUT, .handler = HandleComputers, .user_ctx = nullptr},
    {.uri = "/api/wol", .method = HTTP_DELETE, .handler = HandleComputers, .user_ctx = nullptr},
    {.uri = "/api/wol/wake", .method = HTTP_POST, .handler = HandleWake, .user_ctx = nullptr},
};

}  // namespace

void WolWebApi::Register(WebServer& server) {
    for (const auto& route : kRoutes) {
        server.AddHandler(route);
    }
}
