(function () {
  var TOKEN_KEY = "garage_token";
  var $ = function (id) { return document.getElementById(id); };

  function token() { return sessionStorage.getItem(TOKEN_KEY) || ""; }

  function setStatus(text, cls) {
    var el = $("resultText");
    if (!el) return;
    el.textContent = text;
    el.className = "tip" + (cls ? " " + cls : "");
  }

  function showCtrl(logged) {
    $("loginView").classList.toggle("hidden", logged);
    $("ctrlView").classList.toggle("hidden", !logged);
    $("statusText").textContent = logged ? "已连接 · 可开关门" : "未登录";
  }

  function api(path, body) {
    return fetch(path, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Garage-Token": token()
      },
      body: JSON.stringify(body || {})
    }).then(function (r) {
      return r.json().then(function (j) {
        if (!r.ok) throw new Error(j.error || ("HTTP " + r.status));
        return j;
      });
    });
  }

  function login() {
    var pw = $("pw").value.trim();
    if (!pw) {
      setStatus("请输入密码", "err");
      return;
    }
    $("loginBtn").disabled = true;
    api("/api/login", { password: pw })
      .then(function (j) {
        sessionStorage.setItem(TOKEN_KEY, j.token || "");
        $("pw").value = "";
        showCtrl(true);
        setStatus("就绪", "ok");
      })
      .catch(function (e) {
        setStatus(e.message || "登录失败", "err");
      })
      .finally(function () {
        $("loginBtn").disabled = false;
      });
  }

  function sendCmd(cmd, btn) {
    setStatus("发送中…");
    btn.disabled = true;
    api("/api/" + cmd, {})
      .then(function (j) {
        setStatus(j.message || (cmd === "open" ? "已请求开门" : "已请求关门"), "ok");
      })
      .catch(function (e) {
        if ((e.message || "").indexOf("unauthorized") >= 0) {
          sessionStorage.removeItem(TOKEN_KEY);
          showCtrl(false);
          setStatus("请重新登录", "err");
          return;
        }
        setStatus(e.message || "失败", "err");
      })
      .finally(function () {
        btn.disabled = false;
      });
  }

  $("loginBtn").addEventListener("click", login);
  $("pw").addEventListener("keydown", function (e) {
    if (e.key === "Enter") login();
  });
  $("openBtn").addEventListener("click", function () { sendCmd("open", this); });
  $("closeBtn").addEventListener("click", function () { sendCmd("close", this); });
  $("logoutBtn").addEventListener("click", function () {
    sessionStorage.removeItem(TOKEN_KEY);
    showCtrl(false);
    setStatus("已退出");
  });

  showCtrl(!!token());
})();
