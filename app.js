(function () {
  var TOKEN_KEY = "garage_token";
  var $ = function (id) { return document.getElementById(id); };
  var currentId = null;
  var pollTimer = null;

  function token() { return sessionStorage.getItem(TOKEN_KEY) || ""; }

  function setTip(el, text, cls) {
    if (!el) return;
    el.textContent = text || "";
    el.className = "tip" + (cls ? " " + cls : "");
  }

  function show(view) {
    $("loginView").classList.toggle("hidden", view !== "login");
    $("listView").classList.toggle("hidden", view !== "list");
    $("detailView").classList.toggle("hidden", view !== "detail");
    $("logoutBtn").classList.toggle("hidden", view === "login");
  }

  function api(path, opts) {
    opts = opts || {};
    var headers = opts.headers || {};
    if (!opts.raw) headers["Content-Type"] = "application/json";
    headers["X-Garage-Token"] = token();
    return fetch(path, {
      method: opts.method || "GET",
      headers: headers,
      body: opts.body
    }).then(function (r) {
      return r.json().then(function (j) {
        if (!r.ok) throw new Error(j.error || j.message || ("HTTP " + r.status));
        return j;
      });
    });
  }

  function ago(sec) {
    if (sec < 0) return "从未";
    if (sec < 5) return "刚刚";
    if (sec < 60) return sec + " 秒前";
    if (sec < 3600) return Math.floor(sec / 60) + " 分钟前";
    return Math.floor(sec / 3600) + " 小时前";
  }

  function pill(label, state) {
    var cls = state === "ok" ? "ok" : (state === "warn" ? "warn" : (state === "err" ? "err" : ""));
    return '<span class="pill ' + cls + '"><span class="dot"></span>' + label + "</span>";
  }

  function healthPills(h) {
    if (!h) return "";
    return (
      pill("NFC " + h.nfc, h.nfc === "ok" ? "ok" : (h.nfc === "nochip" ? "err" : "warn")) +
      pill("Web " + h.web, h.web === "ok" ? "ok" : "warn") +
      pill("RF " + h.rf, h.rf === "ok" ? "ok" : "warn") +
      pill("STA " + h.sta, h.sta === "ok" ? "ok" : "warn")
    );
  }

  function login() {
    var pw = $("pw").value.trim();
    if (!pw) {
      setTip($("loginTip"), "请输入密码", "err");
      return;
    }
    $("loginBtn").disabled = true;
    api("/api/login", { method: "POST", body: JSON.stringify({ password: pw }) })
      .then(function (j) {
        sessionStorage.setItem(TOKEN_KEY, j.token || "");
        $("pw").value = "";
        setTip($("loginTip"), "就绪", "ok");
        $("statusText").textContent = "已登录";
        loadList();
      })
      .catch(function (e) {
        setTip($("loginTip"), e.message || "登录失败", "err");
      })
      .finally(function () {
        $("loginBtn").disabled = false;
      });
  }

  function loadList() {
    show("list");
    setTip($("listTip"), "加载中…");
    api("/api/devices")
      .then(function (j) {
        var list = j.devices || [];
        var ota = j.ota || {};
        $("otaCurrent").textContent = ota.version
          ? "服务器固件：" + ota.version + " · " + (ota.size || 0) + " bytes"
          : "服务器尚未上传固件包";
        var box = $("deviceList");
        if (!list.length) {
          box.innerHTML = '<div class="empty">暂无设备上报。设备上线后会自动出现。</div>';
          setTip($("listTip"), "");
          return;
        }
        box.innerHTML = list.map(function (d) {
          return (
            '<div class="device-card" data-id="' + d.id + '">' +
              '<div class="card-top">' +
                '<div class="name">' + (d.name || d.id) + "</div>" +
                '<button class="btn ghost sm del-btn" type="button" data-del="' + d.id + '">删除</button>' +
              "</div>" +
              '<div class="id">' + d.id + " · " + (d.role || "-") + "</div>" +
              '<div class="pill-row" style="margin-bottom:10px">' +
                (d.online ? pill("在线", "ok") : pill("离线", "err")) +
                healthPills(d.health) +
              "</div>" +
              '<div class="fw">fw ' + (d.fw || "-") + "</div>" +
              '<div class="fw" style="color:var(--muted)">最后在线 ' + ago(d.last_seen_ago_s) + "</div>" +
            "</div>"
          );
        }).join("");
        Array.prototype.forEach.call(box.querySelectorAll(".device-card"), function (el) {
          el.addEventListener("click", function () {
            openDetail(el.getAttribute("data-id"));
          });
        });
        Array.prototype.forEach.call(box.querySelectorAll(".del-btn"), function (btn) {
          btn.addEventListener("click", function (e) {
            e.stopPropagation();
            var id = btn.getAttribute("data-del");
            if (!window.confirm("删除设备 " + id + "？\n设备下次上报时会自动重新出现在列表中。")) return;
            btn.disabled = true;
            api("/api/devices/" + encodeURIComponent(id), { method: "DELETE" })
              .then(function () {
                setTip($("listTip"), "已删除 " + id, "ok");
                loadList();
              })
              .catch(function (err) {
                setTip($("listTip"), err.message || "删除失败", "err");
                btn.disabled = false;
              });
          });
        });
        setTip($("listTip"), "共 " + list.length + " 台");
      })
      .catch(function (e) {
        if ((e.message || "").indexOf("unauthorized") >= 0) {
          sessionStorage.removeItem(TOKEN_KEY);
          show("login");
          setTip($("loginTip"), "请重新登录", "err");
          return;
        }
        setTip($("listTip"), e.message || "加载失败", "err");
      });
  }

  function statusItem(k, v, cls) {
    return (
      '<div class="status-item"><div class="k">' + k + '</div><div class="v ' +
      (cls || "") + '">' + v + "</div></div>"
    );
  }

  function nfcState(nfc) {
    if (!nfc) return ["-", ""];
    if (nfc.ok) return ["正常", "ok"];
    if (nfc.absent) return ["无芯片", "err"];
    if (nfc.deferred) return ["延迟重试", "warn"];
    return ["等待", "warn"];
  }

  function rfState(rf) {
    if (!rf) return ["-", ""];
    var has = (rf.open || rf.close) ? "有码" : "无码";
    if (rf.tx_busy) return [has + " · 发射中", "warn"];
    return [has, (rf.open || rf.close) ? "ok" : "err"];
  }

  function renderDetail(j) {
    var d = j.device || {};
    var st = d.status || {};
    currentId = d.id;
    $("devTitle").textContent = (d.name || d.id) + " · " + d.id;
    $("devOnline").className = "pill " + (d.online ? "ok" : "err");
    $("devOnline").innerHTML = '<span class="dot"></span>' + (d.online ? "在线" : "离线");

    var nfc = nfcState(st.nfc);
    var rf = rfState(st.rf);
    var webS = st.web ? ["开", "ok"] : (st.web === 0 ? ["关", "warn"] : ["-", ""]);
    var staS = st.sta ? ["已连接", "ok"] : ["未连接", "warn"];
    var modeS = st.mode === 1 ? "经典" : (st.mode === 0 ? "BLE" : "-");
    var pairS = st.pair
      ? (st.pair.open ? "配对窗口开" : (st.pair.pin ? "已设PIN" : "关"))
      : "-";

    $("statusGrid").innerHTML = [
      statusItem("NFC", nfc[0], nfc[1]),
      statusItem("本地网页", webS[0], webS[1]),
      statusItem("射频 RF", rf[0], rf[1]),
      statusItem("WiFi STA", staS[0], staS[1]),
      statusItem("STA IP", st.sta_ip || "-", ""),
      statusItem("固件", d.fw || st.fw || "-", ""),
      statusItem("门状态", st.door === 1 ? "开" : (st.door === 2 ? "关" : "未知"), ""),
      statusItem("跟踪模式", modeS, ""),
      statusItem("配对", pairS, st.pair && st.pair.open ? "warn" : ""),
      statusItem("车机 RSSI", st.car_rssi != null ? st.car_rssi : "-", ""),
      statusItem("堆内存", st.heap != null ? st.heap : "-", ""),
      statusItem("最大块", st.maxblk != null ? st.maxblk : "-", ""),
      statusItem("WiFi RSSI", st.rssi != null ? st.rssi : "-", ""),
      statusItem("远程令", st.remote ? "开" : "关", st.remote ? "ok" : "warn")
    ].join("");

    // 配置表单回填
    if (st.mac) $("cfgMac").value = st.mac;
    if (st.mode === 0 || st.mode === 1) $("cfgMode").value = String(st.mode);

    $("devMeta").textContent =
      "id=" + d.id +
      " · role=" + (d.role || "-") +
      " · 最后在线 " + ago(d.last_seen_ago_s) +
      (st.uptime_ms != null ? " · uptime " + Math.floor(st.uptime_ms / 1000) + "s" : "");

    var o = j.ota || {};
    if (o.version) {
      $("ctrlTip").textContent =
        "服务器固件 " + o.version + (d.fw && d.fw !== o.version ? " · 设备可升级" : " · 已是最新");
    }
    loadLogs();
  }

  function openDetail(id) {
    currentId = id;
    show("detail");
    $("logBox").textContent = "加载中…";
    refreshDetail();
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(refreshDetail, 8000);
  }

  function refreshDetail() {
    if (!currentId) return;
    api("/api/devices/" + encodeURIComponent(currentId))
      .then(renderDetail)
      .catch(function (e) {
        setTip($("ctrlTip"), e.message || "加载失败", "err");
      });
  }

  function loadLogs() {
    if (!currentId) return;
    api("/api/devices/" + encodeURIComponent(currentId) + "/logs?lines=120")
      .then(function (j) {
        $("logBox").textContent = j.text || "（暂无日志）";
        $("logBox").scrollTop = $("logBox").scrollHeight;
        var now = new Date();
        var hh = ("0" + now.getHours()).slice(-2);
        var mm = ("0" + now.getMinutes()).slice(-2);
        var ss = ("0" + now.getSeconds()).slice(-2);
        $("logMeta").textContent =
          "日志日 " + (j.day || "-") + " · 刷新于 " + hh + ":" + mm + ":" + ss;
      })
      .catch(function () {
        $("logBox").textContent = "日志读取失败";
      });
  }

  function clearLogs() {
    if (!currentId) return;
    if (!window.confirm("清除该设备全部历史日志？此操作不可恢复。")) return;
    setTip($("logClearTip"), "清除中…");
    api("/api/devices/" + encodeURIComponent(currentId) + "/logs/clear", {
      method: "POST",
      body: "{}"
    })
      .then(function (j) {
        $("logBox").textContent = "（暂无日志）";
        $("logMeta").textContent = "已清除 " + (j.removed || 0) + " 个日志文件";
        setTip($("logClearTip"), "已清除", "ok");
      })
      .catch(function (e) {
        setTip($("logClearTip"), e.message || "清除失败", "err");
      });
  }

  function sendCmd(cmd) {
    if (!currentId) return;
    setTip($("ctrlTip"), "发送中…");
    api("/api/devices/" + encodeURIComponent(currentId) + "/" + cmd, {
      method: "POST",
      body: "{}"
    })
      .then(function (j) {
        setTip($("ctrlTip"), j.message || "已下发", "ok");
      })
      .catch(function (e) {
        setTip($("ctrlTip"), e.message || "失败", "err");
      });
  }

  // 通用配置指令（替代本地网页）：POST .../cmd {"cmd":"mac AA:BB:..."}
  function sendRawCmd(cmd) {
    if (!currentId) return Promise.reject(new Error("no device"));
    setTip($("cfgTip"), "下发：" + cmd);
    return api("/api/devices/" + encodeURIComponent(currentId) + "/cmd", {
      method: "POST",
      body: JSON.stringify({ cmd: cmd })
    })
      .then(function (j) {
        if (j.ok) {
          setTip($("cfgTip"), "已下发「" + cmd + "」，约 3 秒生效", "ok");
          setTimeout(refreshDetail, 4500);
        } else {
          setTip($("cfgTip"), "失败：" + (j.result || "unknown"), "err");
        }
        return j;
      })
      .catch(function (e) {
        setTip($("cfgTip"), e.message || "下发失败", "err");
        throw e;
      });
  }

  function cfgWire() {
    $("cfgMacBtn").addEventListener("click", function () {
      var m = ($("cfgMac").value || "").trim().toUpperCase();
      if (m.length !== 17) { setTip($("cfgTip"), "MAC 格式应为 AA:BB:CC:DD:EE:FF", "err"); return; }
      sendRawCmd("mac " + m);
    });
    $("cfgModeBtn").addEventListener("click", function () {
      var m = $("cfgMode").value;
      if (!window.confirm("切换跟踪模式会让设备约 1.2 秒后重启，继续？")) return;
      sendRawCmd("mode " + m);
    });
    $("cfgAutoOn").addEventListener("click", function () { sendRawCmd("autotrack on"); });
    $("cfgAutoOff").addEventListener("click", function () { sendRawCmd("autotrack off"); });
    $("cfgPairOn").addEventListener("click", function () { sendRawCmd("pair on"); });
    $("cfgPairOff").addEventListener("click", function () { sendRawCmd("pair off"); });
    $("cfgPinBtn").addEventListener("click", function () {
      var pin = ($("cfgPin").value || "").trim();
      if (pin && !/^\d{1,6}$/.test(pin)) { setTip($("cfgTip"), "PIN 应为 1-6 位数字", "err"); return; }
      sendRawCmd("pairpin " + pin).then(function () { $("cfgPin").value = ""; });
    });
    $("cfgWifiBtn").addEventListener("click", function () {
      var ssid = ($("cfgSsid").value || "").trim();
      var pass = $("cfgPass").value || "";
      if (!ssid) { setTip($("cfgTip"), "SSID 不能为空", "err"); return; }
      sendRawCmd("wifista " + ssid + (pass ? " " + pass : ""))
        .then(function () { $("cfgPass").value = ""; });
    });
    $("cfgNfcBtn").addEventListener("click", function () { sendRawCmd("nfcinit"); });
    $("cfgWebOff").addEventListener("click", function () {
      if (!window.confirm("关闭本地网页后，车库局域网内 80 端口不再响应（远程控制不受影响）。继续？")) return;
      sendRawCmd("web off");
    });
    $("cfgWebOn").addEventListener("click", function () { sendRawCmd("web on"); });
  }

  function uploadOta() {
    var file = ($("binInput").files || [])[0];
    if (!file) {
      setTip($("otaMsg"), "请选择 firmware.bin", "err");
      return;
    }
    var ver = $("verInput").value.trim();
    // 不带 notify：上传只落服务器，升级靠设备页「立即更新」
    var qs = "?notify=0" + (ver ? "&version=" + encodeURIComponent(ver) : "");
    $("uploadBtn").disabled = true;
    setTip($("otaMsg"), "上传中…");
    fetch("/api/ota/upload" + qs, {
      method: "POST",
      headers: {
        "X-Garage-Token": token(),
        "Content-Type": "application/octet-stream"
      },
      body: file
    })
      .then(function (r) {
        return r.json().then(function (j) {
          if (!r.ok) throw new Error(j.error || ("HTTP " + r.status));
          return j;
        });
      })
      .then(function (j) {
        var o = j.ota || {};
        setTip($("otaMsg"), "已上传 " + (o.version || "") + "，请到设备页点「立即更新」", "ok");
        $("otaCurrent").textContent = "服务器固件：" + (o.version || "-") + " · " + (o.size || 0) + " bytes";
        loadList();
      })
      .catch(function (e) {
        setTip($("otaMsg"), e.message || "上传失败", "err");
      })
      .finally(function () {
        $("uploadBtn").disabled = false;
      });
  }

  function backToList() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = null;
    currentId = null;
    loadList();
  }

  $("loginBtn").addEventListener("click", login);
  $("pw").addEventListener("keydown", function (e) {
    if (e.key === "Enter") login();
  });
  $("logoutBtn").addEventListener("click", function () {
    sessionStorage.removeItem(TOKEN_KEY);
    if (pollTimer) clearInterval(pollTimer);
    show("login");
    $("statusText").textContent = "未登录";
  });
  $("refreshBtn").addEventListener("click", loadList);
  $("backBtn").addEventListener("click", backToList);
  $("dRefreshBtn").addEventListener("click", refreshDetail);
  $("logRefreshBtn").addEventListener("click", loadLogs);
  $("logClearBtn").addEventListener("click", clearLogs);
  $("openBtn").addEventListener("click", function () { sendCmd("open"); });
  $("closeBtn").addEventListener("click", function () { sendCmd("close"); });
  $("updateBtn").addEventListener("click", function () { sendCmd("update"); });
  $("otaBtn").addEventListener("click", function () {
    $("otaBar").classList.toggle("hidden");
  });
  $("uploadBtn").addEventListener("click", uploadOta);
  cfgWire();

  function boot() {
    // 调试期服务端可关闭登录：/api/auth required=false 时直接进列表
    fetch("/api/auth")
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (j && j.required === false) {
          $("statusText").textContent = "调试模式 · 无鉴权";
          $("logoutBtn").classList.add("hidden");
          loadList();
          return;
        }
        if (token()) {
          $("statusText").textContent = "已登录";
          loadList();
        } else {
          show("login");
        }
      })
      .catch(function () {
        if (token()) {
          loadList();
        } else {
          show("login");
        }
      });
  }

  boot();
})();
