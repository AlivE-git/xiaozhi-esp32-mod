#include "radio_web_server.h"

#include "board.h"
#include "cjson_utils.h"
#include "esp32_radio.h"

#include <esp_log.h>

#include <cstring>
#include <vector>

#define TAG "RadioWeb"

namespace {

// Large enough for one station and for a full imported station list.
constexpr size_t kMaxBodySize = 16384;

// Compact station management page. The form fields map 1:1 to the JSON API:
// key, name, url, description, genre, volume and aliases ("Keywords used as
// extra aliases", comma separated).
const char kPageHtml[] = "<!-- radio web ui build " __DATE__ " " __TIME__ " -->\n" R"HTML(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XiaoZhi — Радиостанции</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
<style>
body{background:#f1f4f8}
.app-header{background:linear-gradient(135deg,#111827,#1f3a5f);}
.app-header .badge{background:#f59e0b;color:#111827}
.card{border:0;box-shadow:0 .125rem .5rem rgba(15,23,42,.08);border-radius:.75rem}
.card-header{background:#fff;border-bottom:1px solid #e5e7eb;font-weight:600}
.url{font-size:.82rem;word-break:break-all}
.actions{white-space:nowrap;text-align:right}
</style></head>
<body>
<header class="app-header text-white shadow-sm">
 <div class="container py-3 d-flex align-items-center gap-3">
  <span class="fs-3">📻</span>
  <div>
   <h1 class="h4 mb-0">Радиостанции</h1>
   <div class="small text-white-50">Управление списком станций XiaoZhi</div>
  </div>
  <div class="ms-auto d-flex align-items-center gap-2">
   <span id="nowPlaying" class="badge text-bg-success d-none">▶</span>
   <span id="countBadge" class="badge rounded-pill">0 станций</span>
  </div>
 </div>
</header>
<main class="container py-4">
<div id="msg" class="alert d-none" role="alert"></div>

<section class="card mb-4">
 <div class="card-header" id="formTitle">➕ Добавить станцию</div>
 <div class="card-body">
  <form id="stationForm">
   <div class="row g-3">
    <div class="col-md-6">
     <label class="form-label" for="key">Ключ <span class="text-muted">(уникальный ID, напр. MAYAK)</span></label>
     <input id="key" class="form-control" required autocomplete="off">
    </div>
    <div class="col-md-6">
     <label class="form-label" for="name">Название</label>
     <input id="name" class="form-control" required autocomplete="off">
    </div>
    <div class="col-12">
     <label class="form-label" for="url">Stream URL <span class="text-muted">(http/https)</span></label>
     <input id="url" class="form-control" type="url" required autocomplete="off">
    </div>
    <div class="col-md-6">
     <label class="form-label" for="description">Описание</label>
     <input id="description" class="form-control" autocomplete="off">
    </div>
    <div class="col-md-3">
     <label class="form-label" for="genre">Жанр</label>
     <input id="genre" class="form-control" autocomplete="off">
    </div>
    <div class="col-md-3">
     <label class="form-label" for="volume">Громкость <span class="text-muted">(0.1–4.0)</span></label>
     <input id="volume" class="form-control" type="number" step="0.1" min="0.1" max="4" value="1.0">
    </div>
    <div class="col-12">
     <label class="form-label" for="aliases">Ключевые слова — дополнительные алиасы <span class="text-muted">(через запятую)</span></label>
     <input id="aliases" class="form-control" placeholder="маяк, mayak">
    </div>
    <div class="col-12 d-flex gap-2">
     <button class="btn btn-primary" type="submit">💾 Сохранить станцию</button>
     <button class="btn btn-outline-secondary d-none" type="button" id="cancelBtn">Отменить редактирование</button>
    </div>
   </div>
  </form>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">⇅ Импорт / Экспорт</div>
 <div class="card-body d-flex flex-wrap gap-2 align-items-center">
  <button class="btn btn-outline-success" type="button" id="exportBtn">⬇ Экспорт станций (JSON)</button>
  <button class="btn btn-outline-warning" type="button" id="importBtn">⬆ Импорт и замена всех станций</button>
  <input type="file" id="importFile" accept=".json,application/json" class="d-none">
  <span class="text-muted small">Импорт полностью заменяет текущий список станций.</span>
 </div>
</section>

<section class="card">
 <div class="card-header">📡 Станции</div>
 <div class="table-responsive">
  <table class="table table-hover align-middle mb-0">
   <thead class="table-dark"><tr><th>Ключ</th><th>Название</th><th>URL</th><th>Алиасы</th><th class="text-end">Действия</th></tr></thead>
   <tbody id="rows"></tbody>
  </table>
 </div>
</section>
<footer class="text-center text-muted small py-4">XiaoZhi Radio · локальный веб-интерфейс устройства</footer>
</main>
<script>
let editingKey=null;
let stationsCache=[];
let playback={playing:false,currentKey:'',currentStation:''};
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function msg(t,ok){
  const el=document.getElementById('msg');
  el.textContent=t;
  el.className='alert '+(ok===false?'alert-danger':'alert-success');
}
function isCurrent(s){return playback.playing&&playback.currentKey===s.key}
function renderRows(){
  const stations=stationsCache;
  document.getElementById('countBadge').textContent=stations.length+' станций';
  const tb=document.getElementById('rows');tb.innerHTML='';
  if(stations.length===0){
    tb.innerHTML='<tr><td colspan="5" class="text-center text-muted py-4">Станций пока нет. Добавьте первую через форму выше.</td></tr>';
    return;}
  stations.forEach(s=>{
    const current=isCurrent(s);
    const tr=document.createElement('tr');
    if(current)tr.className='table-success';
    const genre=s.genre?' <span class="badge text-bg-secondary">'+esc(s.genre)+'</span>':'';
    const playingBadge=current?' <span class="badge text-bg-success">▶ играет</span>':'';
    const desc=s.description?'<div class="small text-muted">'+esc(s.description)+'</div>':'';
    tr.innerHTML='<td><code>'+esc(s.key)+'</code></td>'
      +'<td><strong>'+esc(s.name)+'</strong>'+genre+playingBadge+desc+'</td>'
      +'<td class="url">'+esc(s.url)+'</td>'
      +'<td>'+esc((s.aliases||[]).join(', '))+'</td>';
    const td=document.createElement('td');td.className='actions';
    const pb=document.createElement('button');
    if(current){pb.className='btn btn-sm btn-danger me-1';pb.textContent='⏹';pb.onclick=()=>stopPlayback(pb);}
    else{pb.className='btn btn-sm btn-success me-1';pb.textContent='▶';pb.onclick=()=>playStation(s.key,pb);}
    const eb=document.createElement('button');eb.className='btn btn-sm btn-outline-primary me-1';eb.textContent='✏️';
    eb.onclick=()=>startEdit(s);
    const db=document.createElement('button');db.className='btn btn-sm btn-outline-danger';db.textContent='🗑';
    db.onclick=async()=>{if(!confirm('Удалить станцию '+s.key+'?'))return;
      const rr=await fetch('/api/stations?key='+encodeURIComponent(s.key),{method:'DELETE'});
      msg(rr.ok?'Станция удалена':'Не удалось удалить',rr.ok);load();};
    td.append(pb,eb,db);tr.appendChild(td);tb.appendChild(tr);});
}
async function load(){
  const r=await fetch('/api/stations');const data=await r.json();
  stationsCache=data.stations||[];
  renderRows();
}
async function loadStatus(){
  try{
    const r=await fetch('/api/status');
    if(!r.ok)return;
    const st=await r.json();
    const changed=st.playing!==playback.playing||st.currentKey!==playback.currentKey;
    playback={playing:!!st.playing,currentKey:st.currentKey||'',currentStation:st.currentStation||''};
    const np=document.getElementById('nowPlaying');
    if(playback.playing){np.textContent='▶ '+playback.currentStation;np.classList.remove('d-none');}
    else{np.classList.add('d-none');}
    if(changed)renderRows();
  }catch(e){/* device unreachable; keep last state */}
}
async function playStation(key,btn){
  btn.disabled=true;
  const r=await fetch('/api/play?key='+encodeURIComponent(key),{method:'POST'});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'▶ Воспроизведение запущено':'Не удалось запустить: '+(res.error||r.status),r.ok);
  await loadStatus();
}
async function stopPlayback(btn){
  btn.disabled=true;
  const r=await fetch('/api/stop',{method:'POST'});
  msg(r.ok?'⏹ Воспроизведение остановлено':'Не удалось остановить',r.ok);
  await loadStatus();
}
function startEdit(s){
  editingKey=s.key;
  document.getElementById('formTitle').textContent='✏️ Редактирование станции '+s.key;
  document.getElementById('key').value=s.key;document.getElementById('key').readOnly=true;
  document.getElementById('name').value=s.name||'';
  document.getElementById('url').value=s.url||'';
  document.getElementById('description').value=s.description||'';
  document.getElementById('genre').value=s.genre||'';
  document.getElementById('volume').value=s.volume||1.0;
  document.getElementById('aliases').value=(s.aliases||[]).join(', ');
  document.getElementById('cancelBtn').classList.remove('d-none');
  document.getElementById('formTitle').scrollIntoView({behavior:'smooth',block:'start'});
}
function resetForm(){
  editingKey=null;document.getElementById('stationForm').reset();
  document.getElementById('key').readOnly=false;
  document.getElementById('formTitle').textContent='➕ Добавить станцию';
  document.getElementById('cancelBtn').classList.add('d-none');
}
document.getElementById('cancelBtn').onclick=resetForm;
document.getElementById('stationForm').onsubmit=async e=>{
  e.preventDefault();
  const body={key:document.getElementById('key').value.trim(),
    name:document.getElementById('name').value.trim(),
    url:document.getElementById('url').value.trim(),
    description:document.getElementById('description').value.trim(),
    genre:document.getElementById('genre').value.trim(),
    volume:parseFloat(document.getElementById('volume').value)||1.0,
    aliases:document.getElementById('aliases').value.split(',').map(x=>x.trim()).filter(Boolean)};
  const url=editingKey?('/api/stations?key='+encodeURIComponent(editingKey)):'/api/stations';
  const r=await fetch(url,{method:editingKey?'PUT':'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Станция сохранена':('Ошибка: '+(res.error||r.status)),r.ok);
  if(r.ok){resetForm();load();}
};
document.getElementById('exportBtn').onclick=async()=>{
  const r=await fetch('/api/stations');
  if(!r.ok){msg('Не удалось экспортировать',false);return;}
  const text=await r.text();
  const blob=new Blob([text],{type:'application/json'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob);a.download='radio-stations.json';a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),1000);
  msg('Станции экспортированы');
};
document.getElementById('importBtn').onclick=()=>{
  document.getElementById('importFile').click();
};
document.getElementById('importFile').onchange=async e=>{
  const file=e.target.files[0];
  if(!file)return;
  let data;
  try{data=JSON.parse(await file.text());}
  catch(err){msg('Импорт не удался: неверный JSON-файл',false);e.target.value='';return;}
  if(!data||!Array.isArray(data.stations)||data.stations.length===0){
    msg('Импорт не удался: ожидается непустой массив "stations"',false);e.target.value='';return;}
  if(!confirm('Заменить все текущие станции импортированным списком из '+data.stations.length+' станций?')){
    e.target.value='';return;}
  const r=await fetch('/api/import',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  const res=await r.json().catch(()=>({}));
  if(r.ok){msg('Импортировано станций: '+(res.imported||data.stations.length));load();}
  else{msg('Импорт не удался: '+(res.error||r.status),false);}
  e.target.value='';
};
load();
loadStatus();
setInterval(loadStatus,3000);
</script></body></html>)HTML";

std::vector<std::string> SplitAliases(const std::string& value) {
    std::vector<std::string> aliases;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        std::string alias = value.substr(start, comma - start);
        size_t first = alias.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            size_t last = alias.find_last_not_of(" \t\r\n");
            aliases.push_back(alias.substr(first, last - first + 1));
        }
        if (comma == value.size()) {
            break;
        }
        start = comma + 1;
    }
    return aliases;
}

bool StationFromJson(cJSON* root, std::string* key, RadioStation* station) {
    if (root == nullptr || key == nullptr || station == nullptr || !cJSON_IsObject(root)) {
        return false;
    }
    cJSON* key_item = cJSON_GetObjectItem(root, "key");
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(key_item) || !cJSON_IsString(name) || !cJSON_IsString(url)) {
        return false;
    }
    *key = key_item->valuestring;
    station->name = name->valuestring;
    station->url = url->valuestring;
    if (cJSON* description = cJSON_GetObjectItem(root, "description"); cJSON_IsString(description)) {
        station->description = description->valuestring;
    }
    if (cJSON* genre = cJSON_GetObjectItem(root, "genre"); cJSON_IsString(genre)) {
        station->genre = genre->valuestring;
    }
    if (cJSON* volume = cJSON_GetObjectItem(root, "volume"); cJSON_IsNumber(volume)) {
        station->volume = static_cast<float>(volume->valuedouble);
    }
    // Aliases may arrive as a JSON array (Web UI) or a comma separated string.
    if (cJSON* aliases = cJSON_GetObjectItem(root, "aliases")) {
        if (cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    station->aliases.emplace_back(alias->valuestring);
                }
            }
        } else if (cJSON_IsString(aliases)) {
            station->aliases = SplitAliases(aliases->valuestring);
        }
    }
    return true;
}

std::string ErrorJson(const std::string& message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddStringToObject(root.get(), "error", message.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

}  // namespace

RadioWebServer* RadioWebServer::instance_ = nullptr;

RadioWebServer::RadioWebServer() { instance_ = this; }

RadioWebServer::~RadioWebServer() {
    Stop();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool RadioWebServer::Start(Esp32Radio* radio, int port) {
    if (server_handle_ != nullptr) {
        return true;
    }
    if (radio == nullptr) {
        ESP_LOGE(TAG, "Radio Web UI needs a radio instance");
        return false;
    }
    radio_ = radio;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.ctrl_port = 32770;
    // We register 9 handlers; the default limit (8) is not enough.
    config.max_uri_handlers = 16;

    httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = PageHandler,
        .user_ctx = nullptr,
    };
    httpd_uri_t stations_uri = {
        .uri = "/api/stations",
        .method = HTTP_GET,
        .handler = StationsHandler,
        .user_ctx = nullptr,
    };
    // Same handler serves POST/PUT/DELETE; registered once per method.
    httpd_uri_t stations_post_uri = stations_uri;
    stations_post_uri.method = HTTP_POST;
    httpd_uri_t stations_put_uri = stations_uri;
    stations_put_uri.method = HTTP_PUT;
    httpd_uri_t stations_delete_uri = stations_uri;
    stations_delete_uri.method = HTTP_DELETE;
    httpd_uri_t import_uri = {
        .uri = "/api/import",
        .method = HTTP_POST,
        .handler = ImportHandler,
        .user_ctx = nullptr,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = StatusHandler,
        .user_ctx = nullptr,
    };
    httpd_uri_t play_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = PlayHandler,
        .user_ctx = nullptr,
    };
    httpd_uri_t stop_uri = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = StopHandler,
        .user_ctx = nullptr,
    };

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start radio Web UI on port %d", port);
        server_handle_ = nullptr;
        return false;
    }
    auto register_uri = [this](const httpd_uri_t* uri) {
        if (httpd_register_uri_handler(server_handle_, uri) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler: %s", uri->uri);
        }
    };
    register_uri(&page_uri);
    register_uri(&stations_uri);
    register_uri(&stations_post_uri);
    register_uri(&stations_put_uri);
    register_uri(&stations_delete_uri);
    register_uri(&import_uri);
    register_uri(&status_uri);
    register_uri(&play_uri);
    register_uri(&stop_uri);
    ESP_LOGI(TAG, "Radio Web UI started on port %d (http://<device-ip>:%d/), build %s %s", port, port,
             __DATE__, __TIME__);
    return true;
}

void RadioWebServer::Stop() {
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "Radio Web UI stopped");
    }
}

esp_err_t RadioWebServer::PageHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandlePage(req);
}

esp_err_t RadioWebServer::StationsHandler(httpd_req_t* req) {
    if (instance_ == nullptr || instance_->radio_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleStations(req);
}

esp_err_t RadioWebServer::ImportHandler(httpd_req_t* req) {
    if (instance_ == nullptr || instance_->radio_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleImport(req);
}

esp_err_t RadioWebServer::StatusHandler(httpd_req_t* req) {
    if (instance_ == nullptr || instance_->radio_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleStatus(req);
}

esp_err_t RadioWebServer::PlayHandler(httpd_req_t* req) {
    if (instance_ == nullptr || instance_->radio_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandlePlay(req);
}

esp_err_t RadioWebServer::StopHandler(httpd_req_t* req) {
    if (instance_ == nullptr || instance_->radio_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleStop(req);
}

esp_err_t RadioWebServer::HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RadioWebServer::SendJson(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200   ? "200 OK"
                                         : status == 201 ? "201 Created"
                                         : status == 400 ? "400 Bad Request"
                                         : status == 404 ? "404 Not Found"
                                         : status == 409 ? "409 Conflict"
                                         : status == 500 ? "500 Internal Server Error"
                                                         : "500 Internal Server Error");
    return httpd_resp_send(req, body.c_str(), body.size());
}

std::string RadioWebServer::StationsJson() const {
    auto stations = radio_->GetStations();
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& [key, station] : stations) {
        CJsonUniquePtr item(cJSON_CreateObject());
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (item == nullptr || aliases == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "key", key.c_str());
        cJSON_AddStringToObject(item.get(), "name", station.name.c_str());
        cJSON_AddStringToObject(item.get(), "url", station.url.c_str());
        cJSON_AddStringToObject(item.get(), "description", station.description.c_str());
        cJSON_AddStringToObject(item.get(), "genre", station.genre.c_str());
        cJSON_AddNumberToObject(item.get(), "volume", station.volume);
        for (const auto& alias : station.aliases) {
            cJSON_AddItemToArray(aliases.get(), cJSON_CreateString(alias.c_str()));
        }
        cJSON_AddItemToObject(item.get(), "aliases", aliases.release());
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "stations", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t RadioWebServer::HandleStations(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return SendJson(req, 200, StationsJson());
    }

    if (req->method == HTTP_DELETE) {
        char key[64] = {0};
        // httpd_query_key_value wants the query string itself ("key=..."),
        // not the full URI with the path in front of it.
        const char* query = strchr(req->uri, '?');
        const char* query_str = (query != nullptr) ? query + 1 : "";
        if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK || key[0] == '\0') {
            return SendJson(req, 400, ErrorJson("Missing key query parameter"));
        }
        ESP_LOGI(TAG, "Delete station request: key='%s'", key);
        // Distinguish "no such station" from a failed NVS save for the user.
        if (radio_->GetStations().find(key) == radio_->GetStations().end()) {
            ESP_LOGW(TAG, "Delete station: key not present in station list");
            return SendJson(req, 404, ErrorJson("Station not found"));
        }
        if (!radio_->DeleteStation(key)) {
            ESP_LOGE(TAG, "Delete station: failed to persist station list to NVS");
            return SendJson(req, 500, ErrorJson("Failed to save station list"));
        }
        return SendJson(req, 200, "{\"ok\":true}");
    }

    // POST (add) and PUT (edit) both carry the station as a JSON body.
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodySize)) {
        return SendJson(req, 400, ErrorJson("Invalid request body size"));
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            return SendJson(req, 400, ErrorJson("Failed to read request body"));
        }
        received += static_cast<size_t>(ret);
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    std::string key;
    RadioStation station;
    if (!StationFromJson(root.get(), &key, &station)) {
        return SendJson(req, 400, ErrorJson("Expected JSON with key, name and url"));
    }

    if (req->method == HTTP_PUT) {
        char query_key[64] = {0};
        const char* query = strchr(req->uri, '?');
        const char* query_str = (query != nullptr) ? query + 1 : "";
        if (httpd_query_key_value(query_str, "key", query_key, sizeof(query_key)) == ESP_OK &&
            query_key[0] != '\0') {
            key = query_key;
        }
        if (!radio_->UpdateStation(key, station)) {
            return SendJson(req, 404, ErrorJson("Station not found or invalid data"));
        }
        return SendJson(req, 200, "{\"ok\":true}");
    }

    if (req->method == HTTP_POST) {
        if (!radio_->AddStation(key, station)) {
            return SendJson(req, 409, ErrorJson("Station key already exists or invalid data"));
        }
        return SendJson(req, 201, "{\"ok\":true}");
    }

    return SendJson(req, 400, ErrorJson("Unsupported method"));
}

esp_err_t RadioWebServer::HandleImport(httpd_req_t* req) {
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodySize)) {
        return SendJson(req, 400, ErrorJson("Invalid request body size"));
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            return SendJson(req, 400, ErrorJson("Failed to read request body"));
        }
        received += static_cast<size_t>(ret);
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* stations = root != nullptr ? cJSON_GetObjectItem(root.get(), "stations") : nullptr;
    if (!cJSON_IsArray(stations) || cJSON_GetArraySize(stations) == 0) {
        return SendJson(req, 400, ErrorJson("Expected a non-empty \"stations\" array"));
    }

    std::map<std::string, RadioStation> imported;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, stations) {
        std::string key;
        RadioStation station;
        if (!StationFromJson(item, &key, &station)) {
            return SendJson(req, 400, ErrorJson("Invalid station entry: key, name and url are required"));
        }
        if (!imported.emplace(key, station).second) {
            return SendJson(req, 400, ErrorJson("Duplicate station key in import: " + key));
        }
    }

    ESP_LOGI(TAG, "Import station request: %d stations", static_cast<int>(imported.size()));
    if (!radio_->ReplaceStations(imported)) {
        ESP_LOGE(TAG, "Import station request: failed to persist station list to NVS");
        return SendJson(req, 500, ErrorJson("Failed to save imported station list"));
    }
    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return SendJson(req, 500, ErrorJson("Out of memory"));
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddNumberToObject(result.get(), "imported", static_cast<double>(imported.size()));
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t RadioWebServer::HandleStatus(httpd_req_t* req) {
    bool playing = radio_->IsPlaying() || radio_->IsDownloading();
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return SendJson(req, 500, ErrorJson("Out of memory"));
    }
    cJSON_AddBoolToObject(root.get(), "playing", playing);
    cJSON_AddStringToObject(root.get(), "currentKey", playing ? radio_->GetCurrentStationKey().c_str() : "");
    cJSON_AddStringToObject(root.get(), "currentStation",
                            playing ? radio_->GetCurrentStation().c_str() : "");
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t RadioWebServer::HandlePlay(httpd_req_t* req) {
    char key[64] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK || key[0] == '\0') {
        return SendJson(req, 400, ErrorJson("Missing key query parameter"));
    }
    ESP_LOGI(TAG, "Play station request: key='%s'", key);
    if (!radio_->PlayStationByKey(key)) {
        return SendJson(req, 404, ErrorJson("Station not found or failed to start playback"));
    }
    // Starting playback from the Web UI counts as user activity: wake the
    // display if it went to sleep.
    Board::GetInstance().WakeUp();
    return SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t RadioWebServer::HandleStop(httpd_req_t* req) {
    ESP_LOGI(TAG, "Stop playback request");
    if (!radio_->Stop()) {
        return SendJson(req, 500, ErrorJson("Failed to stop playback"));
    }
    return SendJson(req, 200, "{\"ok\":true}");
}
