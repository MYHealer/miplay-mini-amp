// 配网页面（内嵌 HTML）。
//
// 单文件、无外部依赖、无构建步骤——手机连的是没有互联网的开放热点，
// 任何 CDN 字体/JS 都加载不出来。所以字体走系统栈，脚本内联。
//
// 视觉令牌取自 mimo.xiaomi.com 的实际 CSS 包（与 mix_api_bridge 同源）：
//   纸底 #faf7f5 / 墨 #26251e / 弱化线 #e5e2e0 / 强调 #eb5600
//   分格线恒为 1px solid #000（纯黑，不是灰）——这是它"出版物索引"气质的来源
//   圆角一律 0（直角），无过渡动画，无阴影
// 页面结构照搬其 App.vue + Dashboard.vue 的组织：hero 装饰字母行 → 状态条 →
// 分格面板 → 序号列表。
#pragma once

namespace miplay {

inline constexpr char kProvisionPage[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>MiPlay 耳放 · 配网</title>
<style>
:root{
  --bg:#faf7f5; --bg-soft:#f5f0eb; --bg-weak:#f3f0ef;
  --ink:#26251e; --ink-mid:#504f49; --ink-weak:#979696;
  --line:#000; --line-weak:#e5e2e0;
  --accent:#eb5600; --accent-hover:#d24d00;
  --ok:#208662; --bad:#b13b3b;
}
:root[data-theme="dark"]{
  --bg:#191d24; --bg-soft:#292e37; --bg-weak:#343a46;
  --ink:#fffff5; --ink-mid:#ebebeb; --ink-weak:#ebebeb99;
  --line:#545454; --line-weak:#5454547a;
  --accent:#ff8f71; --accent-hover:#ffa78e;
  --ok:#55c59e; --bad:#ff7676;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{
  background:var(--bg); color:var(--ink);
  font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text","Segoe UI",
              "PingFang SC","Microsoft YaHei",Roboto,sans-serif;
  font-size:14px; line-height:1.7;
  -webkit-font-smoothing:antialiased; -webkit-text-size-adjust:100%;
}
.shell{max-width:720px;margin:0 auto;padding:0 16px 64px}

/* ── 顶栏 ── */
.topbar{
  display:flex;align-items:center;justify-content:space-between;
  gap:12px;padding:14px 0;border-bottom:1px solid var(--line-weak);
}
.brand{display:flex;align-items:center;gap:8px;font-weight:600;letter-spacing:.02em}
.brand .dot{width:9px;height:9px;background:var(--accent)}
.iconbtn{
  width:32px;height:32px;display:grid;place-items:center;cursor:pointer;
  background:transparent;border:1px solid var(--line-weak);color:var(--ink);
}
.iconbtn:hover{border-color:var(--ink)}
.iconbtn svg{width:15px;height:15px;fill:none;stroke:currentColor;stroke-width:1.6}

/* ── hero：装饰字母行 ── */
.hero{position:relative;padding:34px 0 26px;border-bottom:1px solid var(--line)}
.pattern{
  position:absolute;inset:0;overflow:hidden;pointer-events:none;
  display:flex;flex-direction:column;justify-content:center;gap:2px;
  opacity:.055;user-select:none;
}
.pattern-row{
  display:flex;justify-content:space-between;
  font-size:15px;font-weight:700;letter-spacing:.28em;white-space:nowrap;
}
.section-number{
  margin:0 0 6px;font-size:11px;font-weight:600;letter-spacing:.16em;
  color:var(--accent);
}
.hero h1{margin:0;font-size:30px;line-height:1.2;font-weight:700;letter-spacing:-.01em}
.hero p{margin:10px 0 0;max-width:46ch;color:var(--ink-mid)}

/* ── 状态条：四格，黑线分格 ── */
.status-strip{
  display:grid;grid-template-columns:repeat(2,1fr);
  border:1px solid var(--line);margin:22px 0;
}
.status-strip>div{padding:12px 14px;border-right:1px solid var(--line-weak)}
.status-strip>div:last-child{border-right:0}
.status-strip .k{
  display:block;font-size:10px;font-weight:600;letter-spacing:.14em;
  text-transform:uppercase;color:var(--ink-weak);margin-bottom:3px;
}
.status-strip .v{font-weight:600;word-break:break-all}

/* ── 面板 ── */
.panel{border:1px solid var(--line);margin-bottom:22px}
.panel-head{
  display:flex;gap:12px;padding:16px 18px;
  border-bottom:1px solid var(--line-weak);
}
.panel-head h2{margin:0;font-size:16px;font-weight:700}
.panel-head p{margin:3px 0 0;color:var(--ink-mid);font-size:13px}
.panel-body{padding:18px}

/* ── 按钮 ── */
button{font:inherit;cursor:pointer}
.primary{
  background:var(--ink);color:var(--bg);border:1px solid var(--ink);
  padding:10px 18px;font-weight:600;
}
.primary:hover:not(:disabled){background:var(--accent);border-color:var(--accent);color:#fff}
.primary:disabled{opacity:.4;cursor:not-allowed}
.ghost{background:transparent;color:var(--ink);border:1px solid var(--line-weak);padding:10px 16px}
.ghost:hover{border-color:var(--ink)}

/* ── 网络列表：序号 + 名称 + 信号 + 箭头，每行一条线 ── */
.ap-list{border-top:1px solid var(--line-weak)}
.ap{
  display:flex;align-items:center;gap:14px;width:100%;text-align:left;
  padding:13px 4px;background:transparent;border:0;
  border-bottom:1px solid var(--line-weak);color:inherit;
}
.ap:hover{background:var(--bg-weak)}
.ap[aria-pressed="true"]{background:var(--bg-soft)}
.ap .idx{font-size:11px;font-weight:600;color:var(--ink-weak);min-width:18px}
.ap .name{flex:1;font-weight:600;word-break:break-all}
.ap .sig{font-size:12px;color:var(--ink-weak);white-space:nowrap}
.ap .arrow{color:var(--ink-weak)}
.ap[aria-pressed="true"] .arrow{color:var(--accent)}

/* ── 表单 ── */
label.field{display:block;margin-bottom:14px}
label.field>span{
  display:block;font-size:10px;font-weight:600;letter-spacing:.14em;
  text-transform:uppercase;color:var(--ink-weak);margin-bottom:5px;
}
input{
  width:100%;padding:10px 12px;font:inherit;color:var(--ink);
  background:var(--bg);border:1px solid var(--line-weak);border-radius:0;
}
input:focus{outline:0;border-color:var(--accent)}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}

/* ── 提示 ── */
.notice{margin:12px 0 0;padding:10px 12px;font-size:13px;border:1px solid var(--line-weak)}
.notice.ok{border-color:var(--ok);color:var(--ok)}
.notice.bad{border-color:var(--bad);color:var(--bad)}
.empty{padding:20px 4px;color:var(--ink-weak)}
.steps{margin:0;padding-left:0;list-style:none;border-top:1px solid var(--line-weak)}
.steps li{display:flex;gap:12px;padding:11px 0;border-bottom:1px solid var(--line-weak)}
.steps .n{font-size:11px;font-weight:600;color:var(--ink-weak);min-width:18px}
.steps strong{display:block;font-weight:600}
.steps p{margin:2px 0 0;color:var(--ink-mid);font-size:13px}
@media(min-width:560px){.status-strip{grid-template-columns:repeat(4,1fr)}}
</style>
</head>
<body>
<div class="shell">

  <header class="topbar">
    <div class="brand"><span class="dot"></span>MiPlay 耳放</div>
    <button class="iconbtn" id="theme" type="button" aria-label="切换深浅色">
      <svg id="theme-icon" viewBox="0 0 24 24"><path d="M20.4 14.2A7.7 7.7 0 0 1 9.8 3.6 8.7 8.7 0 1 0 20.4 14.2Z"/></svg>
    </button>
  </header>

  <section class="hero">
    <div class="pattern" id="pattern" aria-hidden="true"></div>
    <p class="section-number">01</p>
    <h1>连接 Wi-Fi</h1>
    <p>设备还没有联网。选一个 2.4GHz 网络并输入密码，连上之后这个热点会自动关闭，耳放随即上线。</p>
  </section>

  <section class="status-strip" aria-label="设备状态">
    <div><span class="k">状态</span><span class="v" id="st-state">配网中</span></div>
    <div><span class="k">已选网络</span><span class="v" id="st-ssid">—</span></div>
    <div><span class="k">热点</span><span class="v" id="st-ap">—</span></div>
    <div><span class="k">地址</span><span class="v">192.168.4.1</span></div>
  </section>

  <section class="panel">
    <div class="panel-head">
      <p class="section-number">02</p>
      <div>
        <h2>附近的网络</h2>
        <p>ESP32-S3 只支持 2.4GHz。同名网络已按信号强度合并。</p>
      </div>
    </div>
    <div class="panel-body">
      <div class="row">
        <button class="primary" id="rescan" type="button">扫描</button>
        <span id="scan-hint" style="color:var(--ink-weak);font-size:13px"></span>
      </div>
      <div class="ap-list" id="list" style="margin-top:14px">
        <div class="empty">点「扫描」列出周围网络。</div>
      </div>
    </div>
  </section>

  <section class="panel">
    <div class="panel-head">
      <p class="section-number">03</p>
      <div>
        <h2>输入密码</h2>
        <p>从上面选一个网络，或在这里直接填名称。密码错误不会覆盖设备里已有的配置。</p>
      </div>
    </div>
    <div class="panel-body">
      <form id="form" autocomplete="off">
        <label class="field">
          <span>网络名称 SSID</span>
          <input id="ssid" name="ssid" autocapitalize="off" autocorrect="off"
                 spellcheck="false" placeholder="选上面的网络，或手动输入">
        </label>
        <label class="field">
          <span>密码（开放网络留空）</span>
          <input id="pass" name="pass" type="password" autocomplete="new-password">
        </label>
        <div class="row">
          <button class="primary" id="submit" type="submit">连接</button>
          <button class="ghost" id="showpass" type="button">显示密码</button>
        </div>
      </form>
      <div id="result"></div>
    </div>
  </section>

  <section class="panel">
    <div class="panel-head">
      <p class="section-number">04</p>
      <div><h2>之后怎么改</h2><p>换网络或改了密码时。</p></div>
    </div>
    <div class="panel-body" style="padding-top:0">
      <ol class="steps">
        <li><span class="n">01</span><div><strong>断电重连</strong>
          <p>设备连不上原网络时会自动重开这个热点。</p></div></li>
        <li><span class="n">02</span><div><strong>连上热点</strong>
          <p>打开 192.168.4.1，回到这一页重新填。</p></div></li>
        <li><span class="n">03</span><div><strong>看到橙灯</strong>
          <p>灯是橙色 = 还没联网；变成蓝色 = 已经连上了。</p></div></li>
      </ol>
    </div>
  </section>

</div>

<script>
(function(){
  var $ = function(id){ return document.getElementById(id); };

  // ── 主题 ──
  var SUN = 'M12 3v2.1M12 18.9V21M4.4 4.4l1.5 1.5M18.1 18.1l1.5 1.5M3 12h2.1M18.9 12H21M4.4 19.6l1.5-1.5M18.1 5.9l1.5-1.5';
  var MOON = 'M20.4 14.2A7.7 7.7 0 0 1 9.8 3.6 8.7 8.7 0 1 0 20.4 14.2Z';
  var root = document.documentElement;
  function paint(){
    var dark = root.getAttribute('data-theme') === 'dark';
    $('theme-icon').innerHTML = dark
      ? '<path d="'+SUN+'"/><circle cx="12" cy="12" r="4.2"/>'
      : '<path d="'+MOON+'"/>';
  }
  try { root.setAttribute('data-theme', localStorage.getItem('mp-theme') || 'light'); } catch(e){}
  paint();
  $('theme').onclick = function(){
    var next = root.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    root.setAttribute('data-theme', next);
    try { localStorage.setItem('mp-theme', next); } catch(e){}
    paint();
  };

  // ── hero 装饰字母行 ──
  (function(){
    var letters = ['M','I','P','L','A','Y'], html = '';
    for (var r = 0; r < 7; r++){
      var row = '';
      for (var c = 0; c < 16; c++) row += letters[(c + r) % letters.length];
      html += '<span class="pattern-row">' + row + '</span>';
    }
    $('pattern').innerHTML = html;
  })();

  // ── 扫描 ──
  var list = $('list'), hint = $('scan-hint'), picked = null;

  function bars(rssi){
    if (rssi >= -55) return '信号强';
    if (rssi >= -70) return '信号中';
    return '信号弱';
  }

  function render(aps){
    list.innerHTML = '';
    if (!aps.length){
      list.innerHTML = '<div class="empty">没有扫到网络。靠近路由器再试，'
                     + '并确认它是 2.4GHz。</div>';
      return;
    }
    aps.forEach(function(ap, i){
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'ap';
      b.setAttribute('aria-pressed', 'false');
      var n = (i + 1 < 10 ? '0' : '') + (i + 1);
      b.innerHTML = '<span class="idx">' + n + '</span>'
                  + '<span class="name"></span>'
                  + '<span class="sig">' + bars(ap.rssi) + ' ' + ap.rssi + ' dBm</span>'
                  + '<span class="arrow">&rarr;</span>';
      b.querySelector('.name').textContent = ap.ssid;   // 用 textContent 防 SSID 里的 HTML
      b.onclick = function(){
        Array.prototype.forEach.call(list.children, function(el){
          el.setAttribute('aria-pressed', 'false');
        });
        b.setAttribute('aria-pressed', 'true');
        picked = ap.ssid;
        $('ssid').value = ap.ssid;
        $('st-ssid').textContent = ap.ssid;
        $('pass').focus();
      };
      list.appendChild(b);
    });
  }

  function scan(){
    $('rescan').disabled = true;
    hint.textContent = '扫描中…';
    list.innerHTML = '<div class="empty">扫描中…</div>';
    fetch('/api/scan').then(function(r){ return r.json(); }).then(function(d){
      render(d.aps || []);
      hint.textContent = d.aps ? (d.aps.length + ' 个网络') : '';
    }).catch(function(){
      list.innerHTML = '<div class="empty">扫描失败，重试一次。</div>';
      hint.textContent = '';
    }).then(function(){
      $('rescan').disabled = false;
    });
  }
  $('rescan').onclick = scan;

  // ── 提交 ──
  $('showpass').onclick = function(){
    var p = $('pass');
    p.type = p.type === 'password' ? 'text' : 'password';
    this.textContent = p.type === 'password' ? '显示密码' : '隐藏密码';
  };

  $('form').onsubmit = function(ev){
    ev.preventDefault();
    var ssid = $('ssid').value.trim();
    var out = $('result');
    if (!ssid){ out.innerHTML = '<p class="notice bad">请先选一个网络。</p>'; return; }

    $('submit').disabled = true;
    $('st-state').textContent = '连接中';
    out.innerHTML = '<p class="notice">正在连接，最多等 15 秒…</p>';

    fetch('/api/connect', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ssid: ssid, pass: $('pass').value})
    }).then(function(r){ return r.json(); }).then(function(d){
      if (d.ok){
        out.innerHTML = '<p class="notice ok">已连接。热点即将关闭，'
                      + '把手机切回原来的 Wi-Fi 即可在妙播里看到设备。</p>';
        $('st-state').textContent = '已联网';
      } else {
        out.innerHTML = '<p class="notice bad">连接失败：'
                      + (d.msg || '未知原因') + '。密码不对时请重新输入。</p>';
        $('st-state').textContent = '配网中';
        $('submit').disabled = false;
      }
    }).catch(function(){
      out.innerHTML = '<p class="notice bad">请求中断——设备可能已经切走，'
                    + '检查手机 Wi-Fi 是否已连上目标网络。</p>';
      $('submit').disabled = false;
    });
  };

  fetch('/api/info').then(function(r){ return r.json(); }).then(function(d){
    $('st-ap').textContent = d.ap || '—';
  }).catch(function(){});

  scan();
})();
</script>
</body>
</html>)HTML";

}  // namespace miplay