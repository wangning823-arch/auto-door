/* 交接报告 · 交互
   - 按验证状态筛选「预期指标 vs 实测」表
   - 行点击切换高亮（便于逐条对照朗读）
   - 统计数字随筛选同步
*/
(function () {
  'use strict';

  var table = document.getElementById('ev');
  if (!table) return;

  var rows = Array.prototype.slice.call(table.querySelectorAll('tbody tr'));
  var buttons = Array.prototype.slice.call(document.querySelectorAll('.fb'));

  function apply(filter) {
    var shown = 0;
    rows.forEach(function (tr) {
      var ok = tr.getAttribute('data-s') === 'ok';
      var match = filter === 'all' || (filter === 'ok' && ok) || (filter === 'wait' && !ok);
      tr.classList.toggle('is-hidden', !match);
      if (match) shown++;
    });
    buttons.forEach(function (b) {
      b.classList.toggle('is-on', b.getAttribute('data-f') === filter);
    });
    buttons.forEach(function (b) {
      var f = b.getAttribute('data-f');
      if (f === 'all') b.textContent = '全部 ' + rows.length;
      else if (f === 'ok') b.textContent = '已验证 ' + rows.filter(function (r) {
        return r.getAttribute('data-s') === 'ok';
      }).length;
      else b.textContent = '待验证 ' + rows.filter(function (r) {
        return r.getAttribute('data-s') !== 'ok';
      }).length;
    });
    return shown;
  }

  buttons.forEach(function (b) {
    b.addEventListener('click', function () {
      apply(b.getAttribute('data-f'));
    });
  });

  // 点击行高亮，便于对照朗读
  rows.forEach(function (tr) {
    tr.style.cursor = 'pointer';
    tr.addEventListener('click', function () {
      var on = tr.classList.contains('is-picked');
      rows.forEach(function (r) { r.classList.remove('is-picked'); });
      if (!on) tr.classList.add('is-picked');
    });
  });

  apply('all');
})();
