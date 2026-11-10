#include "web_server.h"

#include "application.h"
#include "board.h"
#include "cjson_utils.h"
#include "features/currency/currency_converter.h"
#include "features/joke/joke_reader.h"
#include "features/radio/radio_web_api.h"
#include "features/rss/rss_reader.h"
#include "features/timer/countdown_timer.h"
#include "features/wol/wol_web_api.h"
#include "mcp_server.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TAG "WebServer"

namespace {

const char kPageHtml[] = "<!-- device web ui build " __DATE__ " " __TIME__ " -->\n" R"HTML(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XiaoZhi — устройство</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
<style>
body{background:#f1f4f8}
.app-header{background:linear-gradient(135deg,#111827,#1f3a5f);}
.app-header .badge{background:#f59e0b;color:#111827}
.card{border:0;box-shadow:0 .125rem .5rem rgba(15,23,42,.08);border-radius:.75rem}
.card-header{background:#fff;border-bottom:1px solid #e5e7eb;font-weight:600}
/* Add/edit forms start collapsed; the header itself is the toggle */
.card-header.collapsible{cursor:pointer;user-select:none;display:flex;align-items:center;justify-content:space-between;gap:.5rem}
.card-header.collapsible:hover{background:#f8fafc}
.card-header.collapsible .chevron{color:#64748b;transition:transform .15s}
.card-header.collapsible[aria-expanded="true"] .chevron{transform:rotate(90deg)}
.card-header.collapsible:focus-visible{outline:2px solid #1f3a5f;outline-offset:-2px}
.url{font-size:.82rem;word-break:break-all}
.actions{white-space:nowrap;text-align:right}
.nav-tabs .nav-link{color:#1f3a5f;font-weight:600}
.nav-tabs .nav-link.active{color:#111827}
.timer-value{font-variant-numeric:tabular-nums;font-size:3.5rem;font-weight:700;line-height:1;letter-spacing:.03em}
.timer-idle{color:#94a3b8}
/* Floating notification: sits under the header, fades in, hides itself */
#msg{position:fixed;top:.75rem;left:50%;z-index:1080;margin:0;width:max-content;max-width:min(92vw,34rem);
  opacity:0;transform:translateX(-50%) translateY(-.75rem);transition:opacity .18s,transform .18s;
  box-shadow:0 .5rem 1.5rem rgba(15,23,42,.22);pointer-events:none}
#msg.toast-show{opacity:1;transform:translateX(-50%) translateY(0);pointer-events:auto;cursor:pointer}
</style></head>
<body>
<header class="app-header text-white shadow-sm">
 <div class="container py-3 d-flex align-items-center gap-3">
  <span class="fs-3">🎛</span>
  <div>
   <h1 class="h4 mb-0">XiaoZhi</h1>
   <div class="small text-white-50">Веб-интерфейс устройства</div>
  </div>
  <div class="ms-auto d-flex align-items-center gap-2">
   <span id="nowPlaying" class="badge text-bg-success d-none">▶</span>
  </div>
 </div>
</header>
<main class="container py-4">
<div id="msg" class="alert d-none" role="alert"></div>

<ul class="nav nav-tabs mb-3" id="mainTabs" role="tablist">
 <li class="nav-item" role="presentation"><button class="nav-link active" type="button" data-tab="radio">📻 Радио</button></li>
 <li class="nav-item" role="presentation"><button class="nav-link" type="button" data-tab="news">📰 Новости</button></li>
 <li class="nav-item" role="presentation"><button class="nav-link" type="button" data-tab="timer">⏱ Таймер</button></li>
 <li class="nav-item" role="presentation"><button class="nav-link" type="button" data-tab="skills">🧠 Навыки</button></li>
 <li class="nav-item" role="presentation"><button class="nav-link" type="button" data-tab="wol">🌐 Wake-on-LAN</button></li>
 <li class="nav-item" role="presentation"><button class="nav-link" type="button" data-tab="system">⚙ Система</button></li>
</ul>
<div class="tab-content">
<div class="tab-pane active" id="pane-radio" role="tabpanel">

<section class="card mb-4">
 <div class="card-header collapsible" role="button" tabindex="0" aria-expanded="false" aria-controls="stationFormBody">
  <span id="formTitle">➕ Добавить станцию</span><span class="chevron">▸</span>
 </div>
 <div class="card-body d-none" id="stationFormBody">
  <form id="stationForm">
   <div class="row g-3">
    <div class="col-md-6">
     <label class="form-label" for="name">Название</label>
     <input id="name" class="form-control" required autocomplete="off" maxlength="64">
    </div>
    <div class="col-md-3">
     <label class="form-label" for="genre">Жанр</label>
     <input id="genre" class="form-control" autocomplete="off" maxlength="64">
    </div>
    <div class="col-md-3">
     <label class="form-label" for="volume">Громкость <span class="text-muted">(0.1–4.0)</span></label>
     <input id="volume" class="form-control" type="number" step="0.1" min="0.1" max="4" value="1.0">
    </div>
    <div class="col-12">
     <label class="form-label" for="url">Stream URL <span class="text-muted">(http/https)</span></label>
     <input id="url" class="form-control" type="url" required autocomplete="off" maxlength="256">
    </div>
    <div class="col-12">
     <label class="form-label" for="aliases">Ключевые слова — дополнительные алиасы <span class="text-muted">(через запятую, до 8 штук, каждый до 32 байт)</span></label>
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
<div class="card-header">📡 Станции</div>
<div class="table-responsive">
<table class="table table-hover align-middle mb-0">
<thead class="table-dark"><tr><th>Название</th><th>URL</th><th>Алиасы</th><th class="text-end">Действия</th></tr></thead>
<tbody id="rows"></tbody>
</table>
</div>
</section>

<section class="card">
 <div class="card-header">⇅ Импорт / Экспорт</div>
 <div class="card-body d-flex flex-wrap gap-2 align-items-center">
  <button class="btn btn-outline-success" type="button" id="exportBtn">⬇ Экспорт станций (JSON)</button>
  <button class="btn btn-outline-warning" type="button" id="importBtn">⬆ Импорт и замена всех станций</button>
  <input type="file" id="importFile" accept=".json,application/json" class="d-none">
  <span class="text-muted small">Импорт полностью заменяет текущий список станций.</span>
 </div>
</section>
</div>

<div class="tab-pane" id="pane-timer" role="tabpanel">
<section class="card mb-4">
 <div class="card-header">⏱ Таймер обратного отсчёта</div>
 <div class="card-body">
  <div class="text-center mb-3">
   <div class="timer-value timer-idle" id="timerValue">--:--</div>
   <div class="small text-muted mt-2" id="timerState">Таймер не запущен</div>
  </div>
  <div class="d-flex flex-wrap gap-2 justify-content-center mb-4" id="timerPresets">
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="60">1 мин</button>
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="300">5 мин</button>
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="600">10 мин</button>
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="900">15 мин</button>
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="1800">30 мин</button>
   <button class="btn btn-outline-primary btn-sm" type="button" data-seconds="3600">1 час</button>
  </div>
  <form id="timerForm" class="row g-2 align-items-end justify-content-center">
   <div class="col-auto">
    <label class="form-label small" for="timerMin">Минуты</label>
    <input id="timerMin" class="form-control" type="number" min="0" max="1440" step="1" value="5" style="width:7rem">
   </div>
   <div class="col-auto">
    <label class="form-label small" for="timerSec">Секунды</label>
    <input id="timerSec" class="form-control" type="number" min="0" max="59" step="1" value="0" style="width:6rem">
   </div>
   <div class="col-auto">
    <button class="btn btn-primary" type="submit">▶ Запустить</button>
   </div>
   <div class="col-auto">
    <button class="btn btn-outline-danger" type="button" id="timerStop" disabled>⏹ Остановить</button>
   </div>
  </form>
  <div class="text-muted small text-center mt-4">Таймер идёт независимо от радио и разговора с ассистентом; по окончании звучит сигнал.</div>
 </div>
</section>
</div>

<div class="tab-pane" id="pane-news" role="tabpanel">

<section class="card mb-4">
 <div class="card-header collapsible" role="button" tabindex="0" aria-expanded="false" aria-controls="feedFormBody">
  <span id="feedFormTitle">➕ Добавить RSS-ленту</span><span class="chevron">▸</span>
 </div>
 <div class="card-body d-none" id="feedFormBody">
  <form id="feedForm">
   <div class="row g-3">
    <div class="col-md-12">
     <label class="form-label" for="feedName">Название <span class="text-muted">(как называть голосом)</span></label>
     <input id="feedName" class="form-control" required autocomplete="off" placeholder="Лента" maxlength="64">
    </div>
    <div class="col-12">
     <label class="form-label" for="feedUrl">Адрес ленты <span class="text-muted">(RSS 2.0 или Atom, http/https)</span></label>
     <input id="feedUrl" class="form-control" type="url" required autocomplete="off" placeholder="https://lenta.ru/rss/news" maxlength="256">
    </div>
    <div class="col-md-4">
     <label class="form-label" for="feedCategory">Категория <span class="text-muted">(необязательно)</span></label>
     <input id="feedCategory" class="form-control" autocomplete="off" placeholder="новости" maxlength="64">
    </div>
    <div class="col-md-8">
     <label class="form-label" for="feedAliases">Ключевые слова — дополнительные алиасы <span class="text-muted">(через запятую, до 8 штук, каждый до 32 байт)</span></label>
     <input id="feedAliases" class="form-control" autocomplete="off" placeholder="лента, новости lenta">
    </div>
    <div class="col-12 d-flex gap-2">
     <button class="btn btn-primary" type="submit">💾 Сохранить ленту</button>
     <button class="btn btn-outline-secondary d-none" type="button" id="feedCancelBtn">Отменить редактирование</button>
    </div>
   </div>
  </form>
  <div class="text-muted small mt-3">Голосом можно попросить «прочитай 5 последних новостей».</div>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">📡 Ленты</div>
 <div class="table-responsive">
  <table class="table table-hover align-middle mb-0">
   <thead class="table-dark"><tr><th>Название</th><th>URL</th><th>Алиасы</th><th class="text-end">Действия</th></tr></thead>
   <tbody id="feedRows"></tbody>
  </table>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">⇅ Импорт / Экспорт</div>
 <div class="card-body d-flex flex-wrap gap-2 align-items-center">
  <button class="btn btn-outline-success" type="button" id="feedExportBtn">⬇ Экспорт лент (JSON)</button>
  <button class="btn btn-outline-warning" type="button" id="feedImportBtn">⬆ Импорт и замена всех лент</button>
  <input type="file" id="feedImportFile" accept=".json,application/json" class="d-none">
  <span class="text-muted small">Импорт полностью заменяет текущий список лент.</span>
 </div>
</section>
</div>

<div class="tab-pane" id="pane-skills" role="tabpanel">

<section class="card mb-4">
 <div class="card-header">🧠 Навыки</div>
 <div class="card-body">
  <p class="text-muted small">Отмеченные навыки только читают информацию: после них устройство продолжает слушать и можно сразу задать следующий вопрос. Неотмеченные навыки завершают голосовую сессию — устройство отвечает и уходит в ожидание.</p>
  <div class="table-responsive">
   <table class="table table-hover align-middle mb-0">
    <thead class="table-dark"><tr><th style="width:3rem">✔</th><th>Навык</th><th>Описание</th></tr></thead>
    <tbody id="toolRows"></tbody>
   </table>
  </div>
  <div class="d-flex flex-wrap gap-2 align-items-center mt-3">
   <button class="btn btn-primary" type="button" id="toolsSaveBtn">💾 Сохранить</button>
   <button class="btn btn-outline-secondary" type="button" id="toolsReloadBtn">↻ Обновить</button>
   <span class="text-muted small">Список сохраняется в памяти устройства и применяется сразу, перезагрузка не нужна.</span>
  </div>
 </div>
</section>
</div>


<div class="tab-pane" id="pane-system" role="tabpanel">

<section class="card mb-4">
 <div class="card-header d-flex justify-content-between align-items-center">
  <span>📊 Статус</span>
  <button class="btn btn-sm btn-outline-secondary" type="button" id="statusRefreshBtn">Обновить</button>
 </div>
 <div class="card-body">
  <dl class="row mb-0 small">
   <dt class="col-5 col-sm-3">Прошивка</dt><dd class="col-7 col-sm-9" id="statusVersion">—</dd>
   <dt class="col-5 col-sm-3">Время работы</dt><dd class="col-7 col-sm-9" id="statusUptime">—</dd>
   <dt class="col-5 col-sm-3">Wi-Fi</dt><dd class="col-7 col-sm-9" id="statusWifi">—</dd>
   <dt class="col-5 col-sm-3">NVS</dt>
   <dd class="col-7 col-sm-9">
    <div class="progress" style="height:1.25rem"><div class="progress-bar bg-success" id="statusNvsBar" role="progressbar" style="width:0%">—</div></div>
    <div class="small text-muted" id="statusNvsText">—</div>
   </dd>
  </dl>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">🔊 Громкость</div>
 <div class="card-body">
  <div class="d-flex align-items-center gap-3">
   <span id="volumeValue" class="badge text-bg-secondary d-inline-flex justify-content-center align-items-center"
         style="width: 3rem; min-width: 3rem; flex-shrink: 0;">70</span>
   <input id="volumeSlider" type="range" class="form-range mb-0 flex-grow-1"
          min="0" max="100" step="1" value="70">
  </div>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">🩺 Доступность сервисов</div>
 <div class="card-body">
  <div class="d-flex flex-wrap gap-2">
   <button class="btn btn-outline-primary" type="button" id="checkJokeBtn">😂 Сервис анекдотов</button>
   <button class="btn btn-outline-primary" type="button" id="checkCurrencyBtn">💱 Курс валют</button>
  </div>
  <div class="text-muted small mt-3">Кнопка запрашивает сервис с устройства и показывает, отвечает ли он.</div>
 </div>
</section>

<section class="card">
 <div class="card-header">⚙ Устройство</div>
 <div class="card-body d-flex flex-wrap gap-2 align-items-center">
  <button class="btn btn-outline-danger" type="button" id="rebootBtn">🔄 Перезагрузить</button>
  <span class="text-muted small">Настройки в памяти устройства и в NVS сохраняются.</span>
 </div>
</section>
</div>

<div class="tab-pane" id="pane-wol" role="tabpanel">

<section class="card mb-4">
 <div class="card-header collapsible" role="button" tabindex="0" aria-expanded="false" aria-controls="wolFormBody">
  <span id="wolFormTitle">➕ Добавить компьютер</span><span class="chevron">▸</span>
 </div>
 <div class="card-body d-none" id="wolFormBody">
  <form id="wolForm">
   <div class="row g-3">
    <div class="col-md-3">
     <label class="form-label" for="wolName">Название</label>
     <input id="wolName" class="form-control" type="text" placeholder="Рабочий ПК" required autocomplete="off" maxlength="64">
    </div>
    <div class="col-md-3">
     <label class="form-label" for="wolMac">MAC-адрес</label>
     <input id="wolMac" class="form-control" type="text" placeholder="AA:BB:CC:DD:EE:FF" required autocomplete="off">
    </div>
    <div class="col-md-4">
     <label class="form-label" for="wolBroadcast">Широковещательный адрес</label>
     <input id="wolBroadcast" class="form-control" type="text" value="255.255.255.255" autocomplete="off">
    </div>
    <div class="col-md-2">
     <label class="form-label" for="wolPort">Порт</label>
     <input id="wolPort" class="form-control" type="number" min="1" max="65535" value="9">
    </div>
    <div class="col-12 d-flex gap-2">
     <button class="btn btn-primary" type="submit">💾 Сохранить ПК</button>
     <button class="btn btn-outline-secondary d-none" type="button" id="wolCancelBtn">Отменить редактирование</button>
    </div>
   </div>
  </form>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">🌐 Компьютеры</div>
 <div class="table-responsive">
  <table class="table table-hover align-middle mb-0">
   <thead class="table-dark"><tr><th>Название</th><th>MAC-адрес</th><th>Широковещательный адрес</th><th style="width:5rem">Порт</th><th class="actions">Действия</th></tr></thead>
   <tbody id="wolRows"></tbody>
  </table>
 </div>
</section>

<section class="card mb-4">
 <div class="card-header">⇅ Импорт / Экспорт</div>
 <div class="card-body d-flex flex-wrap gap-2 align-items-center">
  <button class="btn btn-outline-success" type="button" id="wolExportBtn">⬇ Экспорт компьютеров (JSON)</button>
  <button class="btn btn-outline-warning" type="button" id="wolImportBtn">⬆ Импорт и замена всех компьютеров</button>
  <input type="file" id="wolImportFile" accept=".json,application/json" class="d-none">
  <span class="text-muted small">Импорт полностью заменяет текущий список компьютеров.</span>
 </div>
</section>
</div>
</div>

<footer class="text-center text-muted small py-4">XiaoZhi · локальный веб-интерфейс устройства</footer>
</main>
<script>
let editingKey=null;
let stationsCache=[];
let playback={playing:false,currentKey:'',currentStation:''};
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
let msgTimer=null;
function hideMsg(){
  clearTimeout(msgTimer);msgTimer=null;
  const el=document.getElementById('msg');
  el.className='alert d-none';
}
function msg(t,ok){
  const el=document.getElementById('msg');
  el.textContent=t;
  el.className='alert toast-show '+(ok===false?'alert-danger':'alert-success');
  clearTimeout(msgTimer);
  // Errors stay a bit longer, they usually carry a reason to read.
  msgTimer=setTimeout(hideMsg,ok===false?6000:3500);
}
document.getElementById('msg').addEventListener('click',hideMsg);
// Add/edit form cards stay collapsed until the header is clicked or editing starts.
function setFormOpen(bodyId,open){
  const body=document.getElementById(bodyId);
  if(!body)return;
  body.classList.toggle('d-none',!open);
  document.querySelectorAll('.card-header.collapsible[aria-controls="'+bodyId+'"]')
    .forEach(h=>h.setAttribute('aria-expanded',open?'true':'false'));
}
document.querySelectorAll('.card-header.collapsible').forEach(header=>{
  const bodyId=header.getAttribute('aria-controls');
  const toggle=()=>{
    const body=document.getElementById(bodyId);
    if(body)setFormOpen(bodyId,body.classList.contains('d-none'));
  };
  header.addEventListener('click',toggle);
  header.addEventListener('keydown',e=>{
    if(e.key==='Enter'||e.key===' '){e.preventDefault();toggle();}
  });
});
// The firmware limits are in bytes, while HTML maxlength counts characters:
// one Cyrillic letter costs two bytes, so a 64-byte field fits only 32 of them.
const utf8Bytes=s=>new TextEncoder().encode(String(s==null?'':s)).length;
function clampBytes(s,max){
  const chars=Array.from(String(s==null?'':s));
  let total=0;
  for(let i=0;i<chars.length;i++){
    total+=utf8Bytes(chars[i]);
    if(total>max)return chars.slice(0,i).join('');
  }
  return chars.join('');
}
function capBytes(id,max){
  const el=document.getElementById(id);
  if(!el)return;
  el.addEventListener('input',()=>{const v=clampBytes(el.value,max);if(v!==el.value)el.value=v;});
}
capBytes('name',64);capBytes('genre',64);capBytes('url',256);
capBytes('feedName',64);capBytes('feedCategory',64);capBytes('feedUrl',256);
capBytes('wolName',64);
// Aliases are stored one by one, so the limit applies per item, not to the line.
function aliasError(aliases){
  if(aliases.length>8)return 'Слишком много алиасов: не больше 8';
  const long=aliases.find(a=>utf8Bytes(a)>32);
  if(long)return 'Алиас «'+long+'» длиннее 32 байт';
  return '';
}
function isCurrent(s){return playback.playing&&playback.currentKey===s.key}
function renderRows(){
  const stations=stationsCache;
  const tb=document.getElementById('rows');tb.innerHTML='';
  if(stations.length===0){
    tb.innerHTML='<tr><td colspan="4" class="text-center text-muted py-4">Станций пока нет. Добавьте первую через форму выше или импортируйте список.</td></tr>';
    return;}
  stations.forEach(s=>{
    const current=isCurrent(s);
    const tr=document.createElement('tr');
    if(current)tr.className='table-success';
    const genre=s.genre?' <span class="badge text-bg-secondary">'+esc(s.genre)+'</span>':'';
    const playingBadge=current?' <span class="badge text-bg-success">▶ играет</span>':'';
    tr.innerHTML='<td><strong>'+esc(s.name)+'</strong>'+genre+playingBadge+'</td>'
      +'<td class="url">'+esc(s.url)+'</td>'
      +'<td>'+esc((s.aliases||[]).join(', '))+'</td>';
    const td=document.createElement('td');td.className='actions';
    const pb=document.createElement('button');
    if(current){pb.className='btn btn-sm btn-danger me-1';pb.textContent='⏹';pb.onclick=()=>stopPlayback(pb);}
    else{pb.className='btn btn-sm btn-success me-1';pb.textContent='▶';pb.onclick=()=>playStation(s.key,pb);}
    const eb=document.createElement('button');eb.className='btn btn-sm btn-outline-primary me-1';eb.textContent='✏️';
    eb.onclick=()=>startEdit(s);
    const db=document.createElement('button');db.className='btn btn-sm btn-outline-danger';db.textContent='🗑';
    db.onclick=async()=>{if(!confirm('Удалить станцию «'+s.name+'»?'))return;
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
  document.getElementById('formTitle').textContent='✏️ Редактирование станции «'+s.name+'»';
  document.getElementById('name').value=s.name||'';
  document.getElementById('url').value=s.url||'';
  document.getElementById('genre').value=s.genre||'';
  document.getElementById('volume').value=s.volume||1.0;
  document.getElementById('aliases').value=(s.aliases||[]).join(', ');
  document.getElementById('cancelBtn').classList.remove('d-none');
  setFormOpen('stationFormBody',true);
  document.getElementById('formTitle').scrollIntoView({behavior:'smooth',block:'start'});
}
function resetForm(){
  editingKey=null;document.getElementById('stationForm').reset();
  document.getElementById('formTitle').textContent='➕ Добавить станцию';
  document.getElementById('cancelBtn').classList.add('d-none');
  setFormOpen('stationFormBody',false);
}
document.getElementById('cancelBtn').onclick=resetForm;
document.getElementById('stationForm').onsubmit=async e=>{
  e.preventDefault();
  const body={name:document.getElementById('name').value.trim(),
    url:document.getElementById('url').value.trim(),
    genre:document.getElementById('genre').value.trim(),
    volume:parseFloat(document.getElementById('volume').value)||1.0,
    aliases:document.getElementById('aliases').value.split(',').map(x=>x.trim()).filter(Boolean)};
  const aliasProblem=aliasError(body.aliases);
  if(aliasProblem){msg(aliasProblem,false);return;}
  const url=editingKey?('/api/stations?key='+encodeURIComponent(editingKey)):'/api/stations';
  const r=await fetch(url,{method:editingKey?'PUT':'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Станция сохранена':('Ошибка: '+(res.error||r.status)),r.ok);
  if(r.ok){resetForm();load();}
};
document.getElementById('exportBtn').onclick=async()=>{
  const r=await fetch('/api/stations');
  if(!r.ok){msg('Не удалось экспортировать',false);return;}
  let text;
  try{text=JSON.stringify(JSON.parse(await r.text()),null,2);}
  catch(err){msg('Не удалось экспортировать',false);return;}
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
let editingFeed=null;
let feedsCache=[];
function renderFeedRows(){
  const tb=document.getElementById('feedRows');tb.innerHTML='';
  if(feedsCache.length===0){
    tb.innerHTML='<tr><td colspan="4" class="text-center text-muted py-4">RSS-лент пока нет. Добавьте первую через форму выше или импортируйте список.</td></tr>';
    return;}
  feedsCache.forEach(f=>{
    const tr=document.createElement('tr');
    const category=f.category?' <span class="badge text-bg-secondary">'+esc(f.category)+'</span>':'';
    tr.innerHTML='<td><strong>'+esc(f.name)+'</strong>'+category+'</td>'
      +'<td class="url">'+esc(f.url)+'</td>'
      +'<td>'+esc((f.aliases||[]).join(', '))+'</td>';
    const td=document.createElement('td');td.className='actions';
    const eb=document.createElement('button');eb.className='btn btn-sm btn-outline-primary me-1';eb.textContent='✏️';
    eb.onclick=()=>startFeedEdit(f);
    const db=document.createElement('button');db.className='btn btn-sm btn-outline-danger';db.textContent='🗑';
    db.onclick=async()=>{if(!confirm('Удалить ленту «'+f.name+'»?'))return;
      const rr=await fetch('/api/rss?key='+encodeURIComponent(f.key),{method:'DELETE'});
      msg(rr.ok?'Лента удалена':'Не удалось удалить ленту',rr.ok);loadRss();};
    td.append(eb,db);tr.appendChild(td);tb.appendChild(tr);});
}
async function loadRss(){
  try{
    const r=await fetch('/api/rss');
    if(!r.ok)return;
    const data=await r.json();
    feedsCache=data.feeds||[];
    renderFeedRows();
  }catch(e){/* device unreachable; keep last state */}
}
function startFeedEdit(f){
  editingFeed=f.key;
  document.getElementById('feedFormTitle').textContent='✏️ Редактирование ленты «'+f.name+'»';
  document.getElementById('feedName').value=f.name||'';
  document.getElementById('feedUrl').value=f.url||'';
  document.getElementById('feedCategory').value=f.category||'';
  document.getElementById('feedAliases').value=(f.aliases||[]).join(', ');
  document.getElementById('feedCancelBtn').classList.remove('d-none');
  setFormOpen('feedFormBody',true);
  document.getElementById('feedFormTitle').scrollIntoView({behavior:'smooth',block:'start'});
}
function resetFeedForm(){
  editingFeed=null;document.getElementById('feedForm').reset();
  document.getElementById('feedFormTitle').textContent='➕ Добавить RSS-ленту';
  document.getElementById('feedCancelBtn').classList.add('d-none');
  setFormOpen('feedFormBody',false);
}
document.getElementById('feedCancelBtn').onclick=resetFeedForm;
document.getElementById('feedForm').onsubmit=async e=>{
  e.preventDefault();
  const body={name:document.getElementById('feedName').value.trim(),
    url:document.getElementById('feedUrl').value.trim(),
    category:document.getElementById('feedCategory').value.trim(),
    aliases:document.getElementById('feedAliases').value.split(',').map(x=>x.trim()).filter(Boolean)};
  const aliasProblem=aliasError(body.aliases);
  if(aliasProblem){msg(aliasProblem,false);return;}
  if(editingFeed)body.key=editingFeed;
  const r=await fetch('/api/rss',{method:editingFeed?'PUT':'POST',
    headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Лента сохранена':('Ошибка: '+(res.error||r.status)),r.ok);
  if(r.ok){resetFeedForm();loadRss();}
};
document.getElementById('feedExportBtn').onclick=async()=>{
  try{
    const r=await fetch('/api/rss');
    if(!r.ok){msg('Не удалось экспортировать ленты',false);return;}
    const data=await r.json();
    const blob=new Blob([JSON.stringify(data,null,2)],{type:'application/json'});
    const a=document.createElement('a');
    a.href=URL.createObjectURL(blob);a.download='rss-feeds.json';a.click();
    setTimeout(()=>URL.revokeObjectURL(a.href),1000);
    msg('Ленты экспортированы');
  }catch(e){msg('Не удалось экспортировать ленты',false);}
};
document.getElementById('feedImportBtn').onclick=()=>{
  document.getElementById('feedImportFile').click();
};
document.getElementById('feedImportFile').onchange=async e=>{
  const file=e.target.files[0];
  if(!file)return;
  let data;
  try{data=JSON.parse(await file.text());}
  catch(err){msg('Импорт не удался: неверный JSON-файл',false);e.target.value='';return;}
  if(!data||!Array.isArray(data.feeds)||data.feeds.length===0){
    msg('Импорт не удался: ожидается непустой массив "feeds"',false);e.target.value='';return;}
  if(!confirm('Заменить все текущие ленты импортированным списком из '+data.feeds.length+' лент?')){
    e.target.value='';return;}
  const r=await fetch('/api/rss',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({feeds:data.feeds})});
  const res=await r.json().catch(()=>({}));
  if(r.ok){msg('Импортировано лент: '+(res.imported||data.feeds.length));resetFeedForm();loadRss();}
  else{msg('Импорт не удался: '+(res.error||r.status),false);}
  e.target.value='';
};
function showTab(name){
  document.querySelectorAll('#mainTabs .nav-link').forEach(b=>b.classList.toggle('active',b.dataset.tab===name));
  document.querySelectorAll('.tab-content > .tab-pane').forEach(p=>p.classList.toggle('active',p.id==='pane-'+name));
  if(name==='timer')loadTimer();
  if(name==='news')loadRss();
  if(name==='skills')loadTools();
  if(name==='system')loadSystem();
  if(name==='wol')loadWol();
}
document.querySelectorAll('#mainTabs .nav-link').forEach(b=>{b.onclick=()=>showTab(b.dataset.tab)});
function paintTimer(t){
  const running=!!t.running;
  const value=document.getElementById('timerValue');
  value.textContent=running?(t.remaining||'--:--'):'--:--';
  value.classList.toggle('timer-idle',!running);
  document.getElementById('timerState').textContent=running
    ?'Таймер идёт — осталось '+t.remainingSeconds+' с'
    :'Таймер не запущен';
  document.getElementById('timerStop').disabled=!running;
}
async function loadTimer(){
  try{
    const r=await fetch('/api/timer');
    if(!r.ok)return;
    paintTimer(await r.json());
  }catch(e){/* device unreachable; keep last state */}
}
async function setTimer(seconds){
  const r=await fetch('/api/timer?seconds='+seconds,{method:'POST'});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'⏱ Таймер запущен на '+res.remaining:'Не удалось запустить таймер: '+(res.error||r.status),r.ok);
  await loadTimer();
}
async function stopTimer(){
  const btn=document.getElementById('timerStop');
  btn.disabled=true;
  const r=await fetch('/api/timer/stop',{method:'POST'});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'⏹ Таймер остановлен':'Не удалось остановить таймер: '+(res.error||r.status),r.ok);
  await loadTimer();
}
document.querySelectorAll('#timerPresets button').forEach(b=>{b.onclick=()=>setTimer(parseInt(b.dataset.seconds,10))});
document.getElementById('timerStop').onclick=stopTimer;
document.getElementById('timerForm').onsubmit=e=>{
  e.preventDefault();
  const minutes=parseInt(document.getElementById('timerMin').value,10)||0;
  const seconds=parseInt(document.getElementById('timerSec').value,10)||0;
  const total=minutes*60+seconds;
  if(total<1||total>86400){msg('Укажите длительность от 1 секунды до 24 часов',false);return;}
  setTimer(total);
};
let toolsCache=[];
function renderToolRows(){
  const tb=document.getElementById('toolRows');tb.innerHTML='';
  if(toolsCache.length===0){
    tb.innerHTML='<tr><td colspan="3" class="text-center text-muted py-4">Навыки не загрузились. Нажмите «Обновить».</td></tr>';
    return;}
  toolsCache.forEach(t=>{
    const tr=document.createElement('tr');
    const td0=document.createElement('td');
    const cb=document.createElement('input');
    cb.type='checkbox';cb.className='form-check-input';cb.checked=!!t.read_only;cb.dataset.name=t.name;
    cb.title='Не завершать голосовую сессию после этого навыка';
    td0.appendChild(cb);
    const tdName=document.createElement('td');
    tdName.innerHTML='<code>'+esc(t.name)+'</code>';
    const tdDesc=document.createElement('td');tdDesc.className='small text-muted';
    const desc=t.description||'';
    tdDesc.textContent=desc.length>180?desc.slice(0,180)+'…':desc;
    tdDesc.title=desc;
    tr.append(td0,tdName,tdDesc);
    tb.appendChild(tr);});
}
async function loadTools(){
  try{
    const r=await fetch('/api/skills');
    if(!r.ok){msg('Не удалось получить список навыков',false);return;}
    const data=await r.json();
    toolsCache=data.tools||[];
    renderToolRows();
  }catch(e){msg('Не удалось получить список навыков: устройство недоступно',false);}
}
async function saveTools(){
  const btn=document.getElementById('toolsSaveBtn');
  btn.disabled=true;
  const read_only=[...document.querySelectorAll('#toolRows input[type=checkbox]')]
    .filter(cb=>cb.checked).map(cb=>cb.dataset.name);
  const r=await fetch('/api/skills',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({read_only:read_only})});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Настройки навыков сохранены':'Не удалось сохранить: '+(res.error||r.status),r.ok);
  btn.disabled=false;
  if(r.ok)await loadTools();
}
document.getElementById('toolsSaveBtn').onclick=saveTools;
document.getElementById('toolsReloadBtn').onclick=loadTools;
async function loadSystem(){
  try{
    const r=await fetch('/api/system');
    if(!r.ok)return;
    const data=await r.json();
    const slider=document.getElementById('volumeSlider');
    if(typeof data.volume==='number')slider.value=data.volume;
    document.getElementById('volumeValue').textContent=slider.value;
    renderStatus(data);
  }catch(e){/* device unreachable; keep last state */}
}
function fmtUptime(seconds){
  const total=Math.max(0,Math.floor(seconds||0));
  const days=Math.floor(total/86400),hours=Math.floor(total%86400/3600);
  const minutes=Math.floor(total%3600/60),secs=total%60;
  if(days>0)return days+' д '+hours+' ч '+minutes+' мин';
  if(hours>0)return hours+' ч '+minutes+' мин '+secs+' с';
  return minutes+' мин '+secs+' с';
}
function signalLabel(rssi){
  if(rssi>=-60)return 'отличный';
  if(rssi>=-70)return 'хороший';
  if(rssi>=-80)return 'слабый';
  return 'очень слабый';
}
function setStatusText(id,text){
  const el=document.getElementById(id);
  if(el)el.textContent=text;
}
function renderStatus(data){
  const status=data.status||{};
  setStatusText('statusVersion',status.version||'—');
  setStatusText('statusUptime',typeof status.uptimeSeconds==='number'?fmtUptime(status.uptimeSeconds):'—');
  const device=data.device||{};
  const network=device.network||{};
  let wifi='—';
  if(network.ssid||typeof network.rssi==='number'){
    wifi=network.ssid||'без имени';
    if(typeof network.rssi==='number')wifi+=' · '+network.rssi+' dBm · '+signalLabel(network.rssi);
  }
  setStatusText('statusWifi',wifi);
  const nvs=status.nvs||{};
  const nvsBar=document.getElementById('statusNvsBar');
  const nvsText=document.getElementById('statusNvsText');
  if(typeof nvs.used==='number'&&typeof nvs.total==='number'&&nvs.total>0){
    const percent=Math.min(100,Math.round(nvs.used*100/nvs.total));
    nvsBar.style.width=percent+'%';
    nvsBar.className='progress-bar '+(percent>=90?'bg-danger':(percent>=70?'bg-warning text-dark':'bg-success'));
    nvsBar.textContent=percent+'%';
    nvsText.textContent=nvs.used+' из '+nvs.total+' записей занято';
  }else{
    nvsBar.style.width='0%';
    nvsBar.className='progress-bar bg-secondary';
    nvsBar.textContent='—';
    nvsText.textContent='недоступно';
  }
}
async function saveVolume(value){
  const r=await fetch('/api/system',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({volume:value})});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Громкость: '+value:'Не удалось изменить громкость: '+(res.error||r.status),r.ok);
}
async function checkService(service,btn){
  btn.disabled=true;
  try{
    const r=await fetch('/api/system/check?service='+service);
    const data=await r.json().catch(()=>({}));
    msg(r.ok?'Сервис доступен: '+(data.service||service):'Сервис недоступен: '+(data.error||('HTTP '+r.status)),r.ok);
  }catch(e){msg('Не удалось проверить сервис: устройство недоступно',false);}
  finally{btn.disabled=false;}
}
async function rebootDevice(){
  if(!confirm('Перезагрузить устройство?'))return;
  const btn=document.getElementById('rebootBtn');
  btn.disabled=true;
  const r=await fetch('/api/system/reboot',{method:'POST'});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Устройство перезагружается…':'Не удалось перезагрузить: '+(res.error||r.status),r.ok);
}
document.getElementById('volumeSlider').oninput=e=>{
  document.getElementById('volumeValue').textContent=e.target.value;
};
document.getElementById('volumeSlider').onchange=e=>saveVolume(parseInt(e.target.value,10));
document.getElementById('checkJokeBtn').onclick=e=>checkService('joke',e.currentTarget);
document.getElementById('checkCurrencyBtn').onclick=e=>checkService('currency',e.currentTarget);
document.getElementById('rebootBtn').onclick=rebootDevice;
document.getElementById('statusRefreshBtn').onclick=()=>loadSystem();
// Keep uptime and Wi-Fi signal live while the System tab is open.
setInterval(()=>{
  const pane=document.getElementById('pane-system');
  if(pane&&pane.classList.contains('active'))loadSystem();
},5000);
let wolCache=[];
let editingWolKey='';
function renderWolRows(){
  const tb=document.getElementById('wolRows');tb.innerHTML='';
  if(wolCache.length===0){
    tb.innerHTML='<tr><td colspan="5" class="text-center text-muted py-4">Компьютеров пока нет. Добавьте первый через форму выше или импортируйте список.</td></tr>';
    return;}
  wolCache.forEach(c=>{
    const tr=document.createElement('tr');
    tr.innerHTML='<td><strong>'+esc(c.name)+'</strong>'+(c.default?' <span title="По умолчанию">⭐</span>':'')+'</td>'
      +'<td><code>'+esc(c.mac)+'</code></td>'
      +'<td>'+esc(c.broadcast)+'</td>'
      +'<td>'+esc(c.port)+'</td>';
    const td=document.createElement('td');td.className='actions';
    const wake=document.createElement('button');wake.type='button';wake.className='btn btn-sm btn-outline-primary me-1';wake.textContent='⏻';wake.title='Включить';wake.onclick=()=>wakeWol(c.key);
    const star=document.createElement('button');star.type='button';star.className='btn btn-sm me-1 '+(c.default?'btn-warning':'btn-outline-warning');star.textContent='⭐';star.title=c.default?'Снять отметку по умолчанию':'Включать по фразе «включи компьютер»';star.onclick=()=>setDefaultWol(c.key,!c.default);
    const edit=document.createElement('button');edit.type='button';edit.className='btn btn-sm btn-outline-secondary me-1';edit.textContent='✏️';edit.title='Изменить';edit.onclick=()=>editWol(c.key);
    const del=document.createElement('button');del.type='button';del.className='btn btn-sm btn-outline-danger';del.textContent='🗑';del.title='Удалить';del.onclick=()=>deleteWol(c.key);
    td.append(wake,star,edit,del);tr.appendChild(td);tb.appendChild(tr);
  });
}
async function loadWol(){
  try{
    const r=await fetch('/api/wol');
    if(!r.ok){msg('Не удалось получить список компьютеров',false);return;}
    const d=await r.json();
    wolCache=d.computers||[];
    renderWolRows();
  }catch(e){msg('Не удалось получить список компьютеров: устройство недоступно',false);}
}
function resetWolForm(){
  editingWolKey='';
  document.getElementById('wolForm').reset();
  document.getElementById('wolFormTitle').textContent='➕ Добавить компьютер';
  document.getElementById('wolCancelBtn').classList.add('d-none');
  setFormOpen('wolFormBody',false);
}
function editWol(key){
  const c=wolCache.find(x=>x.key===key);
  if(!c)return;
  editingWolKey=key;
  document.getElementById('wolFormTitle').textContent='✏️ Изменение компьютера «'+c.name+'»';
  document.getElementById('wolName').value=c.name||'';
  document.getElementById('wolMac').value=c.mac||'';
  document.getElementById('wolBroadcast').value=c.broadcast||'255.255.255.255';
  document.getElementById('wolPort').value=c.port||9;
  document.getElementById('wolCancelBtn').classList.remove('d-none');
  setFormOpen('wolFormBody',true);
  document.getElementById('wolFormTitle').scrollIntoView({behavior:'smooth',block:'start'});
}
document.getElementById('wolForm').onsubmit=async e=>{
  e.preventDefault();
  const body={
    name:document.getElementById('wolName').value.trim(),
    mac:document.getElementById('wolMac').value.trim(),
    broadcast:document.getElementById('wolBroadcast').value.trim()||'255.255.255.255',
    port:parseInt(document.getElementById('wolPort').value,10)||9
  };
  const url=editingWolKey?('/api/wol?key='+encodeURIComponent(editingWolKey)):'/api/wol';
  try{
    const r=await fetch(url,{method:editingWolKey?'PUT':'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const res=await r.json().catch(()=>({}));
    msg(r.ok?(editingWolKey?'Компьютер сохранён':'Компьютер добавлен'):'Не удалось сохранить: '+(res.error||r.status),r.ok);
    if(r.ok){resetWolForm();await loadWol();}
  }catch(e){msg('Не удалось сохранить: устройство недоступно',false);}
};
async function wakeWol(key){
  try{
    const r=await fetch('/api/wol/wake?key='+encodeURIComponent(key),{method:'POST'});
    const res=await r.json().catch(()=>({}));
    msg(r.ok?'Магический пакет отправлен — включается «'+((res&&res.name)||'компьютер')+'»':'Не удалось включить: '+(res.error||r.status),r.ok);
  }catch(e){msg('Не удалось отправить пакет: устройство недоступно',false);}
}
async function setDefaultWol(key,on){
  const c=wolCache.find(x=>x.key===key);
  if(!c)return;
  try{
    const r=await fetch('/api/wol?key='+encodeURIComponent(key),{method:'PUT',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({name:c.name,mac:c.mac,broadcast:c.broadcast,port:c.port,default:on})});
    const res=await r.json().catch(()=>({}));
    msg(r.ok?(on?'Компьютер по умолчанию: «'+c.name+'»':'Отметка по умолчанию снята'):'Не удалось сохранить: '+(res.error||r.status),r.ok);
    if(r.ok)await loadWol();
  }catch(e){msg('Не удалось сохранить: устройство недоступно',false);}
}
async function deleteWol(key){
  const c=wolCache.find(x=>x.key===key);
  if(!confirm('Удалить компьютер «'+(c?c.name:key)+'»?'))return;
  const r=await fetch('/api/wol?key='+encodeURIComponent(key),{method:'DELETE'});
  const res=await r.json().catch(()=>({}));
  msg(r.ok?'Компьютер удалён':'Не удалось удалить: '+(res.error||r.status),r.ok);
  if(r.ok){if(editingWolKey===key)resetWolForm();await loadWol();}
}
document.getElementById('wolCancelBtn').onclick=resetWolForm;
document.getElementById('wolExportBtn').onclick=async()=>{
  if(wolCache.length===0){msg('Список компьютеров пуст — экспортировать нечего',false);return;}
  const r=await fetch('/api/wol');
  if(!r.ok){msg('Не удалось экспортировать',false);return;}
  let text;
  try{text=JSON.stringify(JSON.parse(await r.text()),null,2);}
  catch(err){msg('Не удалось экспортировать',false);return;}
  const blob=new Blob([text],{type:'application/json'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob);a.download='wol-computers.json';a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),1000);
  msg('Компьютеры экспортированы');
};
document.getElementById('wolImportBtn').onclick=()=>{
  document.getElementById('wolImportFile').click();
};
document.getElementById('wolImportFile').onchange=async e=>{
  const file=e.target.files[0];
  if(!file)return;
  let data;
  try{data=JSON.parse(await file.text());}
  catch(err){msg('Импорт не удался: неверный JSON-файл',false);e.target.value='';return;}
  const list=Array.isArray(data)?data:(data&&Array.isArray(data.computers)?data.computers:null);
  if(!list||list.length===0){
    msg('Импорт не удался: ожидается непустой массив "computers"',false);e.target.value='';return;}
  if(!confirm('Заменить все текущие компьютеры списком из '+list.length+' компьютеров?')){
    e.target.value='';return;}
  const r=await fetch('/api/wol',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({computers:list})});
  const res=await r.json().catch(()=>({}));
  if(r.ok){msg('Импортировано компьютеров: '+(res.count!==undefined?res.count:list.length));resetWolForm();await loadWol();}
  else{msg('Импорт не удался: '+(res.error||r.status),false);}
  e.target.value='';
};
load();
loadStatus();
setInterval(loadStatus,3000);
setInterval(()=>{if(document.getElementById('pane-timer').classList.contains('active'))loadTimer();},1000);
</script></body></html>)HTML";

// "маяк, mayak" -> {"маяк", "mayak"}. The Web UI sends lists as comma separated
// form values in some places and as JSON arrays in others.
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

bool FeedFromJson(cJSON* root, RssFeed* feed) {
    if (root == nullptr || feed == nullptr || !cJSON_IsObject(root)) {
        return false;
    }
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
        return false;
    }
    feed->name = name->valuestring;
    feed->url = url->valuestring;
    if (cJSON* category = cJSON_GetObjectItem(root, "category"); cJSON_IsString(category)) {
        feed->category = category->valuestring;
    }
    // Aliases may arrive as a JSON array (Web UI) or a comma separated string.
    if (cJSON* aliases = cJSON_GetObjectItem(root, "aliases")) {
        if (cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    feed->aliases.emplace_back(alias->valuestring);
                }
            }
        } else if (cJSON_IsString(aliases)) {
            feed->aliases = SplitAliases(aliases->valuestring);
        }
    }
    return true;
}

}  // namespace

namespace web {

std::string ErrorJson(const std::string& message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddStringToObject(root.get(), "error", message.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t SendJson(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200   ? "200 OK"
                                         : status == 201 ? "201 Created"
                                         : status == 400 ? "400 Bad Request"
                                         : status == 404 ? "404 Not Found"
                                         : status == 409 ? "409 Conflict"
                                         : status == 502 ? "502 Bad Gateway"
                                         : status == 500 ? "500 Internal Server Error"
                                                         : "500 Internal Server Error");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t SendError(httpd_req_t* req, int status, const std::string& message) {
    return SendJson(req, status, ErrorJson(message));
}

bool ReadJsonBody(httpd_req_t* req, std::string& body) {
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodySize)) {
        SendError(req, 400, "Invalid request body size");
        return false;
    }
    body.assign(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            SendError(req, 400, "Failed to read request body");
            return false;
        }
        received += static_cast<size_t>(ret);
    }
    return true;
}

}  // namespace web

WebServer* WebServer::instance_ = nullptr;

WebServer::WebServer() { instance_ = this; }

WebServer::~WebServer() {
    Stop();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool WebServer::Start(int port) {
    if (server_handle_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // The whole device has only CONFIG_LWIP_MAX_SOCKETS (10) sockets, and the
    // MQTT/UDP audio channel, the radio stream and OTA all need some. Reserving
    // 7 for the Web UI left nothing for voice input, which then failed with
    // "Failed to create socket" (ENFILE) after browsing the UI. Keep a few
    // sessions and recycle the oldest instead of refusing new connections.
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.ctrl_port = 32770;
    // The availability checks fetch HTTPS APIs from this task, and the TLS
    // handshake does not fit in the default 4 KB stack.
    config.stack_size = 12 * 1024;
    // The TLS handshake is CPU-heavy. httpd defaults to priority 5, above the
    // AFE fetch task (priority 3), so a request starved the audio pipeline: the
    // codec task kept feeding while nothing drained the AFE ring buffer
    // ("Ringbuffer of AFE(FEED) is full"). Keep the Web UI below the audio
    // tasks; handlers still get CPU because they block on the network.
    config.task_priority = 2;
    // Fourteen endpoints here, five for Wake-on-LAN and eight for the radio API.
    config.max_uri_handlers = 27;

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the Web UI on port %d", port);
        server_handle_ = nullptr;
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = PageHandler, .user_ctx = nullptr},
        {.uri = "/api/timer", .method = HTTP_GET, .handler = TimerHandler, .user_ctx = nullptr},
        {.uri = "/api/timer", .method = HTTP_POST, .handler = TimerSetHandler, .user_ctx = nullptr},
        {.uri = "/api/timer/stop",
         .method = HTTP_POST,
         .handler = TimerStopHandler,
         .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_GET, .handler = RssHandler, .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_POST, .handler = RssSaveHandler, .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_PUT, .handler = RssSaveHandler, .user_ctx = nullptr},
        {.uri = "/api/rss",
         .method = HTTP_DELETE,
         .handler = RssDeleteHandler,
         .user_ctx = nullptr},
        {.uri = "/api/skills", .method = HTTP_GET, .handler = SkillsHandler, .user_ctx = nullptr},
        {.uri = "/api/skills", .method = HTTP_POST, .handler = SkillsHandler, .user_ctx = nullptr},
        {.uri = "/api/system", .method = HTTP_GET, .handler = SystemHandler, .user_ctx = nullptr},
        {.uri = "/api/system", .method = HTTP_POST, .handler = SystemHandler, .user_ctx = nullptr},
        {.uri = "/api/system/reboot", .method = HTTP_POST, .handler = RebootHandler,
         .user_ctx = nullptr},
        {.uri = "/api/system/check", .method = HTTP_GET, .handler = CheckHandler,
         .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        AddHandler(route);
    }
    // The radio and computer tabs ship with the device page, so their endpoints
    // are registered here too instead of from every board that has a radio.
    RadioWebApi::Register(*this);
    WolWebApi::Register(*this);

    ESP_LOGI(TAG, "Web UI started on port %d (http://<device-ip>:%d/), build %s %s", port, port,
             __DATE__, __TIME__);
    return true;
}

void WebServer::Stop() {
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "Web UI stopped");
    }
}

bool WebServer::AddHandler(const httpd_uri_t& uri) {
    if (server_handle_ == nullptr) {
        ESP_LOGE(TAG, "Cannot register %s before the Web UI starts", uri.uri);
        return false;
    }
    if (httpd_register_uri_handler(server_handle_, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler: %s", uri.uri);
        return false;
    }
    return true;
}

esp_err_t WebServer::PageHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandlePage(req);
}

esp_err_t WebServer::TimerHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimer(req);
}

esp_err_t WebServer::TimerSetHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimerSet(req);
}

esp_err_t WebServer::TimerStopHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimerStop(req);
}

esp_err_t WebServer::RssHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRss(req);
}

esp_err_t WebServer::RssSaveHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRssSave(req);
}

esp_err_t WebServer::RssDeleteHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRssDelete(req);
}

esp_err_t WebServer::SkillsHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleSkills(req);
}

esp_err_t WebServer::SystemHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleSystem(req);
}

esp_err_t WebServer::RebootHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleReboot(req);
}

esp_err_t WebServer::CheckHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleCheck(req);
}

esp_err_t WebServer::HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::HandleTimer(httpd_req_t* req) {
    auto* timer = Application::GetInstance().GetTimer();
    bool running = timer != nullptr && timer->IsRunning();
    int remaining_seconds = running ? timer->GetRemainingSeconds() : 0;
    // The countdown can reach zero between IsRunning() and the remaining query.
    if (remaining_seconds <= 0) {
        running = false;
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    std::string remaining = running ? timer->FormatRemaining() : std::string();
    cJSON_AddBoolToObject(root.get(), "running", running);
    cJSON_AddNumberToObject(root.get(), "remainingSeconds", remaining_seconds);
    cJSON_AddStringToObject(root.get(), "remaining", remaining.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleTimerSet(httpd_req_t* req) {
    // httpd_query_key_value wants the query string itself, not the full URI.
    char seconds_text[16] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "seconds", seconds_text, sizeof(seconds_text)) != ESP_OK ||
        seconds_text[0] == '\0') {
        return web::SendError(req, 400, "Missing seconds query parameter");
    }

    char* end = nullptr;
    long seconds = strtol(seconds_text, &end, 10);
    if (end == nullptr || *end != '\0' || seconds <= 0 ||
        seconds > static_cast<long>(CountdownTimer::kMaxDurationSeconds)) {
        return web::SendError(req, 400, "Seconds must be between 1 and 86400");
    }

    auto* timer = Application::GetInstance().GetTimer();
    if (timer == nullptr) {
        return web::SendError(req, 500, "Timer unavailable");
    }
    ESP_LOGI(TAG, "Start timer request: %ld s", seconds);
    if (!timer->Start(static_cast<int>(seconds))) {
        return web::SendError(req, 400, "Failed to start timer");
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    std::string remaining = timer->FormatRemaining();
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddNumberToObject(root.get(), "seconds", static_cast<double>(seconds));
    cJSON_AddStringToObject(root.get(), "remaining", remaining.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    // Setting a timer from the Web UI counts as user activity: wake the display
    // so the countdown in the status line becomes visible. CountdownTimer shows
    // its own short confirmation on the subtitle line and clears it by itself.
    Board::GetInstance().WakeUp();
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleTimerStop(httpd_req_t* req) {
    auto* timer = Application::GetInstance().GetTimer();
    if (timer == nullptr) {
        return web::SendError(req, 500, "Timer unavailable");
    }
    ESP_LOGI(TAG, "Stop timer request");
    if (!timer->Stop()) {
        return web::SendError(req, 409, "No active timer");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

std::string WebServer::FeedsJson() const {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return "{}";
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& [key, feed] : rss->GetFeeds()) {
        CJsonUniquePtr item(cJSON_CreateObject());
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (item == nullptr || aliases == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "key", key.c_str());
        cJSON_AddStringToObject(item.get(), "name", feed.name.c_str());
        cJSON_AddStringToObject(item.get(), "url", feed.url.c_str());
        cJSON_AddStringToObject(item.get(), "category", feed.category.c_str());
        for (const auto& alias : feed.aliases) {
            cJSON_AddItemToArray(aliases.get(), cJSON_CreateString(alias.c_str()));
        }
        cJSON_AddItemToObject(item.get(), "aliases", aliases.release());
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "feeds", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t WebServer::HandleRss(httpd_req_t* req) {
    if (Application::GetInstance().GetRssReader() == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    return web::SendJson(req, 200, FeedsJson());
}

esp_err_t WebServer::HandleRssSave(httpd_req_t* req) {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr) {
        return web::SendError(req, 400, "Invalid JSON body");
    }

    // Import replaces the whole list and is sent as {"feeds": [...]}.
    cJSON* feeds = cJSON_GetObjectItem(root.get(), "feeds");
    if (cJSON_IsArray(feeds)) {
        std::vector<std::pair<std::string, RssFeed>> imported_feeds;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, feeds) {
            RssFeed feed;
            if (!FeedFromJson(item, &feed)) {
                return web::SendError(req, 400,
                                      "Invalid feed entry: name and url are required");
            }
            cJSON* item_key = cJSON_GetObjectItem(item, "key");
            imported_feeds.emplace_back(cJSON_IsString(item_key) ? item_key->valuestring : "",
                                        std::move(feed));
        }
        ESP_LOGI(TAG, "Import RSS feeds request: %d feeds",
                 static_cast<int>(imported_feeds.size()));
        int imported = 0;
        std::string error;
        if (!rss->ReplaceFeeds(imported_feeds, &imported, error)) {
            return web::SendError(req, 400, error);
        }
        CJsonUniquePtr result(cJSON_CreateObject());
        if (result == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON_AddBoolToObject(result.get(), "ok", true);
        cJSON_AddNumberToObject(result.get(), "imported", imported);
        CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    RssFeed feed;
    if (!FeedFromJson(root.get(), &feed)) {
        return web::SendError(req, 400, "Expected JSON with name and url");
    }
    // The device only fetches plain http(s); anything else is a typo in the UI.
    if (feed.name.empty() || feed.name.size() > 64 || feed.url.size() < 8 ||
        feed.url.size() > 256 ||
        (feed.url.rfind("http://", 0) != 0 && feed.url.rfind("https://", 0) != 0)) {
        return web::SendError(req, 400, "Feed needs a name (up to 64 chars) and an http(s) URL");
    }

    // PUT edits an existing feed (key unchanged), POST adds a new one and may
    // carry an explicit key; without a key one is generated from the name.
    cJSON* key_item = cJSON_GetObjectItem(root.get(), "key");
    std::string key = cJSON_IsString(key_item) ? key_item->valuestring : "";
    const bool is_edit = req->method == HTTP_PUT;
    if (is_edit && key.empty()) {
        return web::SendError(req, 400, "Missing key for feed update");
    }
    if (!key.empty() && !RssReader::IsValidKey(key)) {
        return web::SendError(req, 400, "Feed key must be 1-32 chars of A-Z a-z 0-9 _ -");
    }
    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }

    if (is_edit) {
        ESP_LOGI(TAG, "Update RSS feed request: key='%s'", key.c_str());
        if (!rss->UpdateFeed(key, feed)) {
            return web::SendError(req, 404, "Feed not found or failed to save it");
        }
    } else {
        ESP_LOGI(TAG, "Add RSS feed request: key='%s' url='%s'", key.c_str(), feed.url.c_str());
        if (!key.empty() && rss->GetFeed(key, nullptr)) {
            return web::SendError(req, 409, "Feed key already exists");
        }
        std::string new_key;
        if (!rss->AddFeed(feed, key, &new_key)) {
            return web::SendError(req, 409, "Feed list is full or it could not be saved");
        }
        key = std::move(new_key);
    }

    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddStringToObject(result.get(), "key", key.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, is_edit ? 200 : 201, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleRssDelete(httpd_req_t* req) {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    char key[64] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK || key[0] == '\0') {
        return web::SendError(req, 400, "Missing key query parameter");
    }
    ESP_LOGI(TAG, "Delete RSS feed request: key='%s'", key);
    if (!rss->DeleteFeed(key)) {
        return web::SendError(req, 404, "Feed not found or failed to save the list");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

std::string WebServer::SkillsJson() const {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& tool : McpServer::GetInstance().GetToolInfos()) {
        CJsonUniquePtr item(cJSON_CreateObject());
        if (item == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "name", tool.name.c_str());
        cJSON_AddStringToObject(item.get(), "description", tool.description.c_str());
        cJSON_AddBoolToObject(item.get(), "read_only", tool.read_only);
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "tools", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t WebServer::HandleSkills(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return web::SendJson(req, 200, SkillsJson());
    }
    // POST carries {"read_only":["self.get_device_status", ...]}: the full list,
    // so skills the user unchecked are missing from it.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* read_only = root != nullptr ? cJSON_GetObjectItem(root.get(), "read_only") : nullptr;
    if (!cJSON_IsArray(read_only)) {
        return web::SendError(req, 400, "Expected a \"read_only\" array");
    }
    std::vector<std::string> names;
    cJSON* name = nullptr;
    cJSON_ArrayForEach(name, read_only) {
        if (cJSON_IsString(name)) {
            names.emplace_back(name->valuestring);
        }
    }
    ESP_LOGI(TAG, "Save read-only skill list: %d skills", static_cast<int>(names.size()));
    if (!McpServer::GetInstance().SetReadOnlyTools(names)) {
        ESP_LOGE(TAG, "Save read-only skill list: failed to persist it to NVS");
        return web::SendError(req, 500, "Failed to save the skill list");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t WebServer::HandleSystem(httpd_req_t* req) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return web::SendError(req, 503, "Audio codec is not available");
    }
    if (req->method == HTTP_GET) {
        CJsonUniquePtr root(cJSON_CreateObject());
        if (root == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON_AddNumberToObject(root.get(), "volume", codec->output_volume());

        // Status comes from data the device already has for the MCP status tool:
        // the app descriptor and the board status JSON. No new route.
        const esp_app_desc_t* app = esp_app_get_description();
        cJSON* status = cJSON_AddObjectToObject(root.get(), "status");
        if (status != nullptr && app != nullptr) {
            cJSON_AddStringToObject(status, "version", app->version);
            cJSON_AddStringToObject(status, "buildDate", app->date);
            cJSON_AddStringToObject(status, "buildTime", app->time);
            cJSON_AddNumberToObject(status, "uptimeSeconds",
                                    static_cast<double>(esp_timer_get_time()) / 1000000.0);
        }
        // NVS usage in 32-byte entries, so the page can show how full the
        // partition is before a save starts failing.
        nvs_stats_t nvs_stats = {};
        if (status != nullptr && nvs_get_stats(nullptr, &nvs_stats) == ESP_OK &&
            nvs_stats.total_entries > 0) {
            cJSON* nvs = cJSON_AddObjectToObject(status, "nvs");
            if (nvs != nullptr) {
                cJSON_AddNumberToObject(nvs, "used", nvs_stats.used_entries);
                cJSON_AddNumberToObject(nvs, "total", nvs_stats.total_entries);
            }
        }
        CJsonUniquePtr device(cJSON_Parse(Board::GetInstance().GetDeviceStatusJson().c_str()));
        if (device != nullptr) {
            cJSON_AddItemToObject(root.get(), "device", device.release());
        }

        CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    // POST carries {"volume":0..100}; the value applies to the speaker at once.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* volume = root != nullptr ? cJSON_GetObjectItem(root.get(), "volume") : nullptr;
    if (!cJSON_IsNumber(volume)) {
        return web::SendError(req, 400, "Expected a \"volume\" number");
    }
    const int value = static_cast<int>(volume->valuedouble);
    if (value < 0 || value > 100) {
        return web::SendError(req, 400, "Volume must be between 0 and 100");
    }
    ESP_LOGI(TAG, "Set speaker volume: %d", value);
    codec->SetOutputVolume(value);

    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddNumberToObject(result.get(), "volume", codec->output_volume());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{\"ok\":true}");
}

esp_err_t WebServer::HandleReboot(httpd_req_t* req) {
    ESP_LOGW(TAG, "Reboot requested from the Web UI");
    // Reply first: the reboot itself runs from the main task a moment later.
    esp_err_t err = web::SendJson(req, 200, "{\"ok\":true}");
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app.Reboot();
    });
    return err;
}

esp_err_t WebServer::HandleCheck(httpd_req_t* req) {
    const char* query = std::strchr(req->uri, '?');
    char service[16] = {0};
    if (query == nullptr ||
        httpd_query_key_value(query + 1, "service", service, sizeof(service)) != ESP_OK) {
        return web::SendError(req, 400, "Missing \"service\" query parameter");
    }
    std::string error;
    if (std::strcmp(service, "joke") == 0) {
        std::string joke;
        if (!JokeReader::Fetch(joke, error)) {
            ESP_LOGW(TAG, "Joke service is unavailable: %s", error.c_str());
            return web::SendError(req, 502, error);
        }
    } else if (std::strcmp(service, "currency") == 0) {
        CurrencyConverter::Rate rate;
        if (!CurrencyConverter::Convert("USD", "RUB", 1, rate, error)) {
            ESP_LOGW(TAG, "Currency service is unavailable: %s", error.c_str());
            return web::SendError(req, 502, error);
        }
    } else {
        return web::SendError(req, 400, "Unknown service");
    }
    ESP_LOGI(TAG, "Service check passed: %s", service);

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddStringToObject(root.get(), "service", service);
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{\"ok\":true}");
}
