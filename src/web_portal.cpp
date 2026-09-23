#include "web_portal.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "ble_bond.h"

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

bool WebPortal::staConfigured() const {
  return staWanted_;
}

bool WebPortal::staConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WebPortal::staIp() const {
  if (!staConnected()) return String("-");
  return WiFi.localIP().toString();
}

String WebPortal::pageHtml() const {
  String mac = store_ ? store_->loadMac(CAR_BT_MAC) : String(CAR_BT_MAC);
  String door = "未知";
  if (door_) {
    DoorState s = door_->doorState();
    if (s == DoorState::OPEN) door = "开";
    else if (s == DoorState::CLOSED) door = "关";
  }
  String rssi = "-";
  if (bt_) {
    int rv = bt_->lastRssi();
    if (rv <= -127)
      rssi = "丢失(未再扫到)";
    else
      rssi = String(rv) + (bt_->seenRecently(20000) ? "" : " (旧)");
  }
  String bleRssi = "-";
  String bleLab = gBleBond.hasIrk() ? gBleBond.identityMac() : String("(未配对)");
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
  html += F("<h1>车库门智能控制器</h1><div class=\"sub\">P0 · 无网可用 · 渐变蓝牙判定 · v"
            FW_VERSION "</div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">热点</span><span class=\"v\">");
  html += apSsid_;
  html += F("</span></div><div class=\"row\"><span class=\"k\">IP</span><span class=\"v\">");
  html += WiFi.softAPIP().toString();
  html += F("</span></div><div class=\"row\"><span class=\"k\">家庭Wi‑Fi</span><span class=\"v ");
  if (staConnected())
    html += F("ok\">");
  else if (staWanted_)
    html += F("warn\">");
  else
    html += F("\">");
  if (!staWanted_)
    html += F("未配置（OTA 需要）");
  else if (staConnected())
    html += staIp() + F(" · OTA: ") + host_ + F(".local");
  else
    html += F("连接中/失败");
  html += F("</span></div><div class=\"row\"><span class=\"k\">门状态</span><span class=\"v\">");
  html += door;
  html += F("</span></div><div class=\"row\"><span class=\"k\">车机RSSI</span><span class=\"v\">");
  html += rssi;
  html += F("</span></div><div class=\"row\"><span class=\"k\">信号趋势</span><span class=\"v\">");
  html += trend;
  html += F("</span></div>"
            "<a href=\"/rssi\" style=\"display:block;margin-top:10px;text-align:center;"
            "padding:12px;border-radius:10px;background:#2f80ed;color:#fff;"
            "text-decoration:none;font-weight:600\">RSSI curve over time</a>"
            "</div>");

  // 家庭 Wi‑Fi（STA）：保存后可从书桌 espota 烧录，不必再拔 USB
  {
    String staSsid = store_ ? store_->loadStaSsid() : String();
    html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">家庭 Wi‑Fi（无线烧录）</span><span class=\"v\">");
    html += staSsid.length() ? staSsid : String("未设置");
    html += F("</span></div>"
              "<form method=\"GET\" action=\"/wifista\">"
              "<label>SSID</label>"
              "<input type=\"text\" name=\"s\" value=\"");
    html += staSsid;
    html += F("\" placeholder=\"车库路由名称\" maxlength=\"32\" autocapitalize=\"off\">"
              "<label style=\"margin-top:8px\">密码</label>"
              "<input type=\"password\" name=\"p\" value=\"\" placeholder=\"密码（留空=不改）\" maxlength=\"64\">"
              "<button type=\"submit\">保存并连接</button></form>"
              "<form method=\"GET\" action=\"/wifista/clear\">"
              "<button type=\"submit\" class=\"sec\">清除家庭 Wi‑Fi</button></form>"
              "<div class=\"tip\">保存后设备会以 STA 接入该路由；连上后可用 "
              "<code>upload_port=</code> 里的主机名做 espota，不必去车库插 USB。"
              "热点仍可同时开（APSTA）做配置。测自动门仍建议网页「关闭 WiFi」。</div></div>");
  }

  // 跟踪模式选择
  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">跟踪模式</span><span class=\"v\">");
  html += (trackMode_ == TRACK_MODE_BLE) ? "BLE" : "经典蓝牙";
  html += F("</span></div><div class=\"row\"><span class=\"k\">自动跟踪</span><span class=\"v\">");
  html += (bt_ && bt_->autoTrack()) ? "开" : "关";
  html += F("</span></div>"
            "<form method=\"GET\" action=\"/mode\" style=\"margin-top:8px\">"
            "<label style=\"display:flex;align-items:center;gap:8px;margin:8px 0;cursor:pointer\">"
            "<input type=\"radio\" name=\"m\" value=\"0\" ");
  if (trackMode_ == TRACK_MODE_BLE) html += F("checked ");
  html += F(">BLE（配对手机 IRK 跟踪）</label>"
            "<label style=\"display:flex;align-items:center;gap:8px;margin:8px 0;cursor:pointer\">"
            "<input type=\"radio\" name=\"m\" value=\"1\" ");
  if (trackMode_ == TRACK_MODE_CLASSIC) html += F("checked ");
  html += F(">经典蓝牙（小蚂蚁等车机 MAC）</label>"
            "<button type=\"submit\">保存模式</button></form>"
            "<div class=\"tip\">开/关：无→有且&lt;-80立刻开；≥-80不开；离场≤-90约10m关。"
            "经典模式保存后会自动打开周期 Inquiry；网页会显示「自动跟踪」状态。</div></div>");

  // ===== 手机配对：开关 + 6位PIN（仅 BLE 模式显示；经典模式隐藏）=====
  if (trackMode_ == TRACK_MODE_BLE) {
  html += F("<div class=\"card\">"
            "<div class=\"row\"><span class=\"k\">手机配对</span><span class=\"v ");
  if (gBleBond.pairingOpen())
    html += F("warn\">开 · 90秒内可绑 GarageDoorBLE</span></div>");
  else
    html += F("ok\">关 · 拒绝新绑定</span></div>");
  html += F("<div class=\"row\"><span class=\"k\">已配对手机</span><span class=\"v\">");
  html += gBleBond.hasIrk() ? gBleBond.identityMac() : String("(尚未绑定)");
  html += F("</span></div>"
            "<div class=\"row\"><span class=\"k\">手机配对 PIN</span><span class=\"v\">");
  html += gBleBond.hasPasskey() ? gBleBond.pairingPin()
                                : String("未设置(Just Works)");
  html += F("</span></div>"
            "<form method=\"GET\" action=\"/pair\" style=\"display:flex;gap:8px;margin-top:8px\">"
            "<input type=\"hidden\" name=\"a\" value=\"on\">"
            "<button type=\"submit\" style=\"margin-top:0;flex:1\">打开配对（90秒）</button>"
            "</form>"
            "<form method=\"GET\" action=\"/pair\">"
            "<input type=\"hidden\" name=\"a\" value=\"off\">"
            "<button type=\"submit\" class=\"sec\" style=\"margin-top:8px\">关闭配对</button>"
            "</form>"
            "<form method=\"GET\" action=\"/pairpin\" style=\"display:flex;gap:8px;margin-top:10px\">"
            "<input type=\"password\" name=\"p\" placeholder=\"6位数字PIN（手机配对时输入；留空清除）\" "
            "maxlength=\"6\" inputmode=\"numeric\" "
            "style=\"flex:1;min-width:0;padding:10px 12px;border-radius:8px;"
            "border:1px solid #2e3d52;background:#0f1419;color:#e7ecf1;letter-spacing:4px\">"
            "<button type=\"submit\" class=\"sec\" style=\"margin-top:0;flex:1\">保存 PIN</button>"
            "</form>"
            "<form method=\"GET\" action=\"/pair\">"
            "<input type=\"hidden\" name=\"a\" value=\"unpair\">"
            "<button type=\"submit\" class=\"sec\" style=\"margin-top:8px;background:#5a2a2a\">"
            "解绑并清系统蓝牙</button>"
            "</form>"
            "<div class=\"tip\">打开后 90 秒手机可搜 <b>GarageDoorBLE</b>。"
            "若已设 6 位 PIN，手机配对时要输入该 PIN（串口会打印同一 PIN）。"
            "关闭配对 = 停广播 + 拒绝绑定 + 断开连接。"
            "换手机：解绑再开配对。</div></div>");
  } else {
    html += F("<div class=\"card\">"
              "<div class=\"row\"><span class=\"k\">手机配对</span>"
              "<span class=\"v\">经典模式已禁用（切 BLE 后可配对）</span></div></div>");
  }

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

  html += F("<div class=\"card\">"
            "<div class=\"row\"><span class=\"k\">配对手机跟踪</span><span class=\"v\">");
  html += bleLab;
  html += F("</span></div><div class=\"row\"><span class=\"k\">手机 RSSI</span><span class=\"v\">");
  html += bleRssi;
  html += F("</span></div>"
            "<div class=\"tip\">BLE 模式只用已配对手机的 IRK+RSSI 判断进出，"
            "不再按名称/MAC 特征过滤。名称扫描已移除；经典车机仍用上方 MAC 扫描。</div></div>");

  html += F("<div class=\"card\">"
            "<div class=\"row\"><span class=\"k\">NFC</span><span class=\"v\">");
  if (nfc_) {
    if (nfc_->ok())
      html += F("ok</span></div>");
    else if (nfc_->deferred())
      html += F("defer · 可网页强制初始化</span></div>");
    else
      html += F("wait · 自动初始化中</span></div>");
  } else {
    html += F("-</span></div>");
  }
  html += F("<form method=\"GET\" action=\"/nfcinit\">"
            "<button type=\"submit\" class=\"sec\">重新初始化 NFC</button></form>"
            "<div class=\"tip\">上电约 5 秒后会自动初始化；失败则约每 10 分钟慢速重试。"
            "刷卡无效时可在此强制 nfcinit。</div></div>");

  html += F("<div class=\"card\"><div class=\"row\"><span class=\"k\">射频</span>"
            "<span class=\"v\">WiFi 与蓝牙共用 2.4G</span></div>"
            "<form method=\"GET\" action=\"/wifi/off\">"
            "<button type=\"submit\" style=\"background:#c0392b\">关热点（保留家庭 Wi‑Fi 看网页）</button>"
            "</form>"
            "<div class=\"tip\"><strong>热点开着时不起蓝牙栈</strong>（防网页/复位问题）。"
            "点上方关热点后：STA 网页仍可用；约 1.5 秒后自动起经典蓝牙，"
            "再点「扫描」。自动门同样在关热点后才跑。</div></div>");

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
            "</script>");

  html += F("</div></body></html>");
  return html;
}

void WebPortal::setupRoutes() {
  // 手机连上「无互联网」热点后会探测这些 URL；不答 204 会被系统判为无法上网，
  // 用户即使已关联也可能打不开本地页。全部指回配置页。
  auto sendCaptive = []() {
    if (!gPortal) {
      server.send(200, "text/html", "garage-door");
      return;
    }
    String html =
        F("<!DOCTYPE html><html><head><meta charset=utf-8>"
          "<meta http-equiv=refresh content=\"0;url=http://192.168.4.1/\">"
          "<title>GarageDoor</title></head><body>"
          "<p>正在打开车库门配置页… <a href=http://192.168.4.1/>192.168.4.1</a></p>"
          "</body></html>");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", html);
  };

  server.on("/generate_204", HTTP_GET, sendCaptive);
  server.on("/gen_204", HTTP_GET, sendCaptive);
  server.on("/hotspot-detect.html", HTTP_GET, sendCaptive);
  server.on("/library/test/success.html", HTTP_GET, sendCaptive);
  server.on("/ncsi.txt", HTTP_GET, sendCaptive);
  server.on("/connecttest.txt", HTTP_GET, sendCaptive);
  server.on("/success.txt", HTTP_GET, sendCaptive);
  server.on("/canonical.html", HTTP_GET, sendCaptive);
  server.on("/ping", HTTP_GET, []() {
    Serial.printf("[WEB] GET /ping from %s\n",
                  server.client().remoteIP().toString().c_str());
    server.send(200, "text/plain", "pong " + String((unsigned)millis()));
  });

  server.on("/rssi/data", HTTP_GET, []() {
    if (!gPortal || !gPortal->bt_) {
      server.send(200, "application/json", "{\"n\":0,\"t\":[],\"r\":[]}");
      return;
    }
    static uint32_t tBuf[BleTracker::TS_N];
    static int16_t rBuf[BleTracker::TS_N];
    int n = gPortal->bt_->tsExport(tBuf, rBuf, BleTracker::TS_N);
    String j;
    j.reserve((size_t)n * 10 + 80);
    j += "{\"n\":";
    j += String(n);
    j += ",\"t\":[";
    for (int i = 0; i < n; i++) {
      if (i) j += ",";
      j += String(tBuf[i]);
    }
    j += "],\"r\":[";
    for (int i = 0; i < n; i++) {
      if (i) j += ",";
      j += String(rBuf[i]);
    }
    j += "],\"cur\":";
    j += String(gPortal->bt_->lastRssi());
    j += ",\"auto\":";
    j += gPortal->bt_->autoTrack() ? 1 : 0;
    j += ",\"target\":";
    j += gPortal->bt_->hasTarget() ? 1 : 0;
    j += "}";
    server.send(200, "application/json", j);
  });

  server.on("/rssi", HTTP_GET, []() {
    String h;
    h.reserve(3200);
    h += F(
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>RSSI</title><style>"
        "body{font-family:system-ui;background:#0f1419;color:#e7ecf1;margin:0;padding:12px}"
        "a{color:#2f80ed}h1{font-size:18px;margin:0 0 8px}"
        "#bar{font-size:13px;color:#8b9aab;margin-bottom:8px}"
        "canvas{width:100%;height:280px;background:#151c26;border-radius:10px;"
        "border:1px solid #2e3d52}"
        ".row{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}"
        "button,a.btn{padding:10px 14px;border-radius:8px;border:0;"
        "background:#2f80ed;color:#fff;font-size:14px;text-decoration:none}"
        ".sec{background:#2e3d52}</style></head><body>"
        "<h1>RSSI over time</h1><div id=bar>loading...</div>"
        "<canvas id=c width=640 height=280></canvas><div class=row>"
        "<button onclick='tick()'>Refresh</button>"
        "<a class=btn href=/>Back</a></div><script>"
        "function draw(d){var c=document.getElementById('c'),x=c.getContext('2d');"
        "var W=c.width,H=c.height;x.clearRect(0,0,W,H);"
        "x.strokeStyle='#2e3d52';x.lineWidth=1;"
        "for(var y=0;y<=4;y++){var py=20+y*(H-40)/4;x.beginPath();"
        "x.moveTo(40,py);x.lineTo(W-8,py);x.stroke();"
        "x.fillStyle='#8b9aab';x.font='11px sans-serif';"
        "x.fillText(String(-40-y*20),4,py+4);}"
        "var n=d.n||0;document.getElementById('bar').textContent="
        "'n='+n+' cur='+(d.cur!=null?d.cur:'-')+' auto='+(d.auto||0)"
        "+' target='+(d.target||0)+(n?' span='+d.t[n-1]+'s':'');"
        "if(n<2)return;"
        "var t0=d.t[0],t1=d.t[n-1];if(t1<=t0)t1=t0+1;"
        "function X(t){return 40+(t-t0)*(W-50)/(t1-t0);}"
        "function Y(r){if(r<=-127)return H-10;"
        "var rr=Math.max(-120,Math.min(-40,r));"
        "return 20+((-40-rr)/80)*(H-40);}"
        "x.strokeStyle='#3dd68c';x.lineWidth=2;x.beginPath();"
        "var pen=false;for(var i=0;i<n;i++){"
        "if(d.r[i]<=-127){pen=false;continue;}"
        "var px=X(d.t[i]),py=Y(d.r[i]);"
        "if(!pen){x.moveTo(px,py);pen=true;}else x.lineTo(px,py);}"
        "x.stroke();}"
        "function tick(){fetch('/rssi/data').then(function(r){return r.json();})"
        ".then(draw).catch(function(){"
        "document.getElementById('bar').textContent='fetch error';});}"
        "tick();setInterval(tick,2000);</script></body></html>");
    server.send(200, "text/html; charset=utf-8", h);
  });

  server.on("/", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    uint32_t t0 = millis();
    Serial.printf("[WEB] GET / from %s\n",
                  server.client().remoteIP().toString().c_str());
    String html = gPortal->pageHtml();
    server.send(200, "text/html; charset=utf-8", html);
    Serial.printf("[WEB] GET / bytes=%u gen=%ums clients=%d\n",
                  (unsigned)html.length(), (unsigned)(millis() - t0),
                  WiFi.softAPgetStationNum());
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
    // 二选一：只有经典模式才起 SerialBT；BLE 模式只存 MAC 不拉经典栈
    if (gPortal->bt_ && gPortal->trackMode() == TRACK_MODE_CLASSIC) {
      gPortal->bt_->begin(mac.c_str());
    }
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

  server.on("/pairpin", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    gBleBond.setPairingPin(server.arg("p"));
    String st = gBleBond.hasPasskey()
                    ? String("当前 PIN=") + gBleBond.pairingPin()
                    : String("当前未设 PIN（Just Works）");
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'><h2>手机配对 PIN 已保存</h2><p>");
    body += st;
    body += F("</p><p style='color:#8b9aab'>手机需先删除旧的 GarageDoorBLE 配对，"
              "再重新连接才会要求输入 PIN</p>"
              "<p><a style='color:#2f80ed' href='/'>返回首页</a></p></body>");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.on("/pair", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "text/plain", "no portal");
      return;
    }
    if (gPortal->trackMode() != TRACK_MODE_BLE) {
      server.send(200, "text/html; charset=utf-8",
                  F("<meta charset=utf-8><p>当前是经典蓝牙模式，BLE 配对已禁用。</p>"
                    "<p><a href=/>返回</a></p>"));
      return;
    }
    String a = server.hasArg("a") ? server.arg("a") : String();
    String p = server.arg("p");
    Serial.printf("[WEB] /pair a='%s' p_len=%u open=%d\n", a.c_str(),
                  (unsigned)p.length(), (int)gBleBond.pairingOpen());

    auto back = [](const char* title, const char* msg, const char* color) {
      String body =
          F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'>"
            "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
            "padding:24px;text-align:center'><h2>");
      body += title;
      body += F("</h2><p style='color:");
      body += color;
      body += "'>";
      body += msg;
      body += F("</p><p><a style='color:#2f80ed' href='/'>返回首页</a></p></body>");
      return body;
    };

    if (a == "off") {
      gBleBond.closePairingWindow("web");
      server.send(200, "text/html; charset=utf-8",
                  back("配对已关闭",
                       "已停广播、断开连接；手机应搜不到 GarageDoorBLE，且无法再绑定",
                       "#3dd68c"));
      return;
    }
    if (a == "unpair") {
      gBleBond.clearBond("web");
      gBleBond.closePairingWindow("unpair");
      server.send(200, "text/html; charset=utf-8",
                  back("已解绑",
                       "已清 IRK 与系统蓝牙 bond；请再点「打开配对」绑新手机",
                       "#f2c94c"));
      return;
    }
    if (a != "on") {
      server.send(400, "text/html; charset=utf-8",
                  back("参数错误", "缺少 a=on/off（表单请用 hidden 字段）",
                       "#e74c3c"));
      return;
    }
    // 只请求开窗；真正 startAdv 在 loop/service 里，避免 HTTP 回调打断 SoftAP
    gBleBond.requestOpenPairing(90000);
    String pinMsg = gBleBond.hasPasskey()
                        ? String("请先在手机删除旧 GarageDoorBLE，再搜索并输入 PIN ") +
                              gBleBond.pairingPin()
                        : String("手机搜索 GarageDoorBLE 并确认配对");
    server.send(200, "text/html; charset=utf-8",
                back("已请求打开配对（约1秒后生效）", pinMsg.c_str(),
                     "#f2c94c"));
  });

  server.on("/nfcinit", HTTP_GET, []() {
    if (!gPortal || !gPortal->nfc_) {
      server.send(500, "text/html; charset=utf-8",
                  F("<meta charset=utf-8><p>NFC 模块未接入</p><p><a href=/>返回</a></p>"));
      return;
    }
    Serial.println("[WEB] /nfcinit forceInit...");
    bool ok = gPortal->nfc_->forceInit();
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'><h2>NFC 初始化");
    body += ok ? F("成功</h2><p style='color:#3dd68c'>读头已就绪，可刷卡测试</p>")
               : F("失败</h2><p style='color:#e74c3c'>检查 I2C 接线/供电；"
                   "串口可看 SCL 电平。可稍后再试。</p>");
    body += F("<p><a style='color:#2f80ed' href='/'>返回设置</a></p></body>");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.on("/status", HTTP_GET, []() {
    String j = "{";
    if (gPortal && gPortal->door_) {
      j += "\"door\":" + String((int)gPortal->door_->doorState());
    }
    if (gPortal && gPortal->bt_) {
      j += ",\"rssi\":" + String(gPortal->bt_->lastRssi());
      j += ",\"seen\":" + String(gPortal->bt_->seenRecently(20000) ? 1 : 0);
      j += ",\"auto\":" + String(gPortal->bt_->autoTrack() ? 1 : 0);
      j += ",\"trend\":" + String((int)gPortal->bt_->trend());
      j += ",\"zone\":" + String((int)gPortal->bt_->zone());
      j += ",\"target\":" + String(gPortal->bt_->hasTarget() ? 1 : 0);
      j += ",\"inquiry\":" + String(gPortal->bt_->inquiryBusy() ? 1 : 0);
    }
    j += ",\"ap\":" + String(gPortal && gPortal->apActive() ? 1 : 0);
    j += ",\"ap_clients\":" + String(WiFi.softAPgetStationNum());
    j += ",\"sta\":" + String(gPortal && gPortal->staConnected() ? 1 : 0);
    if (gPortal && gPortal->staConnected()) {
      j += ",\"sta_ip\":\"" + gPortal->staIp() + "\"";
    }
    j += "}";
    server.send(200, "application/json", j);
  });

  // 桌面 OTA 客户端：在线 / 版本 / 能否升级
  server.on("/ota", HTTP_GET, []() {
    if (!gPortal) {
      server.send(500, "application/json", "{\"ok\":0}");
      return;
    }
    bool sta = gPortal->staConnected();
    bool ota = gPortal->otaReady();
    String j;
    j.reserve(256);
    j += "{\"ok\":1,\"fw\":\"" FW_VERSION "\",\"host\":\"";
    j += gPortal->host_;
    j += "\"";
    j += ",\"ip\":\"";
    j += sta ? gPortal->staIp() : String();
    j += "\"";
    j += ",\"ap_ip\":\"";
    j += WiFi.softAPIP().toString();
    j += "\"";
    j += ",\"sta\":";
    j += sta ? 1 : 0;
    j += ",\"ota\":";
    j += ota ? 1 : 0;
    j += ",\"can_update\":";
    j += (sta && ota) ? 1 : 0;
    j += ",\"build\":\"" __DATE__ " " __TIME__ "\"";
    j += ",\"uptime_ms\":";
    j += String((unsigned long)millis());
    j += ",\"heap\":";
    j += String((unsigned)ESP.getFreeHeap());
    j += "}";
    server.send(200, "application/json", j);
  });

  server.on("/wifi/off", HTTP_GET, []() {
    // 只打标记：禁止在 HTTP 回调里 delay/stopAp（会复位）
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>Closing hotspot...</h2>"
          "<p style='color:#3dd68c'>After AP is off, open web via STA IP, "
          "e.g. http://192.168.199.170/</p>"
          "<p style='color:#f2c94c'>BT stack starts ~1.5s after AP is stable off. "
          "Wait a few more seconds before scan.</p>"
          "<p style='color:#8b9aab'>This page may disconnect in 1s.</p></body>");
    server.send(200, "text/html; charset=utf-8", body);
    if (gPortal) {
      if (gPortal->store_) gPortal->store_->saveWifiEnabled(false);
      gPortal->requestStopAp();  // loop 里再真正关
    }
    Serial.println("[WEB] SoftAP off requested (deferred to loop)");
  });

  server.on("/wifista", HTTP_GET, []() {
    if (!gPortal || !gPortal->store_) {
      server.send(500, "text/plain", "no store");
      return;
    }
    String ssid = server.arg("s");
    ssid.trim();
    String pass = server.arg("p");
    if (ssid.length() == 0) {
      server.send(400, "text/html; charset=utf-8",
                  F("<meta charset=utf-8><p>SSID 不能为空</p><p><a href=/>返回</a></p>"));
      return;
    }
    // 密码留空且已有保存 → 不覆盖（防误清）
    String oldPass = gPortal->store_->loadStaPass();
    if (pass.length() == 0 && gPortal->store_->loadStaSsid().length() > 0 &&
        gPortal->store_->loadStaSsid() == ssid) {
      pass = oldPass;
    }
    bool ok = gPortal->store_->saveSta(ssid, pass);
    Serial.println("[WEB] STA save ssid=" + ssid + " ok=" + String(ok ? 1 : 0) +
                   " pass_len=" + String(pass.length()));
    gPortal->startStaFromStore();
    String body =
        F("<!DOCTYPE html><meta charset=utf-8><meta name=viewport "
          "content='width=device-width,initial-scale=1'>"
          "<body style='font-family:system-ui;background:#0f1419;color:#e7ecf1;"
          "padding:24px;text-align:center'>"
          "<h2>家庭 Wi‑Fi 已保存</h2><p>SSID：<code>");
    body += ssid;
    body += F("</code></p><p style='color:#f2c94c'>正在连接… 约几秒后刷新首页看状态。"
              "连上后无线烧录主机名：</p><p><code>");
    body += gPortal->host_;
    body += F(".local</code></p>"
              "<p><a style='color:#2f80ed' href='/'>返回设置</a></p></body>");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.on("/wifista/clear", HTTP_GET, []() {
    if (gPortal && gPortal->store_) {
      gPortal->store_->clearSta();
    }
    if (gPortal) gPortal->stopSta();
    Serial.println("[WEB] STA cleared");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "ok");
  });

  server.on("/wifi/on", HTTP_GET, []() {
    if (gPortal && gPortal->store_) {
      gPortal->store_->saveWifiEnabled(true);
    }
    bool ok = gPortal ? gPortal->startAp() : false;
    if (gPortal && ok) gPortal->startStaFromStore();
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

  server.on("/scan/start", HTTP_GET, []() {
    if (!gPortal || !gPortal->bt_) {
      server.send(500, "application/json", "{\"error\":\"no_bt_ptr\"}");
      return;
    }
    if (gPortal->trackMode() != TRACK_MODE_CLASSIC) {
      server.send(200, "application/json",
                  "{\"error\":\"not_classic\",\"msg\":\"当前是 BLE 模式，经典扫描已禁用\"}");
      return;
    }
    if (gPortal->apActive()) {
      server.send(200, "application/json",
                  "{\"error\":\"turn_off_ap_first\",\"msg\":\"先关热点再扫描\"}");
      return;
    }
    if (!gPortal->bt_->ready()) {
      server.send(200, "application/json",
                  "{\"error\":\"bt_not_ready\",\"msg\":\"热点关稳约2秒后再扫\"}");
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

  server.onNotFound([]() {
    String uri = server.uri();
    Serial.printf("[WEB] 404 %s from %s\n", uri.c_str(),
                  server.client().remoteIP().toString().c_str());
    // 任意域名（手机连 AP 后乱跳）都导到配置页
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "redirect http://192.168.4.1/");
  });
}

void WebPortal::begin(ConfigStore* store, BleTracker* bt, DoorFsm* door,
                      BleScanTool* ble, NfcReader* nfc, bool enableAp) {
  gPortal = this;
  store_ = store;
  bt_ = bt;
  door_ = door;
  ble_ = ble;
  nfc_ = nfc;

  // 加载跟踪模式
  if (store_) {
    trackMode_ = store_->loadTrackMode(TRACK_MODE_DEFAULT);
  }

  // 二选一：仅 BLE 模式开 IRK 跟踪；经典模式绝不碰 BLE scan
  if (ble_) {
    if (trackMode_ == TRACK_MODE_BLE && gBleBond.hasIrk()) {
      ble_->setTrack(true);
      Serial.println("[WEB] BLE IRK track ON (paired phone)");
    } else {
      ble_->setTrack(false);
      if (trackMode_ == TRACK_MODE_CLASSIC) {
        Serial.println("[WEB] classic mode — BLE track forced OFF");
      }
    }
  }

  uint64_t chipid = ESP.getEfuseMac();
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%04X", (uint16_t)(chipid & 0xFFFF));
  apSsid_ = String(AP_SSID_PREFIX) + suffix;
  // mDNS/OTA：小写 hostname，避免和 SoftAP SSID 混淆
  {
    String hs(suffix);
    hs.toLowerCase();
    host_ = String("garage-") + hs;
  }

  if (store_) {
    staWanted_ = store_->loadStaSsid().length() > 0;
  }

  setupRoutes();
  serverStarted_ = false;

  // 先 SoftAP，再 HTTP。顺序反了会 assert tcpip_send_msg_wait_sem
  if (enableAp) {
    startAp();
  } else {
    // 关热点（wifi_on=0）：不 startAp、不自动回退开热点
    // STA 延后：由 main 先 initBtStacks，再 startStaFromStore
    // （STA+SerialBT/BLE 同时起会 SW_CPU_RESET）
    Serial.println("[WEB] SoftAP off (wifi_on=0). STA deferred until BT ready.");
    apActive_ = false;
    if (bt_) bt_->setInquiryPaused(false);
    if (!staWanted_) {
      Serial.println("[WEB] No STA config. Hold BOOT 3s or send 'wifi on' to open AP once.");
    }
    return;
  }

  if (staWanted_) startStaFromStore();
}

void WebPortal::startStaFromStore() {
  if (!store_) return;
  String ssid = store_->loadStaSsid();
  String pass = store_->loadStaPass();
  ssid.trim();
  if (ssid.length() == 0) {
    stopSta();
    return;
  }
  staWanted_ = true;

  // SoftAP 已开 → APSTA；否则纯 STA
  WiFi.persistent(false);
  if (apActive_) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.setAutoReconnect(true);
  // BT+WiFi 同开必须允许 modem sleep；setSleep(false) 会直接 SW_CPU_RESET
  // （wifi: Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled）
  WiFi.setSleep(true);

  // mDNS：BT 已在跑时跳过（ESPmDNS+SerialBT 易崩），OTA 用 IP 即可
  if (!mdnsOn_ && !/* placeholder */ false) {
    // 由 main 在无 BT 或确认安全时再开；此处仅在未起 BT 时尝试
  }
  // 明确：STA 模式下暂不开 mDNS，避免 BT+WiFi+mDNS 三栈崩溃
  if (!mdnsOn_) {
    Serial.println("[WEB] skip MDNS (BT may be active); use STA IP for web/OTA");
  }

  Serial.println("[WEB] STA begin ssid=" + ssid + " hostname=" + host_ + ".local");
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("[WEB] WiFi.begin called");
  staTrying_ = true;
  staNextRetryMs_ = millis() + 15000;
}

void WebPortal::stopSta() {
  staWanted_ = false;
  staTrying_ = false;
  WiFi.disconnect(false, false);
  if (mdnsOn_) {
    MDNS.end();
    mdnsOn_ = false;
  }
  if (apActive_) {
    WiFi.mode(WIFI_AP);
  } else {
    WiFi.mode(WIFI_OFF);
  }
  Serial.println("[WEB] STA stopped");
}

void WebPortal::ensureHttpIfSta() {
  if (apActive_) return;
  if (!staWanted_ || !staConnected()) return;
  if (serverStarted_) return;
  server.begin();
  server.enableDelay(true);
  serverStarted_ = true;
  Serial.println("[WEB] STA HTTP up at http://" + staIp() + "/");
}

void WebPortal::loopSta() {
  if (!staWanted_) return;

  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    ensureHttpIfSta();
    // 每 15s 打一次 STA/HTTP/heap：纯 STA 路径没有 AP 诊断日志，挂了要能看见
    if (staTrying_ || millis() - staLastLogMs_ > 15000) {
      staLastLogMs_ = millis();
      if (staTrying_) {
        staTrying_ = false;
        Serial.println("[WEB] STA connected ip=" + WiFi.localIP().toString() +
                       " host=" + host_ + ".local http=" +
                       String(serverStarted_ ? "up" : "down"));
      } else if (Serial.availableForWrite() > 160) {
        Serial.printf("[WEB] sta=up ip=%s http=%s heap=%u maxblk=%u\n",
                      WiFi.localIP().toString().c_str(),
                      serverStarted_ ? "up" : "down",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap());
      }
    }
    return;
  }

  // STA 掉线：纯 STA 下监听 socket 会失效，必须清标志，重连后再 begin
  if (!apActive_ && serverStarted_) {
    server.stop();
    serverStarted_ = false;
    Serial.println("[WEB] STA lost → HTTP server stopped (will re-begin on reconnect)");
  }
  // 断线重连期最多 5s 打一行，避免刷屏占满串口拖死 loop
  {
    static uint32_t lastDownLog = 0;
    if (millis() - lastDownLog >= 5000 && Serial.availableForWrite() > 96) {
      lastDownLog = millis();
      Serial.printf("[WEB] sta down st=%d retry_in=%ums http=%s\n", (int)st,
                    (unsigned)(staNextRetryMs_ > millis()
                                   ? staNextRetryMs_ - millis()
                                   : 0),
                    serverStarted_ ? "up" : "down");
    }
  }

  // 未连上：节流重连（WiFi.begin 会有点重，勿每 loop 调）
  uint32_t now = millis();
  if (!millisBefore(now, staNextRetryMs_)) {
    if (apActive_ && WiFi.getMode() != WIFI_AP_STA) {
      WiFi.mode(WIFI_AP_STA);
    }
    String ssid = store_ ? store_->loadStaSsid() : String();
    String pass = store_ ? store_->loadStaPass() : String();
    if (ssid.length()) {
      Serial.printf("[WEB] STA retry ssid=%s st=%d\n", ssid.c_str(), (int)st);
      WiFi.begin(ssid.c_str(), pass.c_str());
      staTrying_ = true;
      staNextRetryMs_ = now + 20000;
    }
  }
}

bool WebPortal::startAp() {
  WiFi.persistent(false);
  // 不要先 WIFI_OFF：部分模组 OFF→AP 后 Beacon 异常（SSID 时有时无/扫不到）
  // 已配家庭 Wi‑Fi 则 APSTA，保住 STA/OTA
  if (staWanted_) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }
  WiFi.setSleep(false);
  delay(300);

  IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), sn(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, gw, sn)) {
    Serial.println("[WEB] softAPConfig FAIL");
  }
  // 固定 2.4G 信道 6（避开部分拥挤的 1），ssid_hidden=0, max_conn=4
  bool ok = WiFi.softAP(apSsid_.c_str(), AP_PASSWORD, 6, 0, 4);
  delay(500);
  IPAddress got = WiFi.softAPIP();
  if (ok && got == IPAddress(0, 0, 0, 0)) {
    Serial.println("[WEB] softAPIP=0 → 重配 + 重试 softAP");
    WiFi.softAPConfig(ip, gw, sn);
    ok = WiFi.softAP(apSsid_.c_str(), AP_PASSWORD, 6, 0, 4);
    delay(500);
    got = WiFi.softAPIP();
  }
  apActive_ = ok && (got != IPAddress(0, 0, 0, 0));
  if (apActive_) {
    server.begin();
    server.enableDelay(true);
    serverStarted_ = true;
    // 强制门户 DNS：* → 192.168.4.1，手机系统探测/连网检测才会落到本机 HTTP
    dns_.setErrorReplyCode(DNSReplyCode::NoError);
    dnsOn_ = dns_.start(53, "*", ip);
    Serial.printf("[WEB] HTTP :80 listening, AP IP=%s dns53=%d\n",
                  got.toString().c_str(), (int)dnsOn_);
  } else {
    Serial.printf("[WEB] SoftAP bring-up FAIL ok=%d ip=%s\n", (int)ok,
                  got.toString().c_str());
  }
#if WIFI_AP_YIELD_BT
  if (apActive_ && bt_) {
    apQuietUntilMs_ = millis() + WIFI_AP_BOOT_QUIET_MS;
    bt_->setInquiryPaused(true);
    bt_->setInquirySlow(true);
    bt_->cancelActiveInquiry();
  }
#endif
  Serial.printf("[WEB] SoftAP %s pass=%s -> %s ip=%s mode=%d sta=%d apmac=%s\n",
                apSsid_.c_str(), AP_PASSWORD, apActive_ ? "OK" : "FAIL",
                WiFi.softAPIP().toString().c_str(), (int)WiFi.getMode(),
                WiFi.softAPgetStationNum(), WiFi.softAPmacAddress().c_str());
  Serial.println("[WEB] 手机请连接 2.4G 热点: " + apSsid_ + " / " + AP_PASSWORD +
                 "  然后浏览器打开 http://192.168.4.1/");
  return apActive_;
}

void WebPortal::stopAp() {
  if (dnsOn_) {
    dns_.stop();
    dnsOn_ = false;
  }
  // WiFi OFF 会拆掉已 listen 的 socket：必须 stop + 清标志，
  // 否则 ensureHttpIfSta 因 serverStarted_==true 直接 return，STA 起来后网页永远打不开
  if (serverStarted_) {
    server.stop();
    serverStarted_ = false;
  }
  // 彻底关 SoftAP：disconnect → OFF → 再按需开纯 STA，避免模式残留再 beacon
  WiFi.softAPdisconnect(true);
  apActive_ = false;
  apQuietUntilMs_ = 0;
  delay(50);
  WiFi.mode(WIFI_OFF);
  delay(120);
  if (staWanted_) {
    WiFi.mode(WIFI_STA);
    delay(50);
    startStaFromStore();
  }
  if (bt_) {
    bt_->setInquiryPaused(false);
    bt_->setInquirySlow(false);
  }
  if (nfc_) nfc_->kickRecover();
  Serial.println("[WEB] SoftAP off hard mode=" + String((int)WiFi.getMode()) +
                 " sta=" + String(staWanted_ ? "keep" : "none") +
                 " sta_ip=" + staIp());
}

bool WebPortal::rfQuietActive() const {
  return apActive_ && apQuietUntilMs_ != 0 &&
         !millisReached(millis(), apQuietUntilMs_);
}

void WebPortal::loop() {
  // 模式切换后延时重启：让 HTTP 302 先发完，再干净地只起一侧 BT 栈
  if (modeRebootPending_ &&
      (int32_t)(millis() - modeRebootAtMs_) >= 0) {
    modeRebootPending_ = false;
    Serial.println("[WEB] reboot for exclusive classic/BLE stack...");
    delay(100);
    ESP.restart();
  }

  // 延迟执行关热点：与 HTTP 回调解耦，避免复位
  if (stopApPending_) {
    stopApPending_ = false;
    Serial.println("[WEB] deferred stopAp begin");
    stopAp();  // 保留 STA + server
    Serial.println("[WEB] deferred stopAp done (STA kept)");
  }

  loopSta();

  // 纯 STA（无热点）也要跑 HTTP：多打几次 handleClient，避免被远程 TLS/NFC 饿死
  if (apActive_ || (staWanted_ && staConnected())) {
    if (serverStarted_) {
      server.handleClient();
      server.handleClient();
      yield();
      server.handleClient();
    }
  }
  if (!apActive_) {
    // 纯 STA 路径：不做 SoftAP 特有维护
    return;
  }

  if (dnsOn_) dns_.processNextRequest();
  server.handleClient();
  server.handleClient();
  yield();
  server.handleClient();

  static uint32_t lastDiag = 0;
  if (millis() - lastDiag >= 3000) {
    lastDiag = millis();
    if (Serial.availableForWrite() > 128) {
      Serial.printf("[WEB] ap=%s ip=%s sta=%s stations=%d heap=%u bt_delayed=%d\n",
                    apSsid_.c_str(), WiFi.softAPIP().toString().c_str(),
                    staIp().c_str(), WiFi.softAPgetStationNum(),
                    (unsigned)ESP.getFreeHeap(), gBleBond.hasIrk() ? 1 : 0);
    }
  }

  // AP IP 丢失或为 0：整段重启 SoftAP + HTTP（只补 softAP 不够）
  static uint32_t lastReassert = 0;
  if (millis() - lastReassert >= 5000) {
    lastReassert = millis();
    IPAddress ip = WiFi.softAPIP();
    if (ip == IPAddress(0, 0, 0, 0)) {
      Serial.println("[WEB] SoftAP IP=0 → 重启 AP+HTTP");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_AP);
      WiFi.setSleep(false);
      delay(50);
      IPAddress a(192, 168, 4, 1), g(192, 168, 4, 1), s(255, 255, 255, 0);
      WiFi.softAPConfig(a, g, s);
      WiFi.softAP(apSsid_.c_str(), AP_PASSWORD);
      delay(200);
      server.begin();
      Serial.printf("[WEB] softAP retry -> %s\n",
                    WiFi.softAPIP().toString().c_str());
    }
  }

#if WIFI_AP_YIELD_BT
  if (bt_) {
    if (rfQuietActive()) {
      // 关联关键期：Inquiry 全停，避免 Beacon/关联帧被挤掉
      bt_->setInquiryPaused(true);
      bt_->setInquirySlow(true);
      bt_->cancelActiveInquiry();
    } else {
      if (apQuietUntilMs_ != 0) {
        apQuietUntilMs_ = 0;
        bt_->setInquiryPaused(false);
        Serial.println("[WEB] RF 静默窗口结束；热点仍开 → Inquiry 保持慢速");
      }
      // 只要热点开着就慢速：关联完成前 stationNum 常为 0，不能恢复全速
      bt_->setInquirySlow(true);
    }

    int clients = WiFi.softAPgetStationNum();
    static int lastC = -1;
    if (clients != lastC) {
      lastC = clients;
      Serial.printf("[WEB] softAP clients=%d quiet=%d\n", clients,
                    (int)rfQuietActive());
      if (clients > 0) {
        bt_->cancelActiveInquiry();
      } else if (nfc_ && !nfc_->ok()) {
        nfc_->kickRecover();  // 手机离开热点后立刻补 NFC init
      }
    }
  }
#else
  if (bt_) {
    int clients = WiFi.softAPgetStationNum();
    static int lastC = -1;
    if (clients != lastC) {
      lastC = clients;
      Serial.printf("[WEB] softAP clients=%d\n", clients);
      if (clients > 0) {
        bt_->cancelActiveInquiry();
        bt_->setInquirySlow(true);
      } else {
        bt_->setInquirySlow(false);
      }
    }
  }
#endif
}

void WebPortal::setTrackMode(int mode) {
  if (mode != TRACK_MODE_BLE && mode != TRACK_MODE_CLASSIC) return;
  const bool changed = (mode != trackMode_);
  trackMode_ = mode;
  if (store_) store_->saveTrackMode(mode);

  const bool classic = (mode == TRACK_MODE_CLASSIC);

  // 二选一：立刻关掉另一侧的跟踪/配对（重启前也不并跑）
  if (classic) {
    if (ble_) ble_->setTrack(false);
    gBleBond.closePairingWindow("mode classic");
    if (bt_) {
      bt_->setAutoTrack(true);
      bt_->setInquiryPaused(false);
    }
    if (store_) store_->saveAutoTrack(true);
    Serial.println("[WEB] classic track ON; BLE scan/pair forced OFF");
  } else {
    if (bt_) {
      bt_->setAutoTrack(false);
      bt_->setInquiryPaused(false);
      bt_->cancelActiveInquiry();
    }
    if (store_) store_->saveAutoTrack(false);
    if (ble_ && gBleBond.hasIrk()) {
      ble_->setTrack(true);
      Serial.println("[WEB] BLE track ON (paired phone)");
    } else if (ble_) {
      ble_->setTrack(false);
      Serial.println("[WEB] BLE mode but no IRK — track stays OFF (pair first)");
    }
  }

  Serial.println("[WEB] track mode -> " + String(classic ? "Classic" : "BLE") +
                 " autotrack=" + String((bt_ && bt_->autoTrack()) ? "ON" : "OFF") +
                 " ble_track=" + String((ble_ && ble_->trackOn()) ? "ON" : "OFF"));

  // 已起的栈无法热卸载：换模式后约 1.2s 重启，开机只 init 选中的一侧
  if (changed) {
    modeRebootPending_ = true;
    modeRebootAtMs_ = millis() + 1200;
    Serial.println("[WEB] track mode changed → reboot in ~1.2s for exclusive BT stack");
  }
}
