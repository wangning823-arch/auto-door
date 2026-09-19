#include "web_portal.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);
static WebPortal* gPortal = nullptr;

static bool validMac(const String& m) {
  if (m.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) {
      if (m[i] != ':') return false;
    } else {
      char c = m[i];
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
      if (!hex) return false;
    }
  }
  return true;
}

IPAddress WebPortal::apIp() const { return WiFi.softAPIP(); }

String WebPortal::pageHtml() const {
  String mac = store_ ? store_->loadMac(CAR_BT_MAC) : String(CAR_BT_MAC);
  String door = "未知";
  if (door_) {
    DoorState s = door_->doorState();
    if (s == DoorState::OPEN) door = "开";
    else if (s == DoorState::CLOSED) door = "关";
  }
  String rssi = bt_ ? String(bt_->lastRssi()) : "-";
  String bleFilt = store_ ? store_->loadBleFilter() : "";
  if (ble_ && ble_->filter().length()) bleFilt = ble_->filter();
  String bleRssi = "-";
  String bleLab = bleFilt;
  if (ble_ && ble_->matchRssi() > -127) {
    bleRssi = String(ble_->matchRssi());
    if (ble_->matchLabel().length()) bleLab = ble_->matchLabel();
  }
  String trend = "-";
  if (bt_) {
    switch (bt_->trend()) {
      case SignalTrend::GRADUAL_IN: trend = "渐近"; break;
      case SignalTrend::GRADUAL_OUT: trend = "渐离"; break;
      case SignalTrend::STEADY: trend = "稳定"; break;
      case SignalTrend::SUDDEN_APPEAR: trend = "突然出现(忽略开)"; break;
      case SignalTrend::SUDDEN_LOSS: trend = "突然消失(忽略关)"; break;
      default: trend = "未知"; break;
    }
  }

  String html;
  html.reserve(2200);
  html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>车库门控制器</title><style>"
            "body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e7ecf1;}"
            ".wrap{max-width:420px;margin:0 auto;padding:20px 16px 40px;}"
            "h1{font-size:1.15rem;margin:0 0 4px;}"
            ".sub{color:#8b9aab;font-size:.85rem;margin-bottom:18px;}"
            ".card{background:#1a2332;border-radius:12px;padding:14px 16px;margin-bottom:14px;}"
            "label{display:block;font-size:.8rem;color:#8b9aab;margin-bottom:6px;}"
            "input[type=text]{width:100%;box-sizing:border-box;padding:10px 12px;border-radius:8px;"
            "border:1px solid #2e3d52;background:#0f1419;color:#e7ecf1;font-size:1rem;}"
            "button{width:100%;margin-top:10px;padding:12px;border:0;border-radius:8px;font-size:1rem;"
            "background:#2f80ed;color:#fff;font-weight:600;}"
            "button.sec{background:#2a3548;color:#c5d0dc;}"
            ".row{display:flex;justify-content:space-between;padding:6px 0;font-size:.9rem;}"
            ".k{color:#8b9aab;}.v{font-variant-numeric:tabular-nums;}"
            ".ok{color:#3dd68c;}.warn{color:#f2c94c;}"
            ".tip{font-size:.75rem;color:#6b7c8f;margin-top:8px;line-height:1.4;}"
            "</style></head><body><div class=\"wrap\">");
  html += F("<h1>车库门智能控制器</h1><div class=\"sub\">P0 · 无网可用 · 渐变蓝牙判定</div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">热点</span><span class=\"v\">");
  html += apSsid_;
  html += F("</span></div><div class=\"row\"><span class=\"k\">IP</span><span class=\"v\">");
  html += WiFi.softAPIP().toString();
  html += F("</span></div><div class=\"row\"><span class=\"k\">门状态</span><span class=\"v\">");
  html += door;
  html += F("</span></div><div class=\"row\"><span class=\"k\">车机RSSI</span><span class=\"v\">");
  html += rssi;
  html += F("</span></div><div class=\"row\"><span class=\"k\">信号趋势</span><span class=\"v\">");
  html += trend;
  html += F("</span></div></div>");

  // 跟踪模式选择
  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">跟踪模式</span><span class=\"v\">");
  html += (trackMode_ == TRACK_MODE_BLE) ? "BLE" : "经典蓝牙";
  html += F("</span></div>"
            "<form method=\"GET\" action=\"/mode\" style=\"margin-top:8px\">"
            "<label style=\"display:flex;align-items:center;gap:8px;margin:8px 0;cursor:pointer\">"
            "<input type=\"radio\" name=\"m\" value=\"0\" ");
  if (trackMode_ == TRACK_MODE_BLE) html += F("checked ");
  html += F(">BLE（SU7 等有 BLE 广播的车）</label>"
            "<label style=\"display:flex;align-items:center;gap:8px;margin:8px 0;cursor:pointer\">"
            "<input type=\"radio\" name=\"m\" value=\"1\" ");
  if (trackMode_ == TRACK_MODE_CLASSIC) html += F("checked ");
  html += F(">经典蓝牙（小蚂蚁等无 BLE 的车）</label>"
            "<button type=\"submit\">保存模式</button></form>"
            "<div class=\"tip\">BLE 与经典同一套：无→有即开（弱也开）；首见≥-70 不开；强→弱→无关。</div></div>");

  html += F("<div class=\"card\"><form method=\"GET\" action=\"/save\" id=\"macform\">"
            "<label>车机 / 钥匙 蓝牙 MAC</label>"
            "<input type=\"text\" name=\"mac\" id=\"mac\" value=\"");
  html += mac;
  html += F("\" placeholder=\"AA:BB:CC:DD:EE:FF\" maxlength=\"17\" autocapitalize=\"characters\">"
            "<button type=\"submit\">保存 MAC</button></form>"
            "<button class=\"sec\" type=\"button\" onclick=\"startScan()\" id=\"scanBtn\">"
            "扫描附近蓝牙</button>"
            "<div id=\"scanBox\" class=\"tip\"></div>"
            "<div class=\"tip\">点「扫描」约 10 秒。这是<strong>经典蓝牙</strong>搜索："
            "手机需打开蓝牙并开启「可被搜索/开放检测」；车机需开着蓝牙。"
            "扫到后点列表即可填入 MAC。</div></div>");

  html += F("<div class=\"card\">"
            "<div class=\"row\"><span class=\"k\">手动控制</span><span class=\"v\">开/关各发 RF 码，不用门磁</span></div>"
            "<form method=\"GET\" action=\"/door\" style=\"display:flex;gap:8px\">"
            "<button name=\"a\" value=\"open\" type=\"submit\" style=\"flex:1\">开（上）</button>"
            "<button name=\"a\" value=\"close\" type=\"submit\" class=\"sec\" style=\"flex:1\">关（下）</button>"
            "</form>"
            "<div class=\"tip\">开、关是不同遥控码，请用明确的开/关按钮，不要靠模糊状态猜。"
            "串口也可: open / close</div></div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">BLE 特征</span><span class=\"v\">");
  html += bleFilt.length() ? bleFilt : String("(未选)");
  html += F("</span></div><div class=\"row\"><span class=\"k\">BLE RSSI</span><span class=\"v\">");
  html += bleRssi;
  html += F("</span></div>"
            "<button type=\"button\" onclick=\"startBle()\" id=\"bleBtn\">扫描 BLE 设备（约8秒）</button>"
            "<div id=\"bleBox\" class=\"tip\">支持手机、汽车等任意 BLE 设备。点选要跟踪的设备名称保存为特征。</div></div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">射频</span>"
            "<span class=\"v\">WiFi 与蓝牙共用 2.4G</span></div>"
            "<form method=\"GET\" action=\"/wifi/off\">"
            "<button type=\"submit\" style=\"background:#c0392b\">关闭 WiFi（释放给蓝牙）</button>"
            "</form>"
            "<div class=\"tip\">设置完成后点这里：热点断开，后台 Inquiry 不再被压制。"
            "下次要改配置：串口发 <code>wifi on</code>，"
            "或<strong>在程序运行时</strong>长按 BOOT 约 3 秒"
            "（LED 闪两下表示已开热点；不要在上电时按 BOOT，会进下载模式）。</div></div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">自动开</span>"
            "<span class=\"v ok\">仅蓝牙渐近</span></div>"
            "<div class=\"row\"><span class=\"k\">自动关</span>"
            "<span class=\"v ok\">仅渐离+清空</span></div>"
            "<div class=\"row\"><span class=\"k\">突变</span>"
            "<span class=\"v warn\">忽略，不动作</span></div>"
            "<div class=\"row\"><span class=\"k\">防砸车</span>"
            "<span class=\"v ok\">F0 已启用</span></div></div>");

  html += F("<script>"
            "function startScan(){"
            " var b=document.getElementById('scanBtn');"
            " var box=document.getElementById('scanBox');"
            " b.disabled=true; b.textContent='扫描中…';"
            " box.innerHTML='正在搜索经典蓝牙（约10秒）…设备需开启「可被搜索」';"
            " fetch('/scan/start').then(function(){setTimeout(poll,1200);});"
            "}"
            "function poll(){"
            " fetch('/scan/results').then(function(r){return r.json();}).then(function(j){"
            "  var box=document.getElementById('scanBox');"
            "  var b=document.getElementById('scanBtn');"
            "  if(j.running){box.innerHTML='扫描中… 已发现 '+j.devices.length+' 个，正在读名称…';setTimeout(poll,1200);return;}"
            "  b.disabled=false; b.textContent='扫描附近蓝牙';"
            "  if(!j.devices.length){box.innerHTML='未扫到。请确认设备已开蓝牙且开启「可被搜索/开放检测」';return;}"
            "  var h='<div style=\"margin-top:8px\">';"
            "  j.devices.forEach(function(d){"
            "   var nm=d.name&&d.name.length?d.name:'(未读到名称)';"
            "   h+='<div class=\"dev\" onclick=\"pick(\\''+d.mac+'\\')\" "
            "style=\"padding:10px;margin:6px 0;background:#0f1419;border-radius:8px;"
            "cursor:pointer;border:1px solid #2e3d52\">"
            "<div style=\"font-weight:600\">'+nm+'</div>"
            "<div style=\"color:#8b9aab;font-size:.85rem;margin-top:2px\">'+d.mac+' · RSSI '+d.rssi+'</div></div>';"
            "  });"
            "  h+='</div><div class=\"tip\" style=\"margin-top:6px\">点设备名称那一行即可填入 MAC</div>';"
            "  box.innerHTML=h;"
            " }).catch(function(){var b=document.getElementById('scanBtn');b.disabled=false;b.textContent='扫描失败，重试';});"
            "}"
            "function pick(m){document.getElementById('mac').value=m;document.getElementById('mac').scrollIntoView();}"
            "function startBle(){"
            " var b=document.getElementById('bleBtn');"
            " var box=document.getElementById('bleBox');"
            " b.disabled=true; b.textContent='BLE 扫描中…';"
            " box.innerHTML='约 8 秒，请靠近/停稳后等待…';"
            " fetch('/ble/scan').then(function(){setTimeout(pollBle,2000);});"
            "}"
            "function pollBle(){"
            " fetch('/ble/results').then(function(r){return r.json();}).then(function(j){"
            "  var box=document.getElementById('bleBox');"
            "  var b=document.getElementById('bleBtn');"
            "  if(j.busy){box.innerHTML='扫描中… 已发现 '+j.n+' 个候选…';setTimeout(pollBle,1500);return;}"
            "  b.disabled=false; b.textContent='扫描 BLE 设备（约8秒）';"
            "  if(!j.devices||!j.devices.length){box.innerHTML='未扫到 BLE 设备，请确认周围有蓝牙设备在广播';return;}"
            "  var h='<div style=\"margin-top:8px\">';"
            "  j.devices.forEach(function(d){"
            "   var nm=d.name&&d.name.length?d.name:'(无名)';"
            "   h+='<div onclick=\"saveBle(\\''+encodeURIComponent(d.name||d.mac)+'\\')\" "
            "style=\"padding:10px;margin:6px 0;background:#0f1419;border-radius:8px;"
            "cursor:pointer;border:1px solid #2e3d52\">"
            "   <div style=\"font-weight:600\">'+nm+'</div>"
            "   <div style=\"color:#8b9aab;font-size:.85rem;margin-top:2px\">'+d.mac+' · RSSI '+d.rssi+'</div></div>';"
            "  });"
            "  h+='</div><div class=\"tip\">点要跟踪的设备名称，会写入 BLE 特征并开始跟踪</div>';"
            "  box.innerHTML=h;"
            " }).catch(function(){var b=document.getElementById('bleBtn');b.disabled=false;b.textContent='BLE 扫描失败';});"
            "}"
            "function saveBle(v){"
            " var name=decodeURIComponent(v);"
            " location.href='/ble/save?f='+encodeURIComponent(name);"
            "}"
            "</script>");

  html += F("</div></body></html>");
  return html;
}

void WebPortal::setupRoutes() {
  server.on("/", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    server.send(200, "text/html; charset=utf-8", gPortal->pageHtml());
  });

  server.on("/save", HTTP_GET, []() {
    if (!gPortal || !gPortal->store_) {
      server.send(500, "text/plain", "no store");
      return;
    }
    String mac = server.arg("mac");
    mac.trim();
    mac.toUpperCase();
    if (!validMac(mac)) {
      server.send(400, "text/html; charset=utf-8",
                  F("<meta charset=utf-8><p>MAC 格式错误，请用 AA:BB:CC:DD:EE:FF</p>"
                    "<p><a href=/>返回</a></p>"));
      return;
    }
    gPortal->store_->saveMac(mac);
    if (gPortal->bt_) gPortal->bt_->begin(mac.c_str());
    Serial.println("[WEB] MAC saved: " + mac);
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>已保存</h2><p>车机 MAC：<code>");
    body += mac;
    body += F("</code></p><p style='color:#3dd68c'>已写入闪存并立即生效</p>"
              "<p><a style='color:#2f80ed' href='/'>返回设置</a></p></body>");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.on("/door", HTTP_GET, []() {
    if (!gPortal || !gPortal->door_) {
      server.send(500, "text/plain", "no door");
      return;
    }
    String a = server.arg("a");
    if (a == "open") {
      gPortal->door_->requestManualOpen(OpenSource::NFC);
    } else if (a == "close") {
      gPortal->door_->requestManualClose(OpenSource::NFC);
    } else {
      // 兼容旧 toggle：默认当开（开/关为不同 RF 码，无门磁）
      gPortal->door_->requestManualOpen(OpenSource::NFC);
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "ok");
  });

  server.on("/mode", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    String m = server.arg("m");
    int mode = m.toInt();
    if (mode == TRACK_MODE_BLE || mode == TRACK_MODE_CLASSIC) {
      gPortal->setTrackMode(mode);
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "ok");
  });

  server.on("/status", HTTP_GET, []() {
    String j = "{";
    if (gPortal && gPortal->door_) {
      j += "\"door\":" + String((int)gPortal->door_->doorState());
    }
    if (gPortal && gPortal->bt_) {
      j += ",\"rssi\":" + String(gPortal->bt_->lastRssi());
      j += ",\"trend\":" + String((int)gPortal->bt_->trend());
      j += ",\"zone\":" + String((int)gPortal->bt_->zone());
    }
    j += "}";
    server.send(200, "application/json", j);
  });

  server.on("/wifi/off", HTTP_GET, []() {
    if (gPortal && gPortal->store_) {
      gPortal->store_->saveWifiEnabled(false);
    }
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>正在关闭 WiFi…</h2>"
          "<p style='color:#3dd68c'>已写入配置：下次开机默认无网，蓝牙 Inquiry 独占射频。</p>"
          "<p style='color:#8b9aab'>约 2 秒后本页面断开。</p>"
          "<p>重新打开方式：USB 串口 <code>wifi on</code>，"
          "或长按 BOOT 约 3 秒后上电。</p></body>");
    server.send(200, "text/html; charset=utf-8", body);
    delay(400);
    if (gPortal) gPortal->stopAp();
    Serial.println("[WEB] WiFi SoftAP stopped by user (persisted wifi_on=0)");
  });

  server.on("/wifi/on", HTTP_GET, []() {
    if (gPortal && gPortal->store_) {
      gPortal->store_->saveWifiEnabled(true);
    }
    bool ok = gPortal ? gPortal->startAp() : false;
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>WiFi 已打开</h2><p>请重新连接热点后刷新 192.168.4.1</p></body>");
    if (ok) {
      server.send(200, "text/html; charset=utf-8", body);
    } else {
      server.send(500, "text/plain", "startAp failed");
    }
    Serial.println("[WEB] WiFi SoftAP re-enabled");
  });

  server.on("/ble/scan", HTTP_GET, []() {
    if (!gPortal || !gPortal->ble_) {
      server.send(500, "application/json", "{\"error\":\"no ble\"}");
      return;
    }
    // 同步扫 8 秒（请求会等一会儿，手机上显示加载即可）
    gPortal->ble_->runScan(8000);
    server.send(200, "application/json", "{\"ok\":1}");
  });

  server.on("/ble/results", HTTP_GET, []() {
    if (!gPortal || !gPortal->ble_) {
      server.send(500, "application/json", "{\"error\":\"no ble\"}");
      return;
    }
    auto list = gPortal->ble_->interestingHits();
    String j = "{\"busy\":false,\"n\":";
    j += String((int)list.size());
    j += ",\"filter\":\"" + gPortal->ble_->filter();
    j += "\",\"match_rssi\":" + String(gPortal->ble_->matchRssi());
    j += ",\"devices\":[";
    for (size_t i = 0; i < list.size(); i++) {
      if (i) j += ",";
      String nm = list[i].name;
      nm.replace("\"", "'");
      j += "{\"mac\":\"" + list[i].addr + "\",\"rssi\":" + String(list[i].rssi);
      j += ",\"name\":\"" + nm + "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
  });

  server.on("/ble/save", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    String f = server.arg("f");
    f.trim();
    if (f.length() == 0) {
      server.send(400, "text/plain", "empty filter");
      return;
    }
    if (gPortal->store_) gPortal->store_->saveBleFilter(f);
    if (gPortal->ble_) {
      gPortal->ble_->setFilter(f);
      gPortal->ble_->setTrack(true);
    }
    Serial.println("[WEB] BLE filter saved+track: " + f);
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>BLE 特征已保存</h2><p><code>");
    body += f;
    body += F("</code></p><p style='color:#3dd68c'>已开始周期跟踪，可回到首页看 RSSI</p>"
              "<p><a style='color:#2f80ed' href='/'>返回</a></p></body>");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.on("/scan/start", HTTP_GET, []() {
    if (!gPortal || !gPortal->bt_) {
      server.send(500, "application/json", "{\"error\":\"no bt\"}");
      return;
    }
    gPortal->bt_->startDiscovery(10000);
    server.send(200, "application/json", "{\"ok\":1,\"ms\":10000}");
  });

  server.on("/scan/results", HTTP_GET, []() {
    if (!gPortal || !gPortal->bt_) {
      server.send(500, "application/json", "{\"error\":\"no bt\"}");
      return;
    }
    bool running = gPortal->bt_->discoveryRunning();
    auto list = gPortal->bt_->discoveryResults();
    String j = "{\"running\":";
    j += running ? "true" : "false";
    j += ",\"devices\":[";
    for (size_t i = 0; i < list.size(); i++) {
      if (i) j += ",";
      j += "{\"mac\":\"" + list[i].mac + "\",\"rssi\":" + String(list[i].rssi);
      j += ",\"name\":\"" + list[i].name + "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
  });

  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
}

void WebPortal::begin(ConfigStore* store, BleTracker* bt, DoorFsm* door,
                      BleScanTool* ble, bool enableAp) {
  gPortal = this;
  store_ = store;
  bt_ = bt;
  door_ = door;
  ble_ = ble;

  // 加载跟踪模式
  if (store_) {
    trackMode_ = store_->loadTrackMode(TRACK_MODE_DEFAULT);
  }

  if (ble_ && store_) {
    String f = store_->loadBleFilter();
    if (f.length()) {
      ble_->setFilter(f);
      ble_->setTrack(true);
      Serial.println("[WEB] BLE filter from NVS: " + f);
    }
  }

  uint64_t chipid = ESP.getEfuseMac();
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%04X", (uint16_t)(chipid & 0xFFFF));
  apSsid_ = String(AP_SSID_PREFIX) + suffix;

  setupRoutes();
  serverStarted_ = false;

  // 先 SoftAP，再 HTTP。顺序反了会 assert tcpip_send_msg_wait_sem
  if (enableAp) {
    startAp();
  } else {
    // 纯蓝牙模式：不要 server.begin()（无 netif 时同样会 assert）
    Serial.println("[WEB] SoftAP disabled by config (BT-only mode, no HTTP)");
    apActive_ = false;
    if (bt_) bt_->setInquiryPaused(false);
  }
}

bool WebPortal::startAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  delay(200);
  bool ok = WiFi.softAP(apSsid_.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
  delay(100);
  apActive_ = ok;
  if (ok && !serverStarted_) {
    server.begin();
    server.enableDelay(false);
    serverStarted_ = true;
    Serial.println("[WEB] HTTP routes ready");
  }
  if (bt_) bt_->setInquiryPaused(false);
  Serial.printf("[WEB] SoftAP %s pass=%s -> %s ip=%s\n", apSsid_.c_str(),
                AP_PASSWORD, ok ? "OK" : "FAIL",
                WiFi.softAPIP().toString().c_str());
  return ok;
}

void WebPortal::stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apActive_ = false;
  if (bt_) bt_->setInquiryPaused(false);
}

void WebPortal::loop() {
  if (!apActive_) return;

  server.handleClient();
  // 有客户端连热点时：只停「后台跟踪 inquiry」，不停用户点的扫描
  if (bt_) {
    int clients = WiFi.softAPgetStationNum();
    static int lastC = -1;
    if (clients != lastC) {
      lastC = clients;
      Serial.printf("[WEB] softAP clients=%d\n", clients);
    }
    bt_->setInquiryPaused(clients > 0);
  }
}

void WebPortal::setTrackMode(int mode) {
  if (mode != TRACK_MODE_BLE && mode != TRACK_MODE_CLASSIC) return;
  trackMode_ = mode;
  if (store_) store_->saveTrackMode(mode);
  Serial.println("[WEB] track mode -> " + String(mode == TRACK_MODE_BLE ? "BLE" : "Classic"));
}
