(function () {
  var TOKEN_KEY = "garage_token";
  var $ = function (id) { return document.getElementById(id); };
  var currentId = null;
  var pollTimer = null;
  var busyCmd = false;

  function token() { return sessionStorage.getItem(TOKEN_KEY) || ""; }

  function setTip(el, text, cls) {
    if (!el) return;
    el.textContent = text || "";
    el.className = "tip" + (cls ? " " + cls : "");
  }

  // 浮层状态反馈：所有远程操作的成功/失败都走这里
  function notify(text, type, ttl) {
    var box = $("toastBox");
    if (!box || !text) return;
    var el = document.createElement("div");
    el.className = "toast " + (type || "info");
    el.textContent = text;
    box.appendChild(el);
    while (box.children.length > 3) box.removeChild(box.firstChild);
    var life = ttl || (type === "err" ? 4200 : 3000);
    setTimeout(function () {
      el.classList.add("hide");
      setTimeout(function () {
        if (el.parentNode) el.parentNode.removeChild(el);
      }, 220);
    }, life);
  }

  function notifyOk(text, ttl) {
    notify(text, "ok", ttl);
  }
  function notifyErr(text, ttl) {
    notify(text, "err", ttl || 4500);
  }
  function notifyWarn(text, ttl) {
    notify(text, "warn", ttl || 4000);
  }
  function notifyInfo(text, ttl) {
    notify(text, "info", ttl || 2500);
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
      notifyErr("请输入密码");
      return;
    }
    $("loginBtn").disabled = true;
    notifyInfo("登录中…");
    api("/api/login", { method: "POST", body: JSON.stringify({ password: pw }) })
      .then(function (j) {
        sessionStorage.setItem(TOKEN_KEY, j.token || "");
        $("pw").value = "";
        setTip($("loginTip"), "登录成功", "ok");
        notifyOk("登录成功");
        $("statusText").textContent = "已登录";
        loadList();
      })
      .catch(function (e) {
        setTip($("loginTip"), e.message || "登录失败", "err");
        notifyErr("登录失败：" + (e.message || "未知错误"));
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
              '<div class="quick-row">' +
                '<button class="btn open quick-btn" type="button" data-id="' + d.id + '" data-cmd="open">开门</button>' +
                '<button class="btn close quick-btn" type="button" data-id="' + d.id + '" data-cmd="close">关门</button>' +
              "</div>" +
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
            notifyInfo("删除设备 " + id + " …");
            api("/api/devices/" + encodeURIComponent(id), { method: "DELETE" })
              .then(function () {
                setTip($("listTip"), "已删除 " + id, "ok");
                notifyOk("设备已删除：" + id);
                loadList();
              })
              .catch(function (err) {
                setTip($("listTip"), err.message || "删除失败", "err");
                notifyErr("删除失败：" + (err.message || id));
                btn.disabled = false;
              });
          });
        });
        // 列表页快捷开/关门：不进详情页直接下发指令
        Array.prototype.forEach.call(box.querySelectorAll(".quick-btn"), function (btn) {
          btn.addEventListener("click", function (e) {
            e.stopPropagation();
            var id = btn.getAttribute("data-id");
            var cmd = btn.getAttribute("data-cmd");
            var label = cmd === "close" ? "关门" : "开门";
            btn.disabled = true;
            setTip($("listTip"), "发送" + label + "指令…");
            notifyInfo(label + "指令下发中…");
            api("/api/devices/" + encodeURIComponent(id) + "/" + cmd, {
              method: "POST",
              body: "{}"
            })
              .then(function (j) {
                var msg = j.message || (label + "指令已下发");
                setTip($("listTip"), msg + " · " + id, j.ok === 0 ? "err" : "ok");
                if (j.ok === 0) {
                  notifyErr(label + "失败：" + msg + " · " + id);
                } else {
                  notifyOk(label + "成功 · " + msg + " · " + id);
                }
              })
              .catch(function (err) {
                var msg = err.message || (label + "失败");
                setTip($("listTip"), msg + " · " + id, "err");
                notifyErr(label + "失败：" + msg + " · " + id);
              })
              .finally(function () {
                btn.disabled = false;
              });
          });
        });
        setTip($("listTip"), "共 " + list.length + " 台 · 可直接点开关门");
      })
      .catch(function (e) {
        if ((e.message || "").indexOf("unauthorized") >= 0) {
          sessionStorage.removeItem(TOKEN_KEY);
          show("login");
          setTip($("loginTip"), "请重新登录", "err");
          notifyErr("会话失效，请重新登录");
          return;
        }
        setTip($("listTip"), e.message || "加载失败", "err");
        notifyErr("设备列表加载失败：" + (e.message || "未知错误"));
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
        notifyWarn("设备状态刷新失败：" + (e.message || ""));
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
    notifyInfo("正在清除设备日志…");
    api("/api/devices/" + encodeURIComponent(currentId) + "/logs/clear", {
      method: "POST",
      body: "{}"
    })
      .then(function (j) {
        $("logBox").textContent = "（暂无日志）";
        $("logMeta").textContent = "已清除 " + (j.removed || 0) + " 个日志文件";
        setTip($("logClearTip"), "已清除", "ok");
        notifyOk("日志清除成功");
      })
      .catch(function (e) {
        setTip($("logClearTip"), e.message || "清除失败", "err");
        notifyErr("日志清除失败：" + (e.message || ""));
      });
  }

  function cmdLabel(cmd) {
    if (cmd === "open") return "开门";
    if (cmd === "close") return "关门";
    if (cmd === "update") return "更新固件";
    return cmd;
  }

  function fetchDevice(id) {
    return api("/api/devices/" + encodeURIComponent(id || currentId));
  }

  function sleep(ms) {
    return new Promise(function (resolve) { setTimeout(resolve, ms); });
  }

  function doorText(st) {
    if (st.door === 1) return "开";
    if (st.door === 2) return "关";
    return "未知";
  }

  // 下发后轮询设备，确认门状态真正变化 / 固件版本变化
  function waitDoorOutcome(id, expectedDoor, timeoutMs) {
    var label = expectedDoor === 1 ? "开门" : "关门";
    var start = Date.now();
    var pendingName = expectedDoor === 1 ? "open" : "close";

    function tick() {
      return fetchDevice(id).then(function (j) {
        var d = j.device || {};
        var st = d.status || {};
        if (st.door === expectedDoor) {
          renderDetail(j);
          setTip($("ctrlTip"), label + "成功 · 门状态：" + doorText(st), "ok");
          notifyOk(label + "成功");
          return;
        }
        if (Date.now() - start < timeoutMs) {
          return sleep(2500).then(tick);
        }
        renderDetail(j);
        if (d.online === false) {
          setTip($("ctrlTip"), label + "失败：设备离线", "err");
          notifyErr(label + "失败：设备离线，请检查设备供电与网络");
        } else if (st.pending === pendingName) {
          setTip($("ctrlTip"), label + "失败：设备尚未领取指令", "err");
          notifyErr(label + "失败：设备尚未领取指令（可能超过 8 秒 TTL）");
        } else {
          // 指令可能已被领取，但门状态未变化（无门磁、状态未更新）
          setTip($("ctrlTip"), label + "指令已下发，门状态未变化，请稍后刷新", "warn");
          notifyWarn(label + "指令已下发，但门状态未变化，请稍后刷新确认");
        }
        throw new Error(label + " confirm timeout");
      });
    }

    return tick();
  }

  function waitUpdateOutcome(id, serverFw, timeoutMs) {
    var start = Date.now();
    var firstFw = null;

    function tick() {
      return fetchDevice(id).then(function (j) {
        var d = j.device || {};
        var st = d.status || {};
        if (firstFw == null) firstFw = d.fw || null;
        var nowFw = d.fw || st.fw || null;
        if (serverFw && nowFw && nowFw === serverFw) {
          renderDetail(j);
          setTip($("ctrlTip"), "固件更新成功 · " + nowFw, "ok");
          notifyOk("固件更新成功 · " + nowFw);
          return;
        }
        if (nowFw && firstFw && nowFw !== firstFw && serverFw && nowFw !== serverFw) {
          renderDetail(j);
          setTip($("ctrlTip"), "固件版本已变化：" + nowFw, "ok");
          notifyOk("固件更新成功 · " + nowFw);
          return;
        }
        if (st.update_sticky || st.pending === "update") {
          setTip($("ctrlTip"), "正在更新固件…" + (nowFw ? " 当前 " + nowFw : ""), "ok");
          notifyInfo("正在更新固件，请耐心等待…", 2000);
        }
        if (Date.now() - start < timeoutMs) {
          return sleep(4000).then(tick);
        }
        renderDetail(j);
        if (d.online === false) {
          setTip($("ctrlTip"), "更新失败：设备离线", "err");
          notifyErr("固件更新失败：设备离线，请检查设备网络");
        } else if (st.update_sticky || st.pending === "update") {
          setTip($("ctrlTip"), "更新仍在进行，未确认完成", "warn");
          notifyWarn("更新仍在进行，尚未确认完成，请稍后在设备页刷新固件版本");
        } else {
          setTip($("ctrlTip"), "更新结果未确认：设备固件 " + (nowFw || "-"), "warn");
          notifyWarn("更新结果未确认，请稍后刷新查看固件版本");
        }
        throw new Error("update confirm timeout");
      });
    }

    return tick();
  }

  // 详情页：开门 / 关门 / 立即更新（带状态反馈 + 结果确认）
  function sendCmd(cmd) {
    if (!currentId) {
      notifyErr("未选择设备");
      return Promise.resolve();
    }
    if (busyCmd) {
      notifyWarn("有指令正在处理，请稍候");
      return Promise.resolve();
    }
    var id = currentId;
    var label = cmdLabel(cmd);
    var btn = cmd === "open" ? $("openBtn") : (cmd === "close" ? $("closeBtn") : $("updateBtn"));

    busyCmd = true;
    if (btn) btn.disabled = true;

    var queuedMsg = label + "指令已下发，设备领取中…";
    setTip($("ctrlTip"), queuedMsg, "ok");
    notifyInfo(queuedMsg);

    return api("/api/devices/" + encodeURIComponent(id) + "/" + cmd, {
      method: "POST",
      body: "{}"
    })
      .then(function (j) {
        var msg = j.message || (label + "已请求");
        if (j.ok === 0) {
          setTip($("ctrlTip"), msg, "err");
          notifyErr(label + "失败：" + msg);
          throw new Error(msg);
        }

        if (cmd === "open" || cmd === "close") {
          var expected = cmd === "open" ? 1 : 2;
          setTip($("ctrlTip"), label + "已下发 · 等待设备执行…", "ok");
          notifyInfo(label + "已下发，等待设备执行…");
          return waitDoorOutcome(id, expected, 20000).catch(function () {
            // waitDoorOutcome 内部已提示
          });
        }

        if (cmd === "update") {
          var serverFw = null;
          return fetchDevice(id).then(function (d) {
            serverFw = (j.ota && j.ota.version) || (d.ota && d.ota.version) || null;
          }).catch(function () {}).then(function () {
            setTip($("ctrlTip"), "更新指令已下发，设备开始下载固件…", "ok");
            notifyOk("更新指令已下发，设备开始下载固件…（约 1–2 分钟）", 4500);
            return waitUpdateOutcome(id, serverFw, 150000).catch(function () {});
          });
        }

        setTip($("ctrlTip"), msg, "ok");
        notifyOk(msg);
      })
      .catch(function (e) {
        // 已在上方提示过的不再重复
        if ((e.message || "").indexOf("confirm timeout") >= 0) {
          return;
        }
        if ((e.message || "").indexOf("已下发") >= 0) {
          return;
        }
        setTip($("ctrlTip"), e.message || (label + "失败"), "err");
        notifyErr(label + "失败：" + (e.message || "未知错误"));
      })
      .finally(function () {
        busyCmd = false;
        if (btn) btn.disabled = false;
      });
  }

  // 通用配置指令（替代本地网页）：POST .../cmd {"cmd":"mac AA:BB:..."}
  function humanCmd(cmd) {
    var map = {
      "autotrack on": "开启经典自动跟踪",
      "autotrack off": "关闭经典自动跟踪",
      "pair on": "打开 BLE 配对窗口 90 秒",
      "pair off": "关闭 BLE 配对",
      "nfcinit": "重新初始化 NFC",
      "web off": "关闭本地网页",
      "web on": "开启本地网页"
    };
    if (map[cmd]) return map[cmd];
    if (cmd.indexOf("mac ") === 0) return "保存车机蓝牙 MAC";
    if (cmd.indexOf("mode ") === 0) return "切换跟踪模式（约 1.2 秒后设备重启）";
    if (cmd.indexOf("pairpin ") === 0) return "保存手机配对 PIN";
    if (cmd.indexOf("wifista ") === 0) return "保存并连接家庭 Wi‑Fi";
    return cmd;
  }

  function sendRawCmd(cmd) {
    if (!currentId) {
      notifyErr("未选择设备");
      return Promise.reject(new Error("no device"));
    }
    var pretty = humanCmd(cmd);
    setTip($("cfgTip"), "下发：" + pretty + " …");
    notifyInfo(pretty + " 下发中…");
    return api("/api/devices/" + encodeURIComponent(currentId) + "/cmd", {
      method: "POST",
      body: JSON.stringify({ cmd: cmd })
    })
      .then(function (j) {
        if (j.ok) {
          setTip($("cfgTip"), "已下发「" + pretty + "」，约 3 秒生效", "ok");
          notifyOk(pretty + " 已下发，约 3 秒生效");
          setTimeout(refreshDetail, 4500);
        } else {
          var fail = "失败：" + (j.result || j.message || "unknown");
          setTip($("cfgTip"), fail, "err");
          notifyErr(pretty + " 失败：" + (j.result || j.message || "unknown"));
        }
        return j;
      })
      .catch(function (e) {
        setTip($("cfgTip"), e.message || "下发失败", "err");
        notifyErr(pretty + " 失败：" + (e.message || "未知错误"));
        throw e;
      });
  }

  function cfgWire() {
    $("cfgMacBtn").addEventListener("click", function () {
      var m = ($("cfgMac").value || "").trim().toUpperCase();
      if (m.length !== 17) {
        setTip($("cfgTip"), "MAC 格式应为 AA:BB:CC:DD:EE:FF", "err");
        notifyErr("MAC 格式应为 AA:BB:CC:DD:EE:FF");
        return;
      }
      sendRawCmd("mac " + m).catch(function () {});
    });
    $("cfgModeBtn").addEventListener("click", function () {
      var m = $("cfgMode").value;
      if (!window.confirm("切换跟踪模式会让设备约 1.2 秒后重启，继续？")) return;
      sendRawCmd("mode " + m).catch(function () {});
    });
    $("cfgAutoOn").addEventListener("click", function () { sendRawCmd("autotrack on").catch(function () {}); });
    $("cfgAutoOff").addEventListener("click", function () { sendRawCmd("autotrack off").catch(function () {}); });
    $("cfgPairOn").addEventListener("click", function () { sendRawCmd("pair on").catch(function () {}); });
    $("cfgPairOff").addEventListener("click", function () { sendRawCmd("pair off").catch(function () {}); });
    $("cfgPinBtn").addEventListener("click", function () {
      var pin = ($("cfgPin").value || "").trim();
      if (pin && !/^\d{1,6}$/.test(pin)) {
        setTip($("cfgTip"), "PIN 应为 1-6 位数字", "err");
        notifyErr("PIN 应为 1-6 位数字");
        return;
      }
      sendRawCmd("pairpin " + pin).then(function () { $("cfgPin").value = ""; }).catch(function () {});
    });
    $("cfgWifiBtn").addEventListener("click", function () {
      var ssid = ($("cfgSsid").value || "").trim();
      var pass = $("cfgPass").value || "";
      if (!ssid) {
        setTip($("cfgTip"), "SSID 不能为空", "err");
        notifyErr("Wi‑Fi SSID 不能为空");
        return;
      }
      sendRawCmd("wifista " + ssid + (pass ? " " + pass : ""))
        .then(function () { $("cfgPass").value = ""; })
        .catch(function () {});
    });
    $("cfgNfcBtn").addEventListener("click", function () { sendRawCmd("nfcinit").catch(function () {}); });
    $("cfgWebOff").addEventListener("click", function () {
      if (!window.confirm("关闭本地网页后，车库局域网内 80 端口不再响应（远程控制不受影响）。继续？")) return;
      sendRawCmd("web off").catch(function () {});
    });
    $("cfgWebOn").addEventListener("click", function () { sendRawCmd("web on").catch(function () {}); });
  }

  function uploadOta() {
    var file = ($("binInput").files || [])[0];
    if (!file) {
      setTip($("otaMsg"), "请选择 firmware.bin", "err");
      notifyErr("请选择 firmware.bin 文件");
      return;
    }
    var ver = $("verInput").value.trim();
    var qs = "?notify=0" + (ver ? "&version=" + encodeURIComponent(ver) : "");
    $("uploadBtn").disabled = true;
    setTip($("otaMsg"), "上传中…");
    notifyInfo("固件上传中，请稍候…");
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
        var okMsg = "固件上传成功" + (o.version ? " · " + o.version : "") + "，请到设备页点「立即更新」";
        setTip($("otaMsg"), okMsg, "ok");
        notifyOk(okMsg, 4000);
        $("otaCurrent").textContent = "服务器固件：" + (o.version || "-") + " · " + (o.size || 0) + " bytes";
        loadList();
      })
      .catch(function (e) {
        var msg = e.message || "上传失败";
        setTip($("otaMsg"), msg, "err");
        notifyErr("固件上传失败：" + msg);
      })
      .finally(function () {
        $("uploadBtn").disabled = false;
      });
  }

  function backToList() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = null;
    currentId = null;
    busyCmd = false;
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
    notifyInfo("已退出登录");
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
