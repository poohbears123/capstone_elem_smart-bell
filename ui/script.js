/**
 * ============================================================
 * FCU Smart Bell - UI logic (./ui/script.js)
 * Design: ./new UI  |  Backend: capstone2.1.0P.ino v2.2.0
 * ------------------------------------------------------------
 * Firmware routes (all but /handleLogin + probes need FCU_SESS cookie):
 *   POST /handleLogin (urlencoded username/password) -> {status:"ok"} + Set-Cookie
 *   GET  /logout -> expires cookie, 302 to /
 *   GET  /            -> login.html (guest) or index.html (authed)
 *   GET  /api/telemetry          -> {time,status,ssid,ap_ssid,attendance,sniffer,schedule_count,ntp_ok,last_ntp}
 *   GET  /api/schedule           -> {schedules:[{id,subject,days[],startTime,endTime,chimeStart,chimeEnd}]}
 *   POST /api/schedule (JSON)    -> {status:"ok"}
 *   GET  /api/attendance         -> CSV "Timestamp,MAC_Address,RSSI,Status"
 *   GET  /api/attendance/status  -> {active:bool}
 *   GET  /ring[?file=name.wav] (.mp3 names auto-map to .wav) -> {status:"ok"}
 *   GET  /api/audio/list         -> {files:[{name,size}]}
 *   POST /api/audio/upload (multipart .wav <=300KB) -> {status:"ok",name,size}
 *   GET  /api/activity?limit=N   -> {events:[{t,ev}],count} (newest first)
 *   POST /api/time/sync         -> NTP sync RTC (needs STA) -> {status:"ok",time}
 *   GET  /api/config             -> {ap_ssid,sta_ssid,sta_pass}
 *   POST /api/config (JSON)      -> {status:"ok"}
 *   GET  /test                   -> "FCU Smart Bell OK!"
 * User/manual/settings views remain static/local (no firmware API).
 * ============================================================
 */
(function () {
  'use strict';

  var API = {
    telemetry: '/api/telemetry',
    schedule: '/api/schedule',
    attendance: '/api/attendance',
    attendanceStatus: '/api/attendance/status',
    login: '/handleLogin',
    logout: '/logout',
    ring: '/ring',
    config: '/api/config',
    test: '/test',
    audioList: '/api/audio/list',
    audioUpload: '/api/audio/upload',
    timeSync: '/api/time/sync',
    activity: '/api/activity'
  };

  // ---------- icon fallback (no CDN: map fa-* to local glyphs) ----------
  (function iconFallback() {
    var map = { gauge: '◉', 'calendar-days': '📅', users: '👥', wifi: '📶', person: '👤', clock: '🕒', hand: '✋', gear: '⚙', bell: '🔔', 'right-from-bracket': '⏻', 'list-ol': '☰', 'circle-check': '✔', signal: '📶', 'list-check': '☑', 'triangle-exclamation': '⚠', 'rotate-right': '🔄', 'user-shield': '🛡' };
    document.querySelectorAll('i[class*="fa-"]').forEach(function (el) {
      var cls = el.className.split(/\s+/);
      for (var i = 0; i < cls.length; i++) {
        var k = cls[i].replace(/^fa-(solid|regular)-/, '');
        if (map[k]) { el.textContent = map[k]; el.setAttribute('aria-hidden', 'true'); break; }
      }
    });
  })();

  // ---------- helpers ----------
  function $(id) { return document.getElementById(id); }
  function esc(s) {
    return String(s == null ? '' : s).replace(/&/g, '&amp;')
      .replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  function pad2(n) { return String(n).padStart(2, '0'); }

  // "08:00:00" or "08:00" -> "08:00 AM" / "12:00 NN" / "01:00 PM"
  function displayTime(startTime) {
    var m = String(startTime || '').match(/(\d{1,2}):(\d{2})/);
    if (!m) return String(startTime || '--');
    var h = Number(m[1]), mm = m[2];
    if (h === 12 && mm === '00') return '12:00 NN';
    var ap = h >= 12 ? 'PM' : 'AM';
    var h12 = h % 12; if (h12 === 0) h12 = 12;
    return pad2(h12) + ':' + mm + ' ' + ap;
  }

  // form "HH:MM" + period AM/NN/PM -> "HH:MM:00" (24h)
  function toStartTime(hhmm, period) {
    var m = String(hhmm || '').trim().match(/^(\d{1,2}):(\d{2})$/);
    if (!m) return '';
    var h = Number(m[1]), mm = Number(m[2]);
    if (h > 23 || mm > 59) return '';
    var p = String(period || 'AM').toUpperCase();
    if (p === 'NN') h = 12;
    else if (p === 'PM' && h < 12) h += 12;
    else if (p === 'AM' && h === 12) h = 0;
    return pad2(h) + ':' + pad2(mm) + ':00';
  }
  function addMinutes(hhmmss, mins) {
    var m = String(hhmmss).match(/(\d+):(\d+)/);
    var t = (Number(m[1]) * 60 + Number(m[2]) + mins + 1440) % 1440;
    return pad2(Math.floor(t / 60)) + ':' + pad2(t % 60) + ':00';
  }
  function basename(p) {
    var s = String(p || '');
    var i = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
    return i >= 0 ? s.slice(i + 1) : s;
  }

  // ---------- view nav (works with or without login wrapper) ----------
  var navLinks = document.querySelectorAll('.menu a[data-view]');
  var viewLinks = document.querySelectorAll('[data-view]');
  function showView(viewId) {
    document.querySelectorAll('.view-section').forEach(function (s) {
      s.classList.toggle('active', s.id === viewId);
    });
    navLinks.forEach(function (l) {
      l.classList.toggle('active', l.getAttribute('data-view') === viewId);
    });
    if (viewId === 'attendance-view') loadAttendance();
    if (viewId === 'activity-view') loadActivity();
  }
  viewLinks.forEach(function (l) {
    l.addEventListener('click', function (e) { e.preventDefault(); showView(l.getAttribute('data-view')); });
  });

  // ---------- login (server session: POST /handleLogin -> FCU_SESS cookie) ----------
  var loginForm = $('login-form'), loginPage = $('login-page'), dashboardPage = $('dashboard-page');
  var hasLoginGate = !!(loginForm && loginPage && dashboardPage);
  function showDashboard() {
    if (!hasLoginGate) return;
    loginPage.style.display = 'none'; dashboardPage.style.display = 'flex';
  }
  function showLogin() {
    if (!hasLoginGate) return;
    loginPage.style.display = 'flex'; dashboardPage.style.display = 'none';
  }
  function loginMsg(t) {
    var el = $('login-message');
    if (el) el.textContent = t || '';
  }
  if (loginForm) loginForm.addEventListener('submit', async function (e) {
    e.preventDefault();
    var u = ($('login-username') && $('login-username').value || '').trim();
    var p = $('login-password') ? $('login-password').value : '';
    if (!u || !p) { loginMsg('Enter username and password.'); return; }
    loginMsg('Signing in...');
    try {
      var res = await fetch(API.login, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'username=' + encodeURIComponent(u) + '&password=' + encodeURIComponent(p)
      });
      if (res.ok) {
        loginMsg('');
        if ($('login-password')) $('login-password').value = '';
        showDashboard(); showView('dashboard-view');
        loadTelemetry(); refreshSchedules();
      }
      else if (res.status === 423) loginMsg('Locked after too many attempts. Try again in a minute.');
      else loginMsg('Invalid username or password.');
    } catch (err) { loginMsg('Network error: ' + err.message); }
  });
  var logoutLink = $('logout-link');
  if (logoutLink) logoutLink.addEventListener('click', function (e) {
    e.preventDefault();
    fetch(API.logout, { cache: 'no-store' }).catch(function () {});
    deviceTime = null;
    showLogin();
  });
  // Source of truth is the firmware session: probe telemetry on boot.
  async function probeAuth() {
    if (!hasLoginGate) { loadTelemetry(); refreshSchedules(); return; }
    try {
      var res = await fetch(API.telemetry, { cache: 'no-store' });
      if (res.ok) { showDashboard(); showView('dashboard-view'); }
      else showLogin();
    } catch (err) { showLogin(); }
  }

  // ---------- telemetry ----------
  var deviceTime = null, clockTimer = null;
  function tickClock() {
    var el = $('current-time');
    if (!el || !deviceTime) return;
    deviceTime = new Date(deviceTime.getTime() + 1000);
    el.textContent = deviceTime.getUTCFullYear() + '-' + pad2(deviceTime.getUTCMonth() + 1) + '-' +
      pad2(deviceTime.getUTCDate()) + ' ' + pad2(deviceTime.getUTCHours()) + ':' +
      pad2(deviceTime.getUTCMinutes()) + ':' + pad2(deviceTime.getUTCSeconds());
  }
  function parseDeviceTime(s) {
    var m = String(s || '').match(/(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})/);
    if (!m) return null;
    return new Date(Date.UTC(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6])));
  }
  async function loadTelemetry() {
    try {
      var res = await fetch(API.telemetry, { cache: 'no-store' });
      if (res.status === 401) { showLogin(); return; }
      if (!res.ok) return;
      var d = await res.json();
      // d = {time,status,ssid,ap_ssid,attendance,sniffer,schedule_count,ntp_ok,last_ntp}
      var ct = $('current-time');
      if (ct && d.time) {
        var parsed = parseDeviceTime(d.time);
        if (parsed) {
          deviceTime = parsed; ct.textContent = d.time;
          if (!clockTimer) clockTimer = setInterval(tickClock, 1000);
        } else { deviceTime = null; ct.textContent = d.time; }
      }
      var sm = $('system-mode');
      if (sm) {
        sm.textContent = d.attendance ? 'Attendance Active' :
          (d.sniffer ? 'Sniffing' : (d.status === 'Connected' ? 'Automatic (Online)' : 'Automatic (AP Only)'));
      }
      var ns = $('network-status');
      if (ns) ns.textContent = d.status || '--';
      var cs = $('connected-ssid');
      if (cs) cs.textContent = d.ssid && d.ssid !== '(none)' ? d.ssid : (d.ap_ssid || '--');
      var ap = $('ap-ssid');
      if (ap) ap.textContent = d.ap_ssid || '--';
      var sc = $('schedule-count-telemetry');
      if (sc && d.schedule_count !== undefined) sc.textContent = String(d.schedule_count);
      var ln = $('last-ntp');
      if (ln && d.last_ntp) ln.textContent = d.last_ntp + (d.ntp_ok ? '' : ' (unsynced)');
      var lsc = $('last-sync-card');
      if (lsc && d.last_ntp) lsc.textContent = d.last_ntp;
      updateAttendanceBadge(!!d.attendance);
      refreshSchedules();
    } catch (e) { /* offline: keep last values */ }
  }

  // ---------- schedule (GET/POST /api/schedule, exact firmware schema) ----------
  var scheduleCache = []; // raw firmware entries
  var editingId = null;

  function scheduleMsg(t, type) {
    var el = $('schedule-message');
    if (!el) return;
    el.textContent = t || ''; el.className = 'message ' + (type || 'success');
  }
  async function fetchScheduleDoc() {
    var res = await fetch(API.schedule, { cache: 'no-store' });
    if (res.status === 401) { showLogin(); throw new Error('unauthorized (session expired)'); }
    if (!res.ok) throw new Error('GET /api/schedule -> ' + res.status);
    var doc = await res.json();
    if (!doc || !Array.isArray(doc.schedules)) throw new Error('bad schedule JSON');
    return doc;
  }
  async function refreshSchedules() {
    try {
      var doc = await fetchScheduleDoc();
      scheduleCache = doc.schedules;
      renderSchedules(scheduleCache);
    } catch (e) {
      scheduleMsg('Cannot load schedule: ' + e.message, 'error');
    }
  }
  function renderSchedules(list) {
    var body = $('schedule-body'), listBody = $('schedule-list-body');
    var next1 = $('next-bell'), next2 = $('schedule-next-bell'), cnt = $('schedule-count');
    if (body) body.innerHTML = '';
    if (listBody) listBody.innerHTML = '';
    if (!list || !list.length) {
      if (body) body.innerHTML = '<tr><td colspan="5" class="empty-state">No bell schedules yet.</td></tr>';
      if (listBody) listBody.innerHTML = '<tr><td colspan="4" class="empty-state">No bell schedules yet.</td></tr>';
      if (next1) next1.textContent = 'No schedule set';
      if (next2) next2.textContent = 'No schedule set';
      if (cnt) cnt.textContent = '0';
      return;
    }
    list.forEach(function (en) {
      var t = displayTime(en.startTime);
      if (body) {
        var tr = document.createElement('tr');
        tr.innerHTML = '<td>' + esc(t) + '</td><td>' + esc(en.subject) + '</td>' +
          '<td>' + esc((en.days || []).join(', ')) + '</td>' +
          '<td><code class="mono">' + esc(basename(en.chimeStart)) + '</code></td>' +
          '<td><button type="button" class="edit-btn" data-id="' + esc(en.id) + '">Edit</button>' +
          '<button type="button" class="delete-btn" data-id="' + esc(en.id) + '">Delete</button></td>';
        body.appendChild(tr);
      }
      if (listBody) {
        var r2 = document.createElement('tr');
        r2.innerHTML = '<td>' + esc(t) + '</td><td>' + esc(en.subject) + '</td>' +
          '<td><code class="mono">' + esc(basename(en.chimeStart)) + '</code></td>';
        listBody.appendChild(r2);
      }
    });
    var first = list[0];
    var label = displayTime(first.startTime) + ' - ' + first.subject;
    if (next1) next1.textContent = label;
    if (next2) next2.textContent = label;
    if (cnt) cnt.textContent = String(list.length);
  }

  function selectedDays() {
    var boxes = document.querySelectorAll('input[name="sched-day"]:checked');
    var out = [];
    boxes.forEach(function (b) { out.push(b.value); });
    return out.length ? out : ['Mon', 'Tue', 'Wed', 'Thu', 'Fri'];
  }
  function resetScheduleForm() {
    var f = $('schedule-form');
    if (f) f.reset();
    var h = $('edit-id'); if (h) h.value = '';
    editingId = null; scheduleMsg('');
  }
  async function saveSchedule(e) {
    if (e) e.preventDefault();
    var timeEl = $('schedule-time'), perEl = $('schedule-period'),
        descEl = $('schedule-description'), fileEl = $('schedule-audio-file');
    var start = toStartTime(timeEl ? timeEl.value : '', perEl ? perEl.value : 'AM');
    var desc = descEl ? descEl.value.trim() : '';
    if (!start || !desc) { scheduleMsg('Enter a valid HH:MM time and description.', 'error'); return; }
    var audioName = (fileEl && fileEl.files && fileEl.files[0]) ? fileEl.files[0].name : '';
    if (!audioName && editingId != null) {
      var cur = scheduleCache.find(function (x) { return String(x.id) === String(editingId); });
      if (cur) audioName = basename(cur.chimeStart);
    }
    var chime = '/audio/' + (audioName || 'chime.wav');
    try {
      scheduleMsg('Saving...', 'warning');
      var doc = await fetchScheduleDoc();
      if (editingId != null) {
        var found = false;
        doc.schedules = doc.schedules.map(function (en) {
          if (String(en.id) === String(editingId)) {
            found = true;
            return { id: en.id, subject: desc, days: selectedDays(), startTime: start,
                     endTime: en.endTime || addMinutes(start, 60), chimeStart: chime, chimeEnd: en.chimeEnd || chime };
          }
          return en;
        });
        if (!found) throw new Error('entry not found');
      } else {
        var maxId = doc.schedules.reduce(function (m, en) { return Math.max(m, Number(en.id) || 0); }, 0);
        doc.schedules.push({ id: maxId + 1, subject: desc, days: selectedDays(), startTime: start,
          endTime: addMinutes(start, 60), chimeStart: chime, chimeEnd: chime });
      }
      var post = await fetch(API.schedule, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(doc)
      });
      if (!post.ok) throw new Error('POST /api/schedule -> ' + post.status);
      scheduleMsg(editingId != null ? 'Schedule updated.' : 'Schedule added.', 'success');
      resetScheduleForm(); refreshSchedules();
    } catch (err) { scheduleMsg('Save failed: ' + err.message, 'error'); }
  }
  async function deleteScheduleById(id) {
    if (!confirm('Delete schedule #' + id + '?')) return;
    try {
      var doc = await fetchScheduleDoc();
      doc.schedules = doc.schedules.filter(function (en) { return String(en.id) !== String(id); });
      var post = await fetch(API.schedule, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(doc)
      });
      if (!post.ok) throw new Error('POST -> ' + post.status);
      scheduleMsg('Schedule deleted.', 'success');
      if (String(editingId) === String(id)) resetScheduleForm();
      refreshSchedules();
    } catch (err) { scheduleMsg('Delete failed: ' + err.message, 'error'); }
  }
  document.addEventListener('click', function (e) {
    var b = e.target.closest('button'); if (!b) return;
    if (b.classList.contains('edit-btn') && b.hasAttribute('data-id')) {
      var id = b.getAttribute('data-id');
      var en = scheduleCache.find(function (x) { return String(x.id) === String(id); });
      if (!en) return;
      editingId = en.id;
      var h = $('edit-id'); if (h) h.value = en.id;
      var m = String(en.startTime || '').match(/(\d{1,2}):(\d{2})/);
      var t = $('schedule-time');
      if (m && t) t.value = m[1].padStart(2, '0') + ':' + m[2];
      var p = $('schedule-period');
      if (p && m) {
        var h24 = Number(m[1]);
        p.value = (h24 === 12 && m[2] === '00') ? 'NN' : (h24 >= 12 ? 'PM' : 'AM');
      }
      var dEl = $('schedule-description');
      if (dEl) dEl.value = en.subject || '';
      (en.days || []).forEach(function (dd) {
        var box = document.querySelector('input[name="sched-day"][value="' + dd + '"]');
        if (box) box.checked = true;
      });
      scheduleMsg('Editing #' + en.id + '.');
      showView('dashboard-view');
      if (t) t.focus();
    } else if (b.classList.contains('delete-btn') && b.hasAttribute('data-id')) {
      deleteScheduleById(b.getAttribute('data-id'));
    }
  });
  var schedForm = $('schedule-form');
  if (schedForm) schedForm.addEventListener('submit', saveSchedule);
  var cancelBtn = $('cancel-edit-btn');
  if (cancelBtn) cancelBtn.addEventListener('click', resetScheduleForm);
  var timeInput = $('schedule-time');
  if (timeInput) timeInput.addEventListener('input', function () {
    timeInput.value = timeInput.value.replace(/[^0-9:]/g, '').slice(0, 5);
  });

  // ---------- attendance (GET /api/attendance CSV + /api/attendance/status) ----------
  function updateAttendanceBadge(active) {
    var badge = $('attendance-status');
    if (!badge) return;
    badge.textContent = active ? 'ACTIVE' : 'INACTIVE';
    badge.className = 'status-badge ' + (active ? 'active' : 'inactive');
    var btn = $('attendance-mode-btn');
    if (btn) { btn.textContent = active ? 'Stop Attendance' : 'Start Attendance'; }
  }
  async function loadAttendance() {
    var body = $('attendance-table-body');
    try {
      var sres = await fetch(API.attendanceStatus, { cache: 'no-store' });
      if (sres.ok) { var sj = await sres.json(); updateAttendanceBadge(!!sj.active); }
      var res = await fetch(API.attendance, { cache: 'no-store' });
      if (res.status === 401) {
        showLogin();
        if (body) body.innerHTML = '<tr><td colspan="4" class="empty-state">Session expired. Please sign in.</td></tr>';
        return;
      }
      if (!res.ok) throw new Error('GET /api/attendance -> ' + res.status);
      var csv = (await res.text()).trim();
      var lines = csv ? csv.split('\n') : [];
      var rows = lines.slice(1).filter(function (l) { return l.trim(); });
      var cnt = $('attendance-count');
      if (cnt) cnt.textContent = String(rows.length);
      var sum = 0, n = 0;
      rows.forEach(function (l) {
        var c = l.split(',');
        var r = parseInt((c[2] || '').trim(), 10);
        if (!isNaN(r)) { sum += r; n++; }
      });
      var avg = $('attendance-avg-rssi');
      if (avg) avg.textContent = n ? Math.round(sum / n) + ' dBm' : '-- dBm';
      var rt = $('attendance-refresh-time');
      if (rt) rt.textContent = 'Updated ' + new Date().toLocaleTimeString();
      if (!body) return;
      if (!rows.length) {
        body.innerHTML = '<tr><td colspan="4" class="empty-state">No attendance records yet.</td></tr>';
        return;
      }
      body.innerHTML = rows.slice().reverse().map(function (l) {
        var c = l.split(',');
        return '<tr><td>' + esc((c[0] || '').trim()) + '</td><td><code class="mono">' +
          esc((c[1] || '').trim()) + '</code></td><td>' + esc((c[2] || '').trim()) +
          ' dBm</td><td><span class="status-badge active">' + esc((c[3] || '').trim()) + '</span></td></tr>';
      }).join('');
    } catch (err) {
      if (body) body.innerHTML = '<tr><td colspan="4" class="empty-state">Error: ' + esc(err.message) + '</td></tr>';
    }
  }
  var refAtt = $('refresh-attendance-btn');
  if (refAtt) refAtt.addEventListener('click', loadAttendance);

  // ---------- activity log (GET /api/activity, newest first) ----------
  function activityStatus(ev) {
    if (/FAIL|LOCKOUT|TIMEOUT|ERROR/i.test(ev)) return ['inactive', 'Needs Attention'];
    if (/NTP_SYNC|SYSTEM_BOOT|WIFI_STA/i.test(ev)) return ['pending', 'Info'];
    return ['active', 'Success'];
  }
  async function loadActivity() {
    var body = $('activity-body'), msg = $('activity-message');
    try {
      var res = await fetch(API.activity + '?limit=60', { cache: 'no-store' });
      if (res.status === 401) {
        showLogin();
        if (body) body.innerHTML = '<tr><td colspan="3" class="empty-state">Session expired. Please sign in.</td></tr>';
        return;
      }
      if (!res.ok) throw new Error('HTTP ' + res.status);
      var ctype = res.headers.get('content-type') || '';
      var txt = await res.text();
      if (ctype.indexOf('application/json') < 0 || txt.charAt(0) !== '{') {
        throw new Error('device returned a page, not JSON — upload the latest firmware');
      }
      var doc = JSON.parse(txt);
      var list = doc.events || [];
      var cnt = $('activity-count');
      if (cnt) cnt.textContent = String(doc.count != null ? doc.count : list.length);
      var alerts = 0;
      list.forEach(function (e) { if (activityStatus(e.ev || '')[0] === 'inactive') alerts++; });
      var alEl = $('activity-alerts');
      if (alEl) alEl.textContent = String(alerts);
      if (!body) return;
      if (!list.length) {
        body.innerHTML = '<tr><td colspan="3" class="empty-state">No events yet.</td></tr>';
        return;
      }
      body.innerHTML = list.map(function (e) {
        var st = activityStatus(e.ev || '');
        return '<tr><td><code class="mono">' + esc(e.t) + '</code></td><td>' + esc(e.ev) +
          '</td><td><span class="status-badge ' + st[0] + '">' + st[1] + '</span></td></tr>';
      }).join('');
      if (msg) msg.textContent = '';
    } catch (err) {
      if (body) body.innerHTML = '<tr><td colspan="3" class="empty-state">Error: ' + esc(err.message) + '</td></tr>';
    }
  }
  var refAct = $('refresh-activity-btn');
  if (refAct) refAct.addEventListener('click', loadActivity);

  // ---------- network config (GET/POST /api/config JSON) ----------
  async function loadConfig() {
    var apEl = $('net-ap'), ssEl = $('net-ssid'), pwEl = $('net-pass');
    if (!apEl && !ssEl && !pwEl) return;
    try {
      var res = await fetch(API.config, { cache: 'no-store' });
      if (res.status === 401) { showLogin(); return; }
      if (!res.ok) return;
      var d = await res.json();
      if (apEl) apEl.value = d.ap_ssid || '';
      if (ssEl) ssEl.value = d.sta_ssid || '';
      if (pwEl) pwEl.value = d.sta_pass || '';
    } catch (e) {}
  }
  async function saveNetwork() {
    var ap = ($('net-ap') && $('net-ap').value || '').trim();
    var ss = ($('net-ssid') && $('net-ssid').value || '').trim();
    var pw = $('net-pass') ? $('net-pass').value : '';
    var msg = $('netMsg');
    if (msg) msg.textContent = 'Saving...';
    try {
      var res = await fetch(API.config, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ap_ssid: ap, sta_ssid: ss, sta_pass: pw })
      });
      if (res.status === 401) { showLogin(); if (msg) msg.textContent = 'Session expired.'; return; }
      if (!res.ok) {
        var err = '';
        try { err = (await res.json()).error || ''; } catch (e) {}
        if (msg) msg.textContent = 'Save failed' + (err ? ': ' + err : ' (' + res.status + ')');
        return;
      }
      if (msg) {
        msg.textContent = 'Saved and applied live.';
        setTimeout(function () { if (msg.textContent === 'Saved and applied live.') msg.textContent = ''; }, 8000);
      }
      loadTelemetry();
    } catch (e) {
      if (msg) msg.textContent = 'Network error during save.';
    }
  }
  var saveNetworkBtn = $('save-network-btn');
  if (saveNetworkBtn) saveNetworkBtn.addEventListener('click', saveNetwork);

  // ---------- time sync (POST /api/time/sync, needs STA online) ----------
  var syncTimeBtn = $('sync-time-btn');
  if (syncTimeBtn) syncTimeBtn.addEventListener('click', async function () {
    var msg = $('timeMsg');
    if (msg) msg.textContent = 'Syncing via NTP...';
    try {
      var res = await fetch(API.timeSync, { method: 'POST' });
      if (res.status === 401) { showLogin(); if (msg) msg.textContent = 'Session expired.'; return; }
      if (res.status === 503) { if (msg) msg.textContent = 'STA offline: join WiFi first.'; return; }
      if (!res.ok) { if (msg) msg.textContent = 'Sync failed: ' + res.status; return; }
      var doc = await res.json();
      if (msg) msg.textContent = 'RTC synced: ' + (doc.time || 'ok');
      loadTelemetry();
    } catch (err) { if (msg) msg.textContent = 'Network error: ' + err.message; }
  });

  // ---------- system controls ----------
  function actionMsg(t, type) {
    var el = $('action-message'); if (!el) return;
    el.textContent = t || ''; el.className = 'message ' + (type || 'success');
  }
  async function ringBell(file) {
    function say(t, c) {
      var el = $('action-message');
      if (el) { el.textContent = t; el.className = 'message ' + (c || 'success'); }
    }
    say('Dispatching...', 'warning');
    try {
      var url = API.ring + (file ? '?file=' + encodeURIComponent(file) : '');
      var res = await fetch(url, { method: 'GET' });
      if (res.status === 401) { showLogin(); say('Session expired. Please sign in.', 'error'); return; }
      if (res.status === 404) { say('Audio file missing on the SD card.', 'error'); return; }
      if (!res.ok) { say('Ring failed (' + res.status + ').', 'error'); return; }
      say('Bell dispatched.', 'success');
      setTimeout(function () {
        var el = $('action-message');
        if (el && el.textContent === 'Bell dispatched.') el.textContent = '';
      }, 3000);
    } catch (e) {
      say('Network error.', 'error');
    }
  }
  var moBtn = $('manual-override-btn');
  if (moBtn) moBtn.addEventListener('click', function () {
    var sm = $('system-mode'); if (sm) sm.textContent = 'Manual Override (local)';
    actionMsg('Override mode is local UI state (no firmware endpoint). Use Ring Bell for an immediate remote chime.', 'warning');
  });
  var emBtn = $('emergency-btn');
  if (emBtn) emBtn.addEventListener('click', function () {
    var sm = $('system-mode'); if (sm) sm.textContent = 'Emergency';
    ringBell('alert.wav');
  });
  var rbBtn = $('ring-bell-btn');
  if (rbBtn) rbBtn.addEventListener('click', function () { ringBell(); });
  var bkBtn = $('backup-btn');
  if (bkBtn) bkBtn.addEventListener('click', async function () {
    try {
      var doc = await fetchScheduleDoc();
      var blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
      var a = document.createElement('a');
      a.href = URL.createObjectURL(blob); a.download = 'schedule-backup.json'; a.click();
      setTimeout(function () { URL.revokeObjectURL(a.href); }, 2000);
      actionMsg('Schedule backup downloaded (GET /api/schedule). Restore by POST-ing the file body to /api/schedule.', 'success');
    } catch (err) { actionMsg('Backup failed: ' + err.message, 'error'); }
  });
  var tBtn = $('test-btn');
  if (tBtn) tBtn.addEventListener('click', async function () {
    try {
      var r = await fetch(API.test, { cache: 'no-store' });
      actionMsg(r.ok ? 'Device reachable: ' + await r.text() : 'Test failed: ' + r.status,
        r.ok ? 'success' : 'error');
    } catch (err) { actionMsg('Network error: ' + err.message, 'error'); }
  });

  // ---------- audio upload/list (POST /api/audio/upload, GET /api/audio/list) ----------
  function uploadMsg(t, type) {
    var el = $('upload-message'); if (!el) return;
    el.textContent = t || ''; el.className = 'message ' + (type || 'success');
  }
  async function refreshAudioList() {
    var ul = $('audio-list');
    try {
      var res = await fetch(API.audioList, { cache: 'no-store' });
      if (res.status === 401) { showLogin(); return; }
      if (!res.ok) throw new Error('HTTP ' + res.status);
      var doc = await res.json();
      if (!ul) return;
      ul.innerHTML = '';
      (doc.files || []).forEach(function (f) {
        var li = document.createElement('li');
        var kb = Math.round((f.size || 0) / 1024);
        li.innerHTML = '<code class="mono">' + esc(f.name) + '</code> (' + kb + 'KB) ';
        var pb = document.createElement('button');
        pb.type = 'button'; pb.textContent = 'Play'; pb.className = 'secondary-btn';
        pb.addEventListener('click', function () { ringBell(f.name); });
        li.appendChild(pb);
        ul.appendChild(li);
      });
      if (!(doc.files || []).length && ul) ul.innerHTML = '<li>No .wav files on SD.</li>';
    } catch (err) { uploadMsg('Audio list failed: ' + err.message, 'error'); }
  }
  var upBtn = $('upload-audio-btn');
  if (upBtn) upBtn.addEventListener('click', async function () {
    var inp = $('audio-upload-file');
    var f = inp && inp.files && inp.files[0];
    if (!f) { uploadMsg('Choose a .wav file first.', 'error'); return; }
    if (!/\.wav$/i.test(f.name)) { uploadMsg('Only .wav files (16-bit mono).', 'error'); return; }
    if (f.size > 300 * 1024) { uploadMsg('File too large (max 300KB).', 'error'); return; }
    uploadMsg('Uploading ' + f.name + '...', 'warning');
    try {
      var fd = new FormData();
      fd.append('file', f, f.name);
      var res = await fetch(API.audioUpload, { method: 'POST', body: fd });
      if (res.status === 401) { showLogin(); uploadMsg('Session expired. Sign in.', 'error'); return; }
      var txt = await res.text();
      if (!res.ok) { uploadMsg('Upload failed: ' + txt, 'error'); return; }
      uploadMsg('Uploaded ' + f.name + '.', 'success');
      inp.value = '';
      refreshAudioList();
    } catch (err) { uploadMsg('Network error: ' + err.message, 'error'); }
  });
  var raBtn = $('refresh-audio-btn');
  if (raBtn) raBtn.addEventListener('click', refreshAudioList);
  if ($('audio-list')) refreshAudioList();

  // ---------- settings (LOCAL ONLY: no /api/config in firmware) ----------
  var sForm = $('settings-form');
  function loadSettings() {
    var s = {};
    try { s = JSON.parse(localStorage.getItem('fcuBellSettings') || '{}'); } catch (e) { s = {}; }
    s = Object.assign({ displayName: 'ADMIN', avatar: 'pfp.jpg', appearance: 'light', defaultPeriod: 'AM' }, s);
    var dn = $('settings-display-name'); if (dn) dn.value = s.displayName || '';
    var ap = $('settings-avatar-preview'); if (ap) ap.src = s.avatar || 'pfp.jpg';
    var pa = $('profile-avatar'); if (pa) pa.src = s.avatar || 'pfp.jpg';
    var nm = $('profile-name'); if (nm) nm.textContent = s.displayName || 'ADMIN';
    var sa = $('settings-appearance'); if (sa) sa.value = s.appearance || 'light';
    var dp = $('settings-default-period'); if (dp) dp.value = s.defaultPeriod || 'AM';
    document.body.dataset.appearance = s.appearance || 'light';
    var per = $('schedule-period'); if (per && s.defaultPeriod) per.value = s.defaultPeriod;
  }
  var sAv = $('settings-avatar');
  if (sAv) sAv.addEventListener('change', function () {
    var f = sAv.files[0]; if (!f) return;
    var rd = new FileReader();
    rd.onload = function () {
      var img = new Image();
      img.onload = function () {
        var cv = document.createElement('canvas');
        var sc = Math.min(1, 256 / Math.max(img.width, img.height));
        cv.width = Math.max(1, Math.round(img.width * sc));
        cv.height = Math.max(1, Math.round(img.height * sc));
        cv.getContext('2d').drawImage(img, 0, 0, cv.width, cv.height);
        var pv = $('settings-avatar-preview');
        if (pv) pv.src = cv.toDataURL('image/jpeg', 0.8);
      };
      img.src = rd.result;
    };
    rd.readAsDataURL(f);
  });
  if (sForm) sForm.addEventListener('submit', function (e) {
    e.preventDefault();
    var s = {};
    try { s = JSON.parse(localStorage.getItem('fcuBellSettings') || '{}'); } catch (err) { s = {}; }
    var dn = $('settings-display-name'); if (dn) s.displayName = dn.value.trim() || 'ADMIN';
    var pv = $('settings-avatar-preview'); if (pv) s.avatar = pv.src;
    var sa = $('settings-appearance'); if (sa) s.appearance = sa.value;
    var dp = $('settings-default-period'); if (dp) s.defaultPeriod = dp.value;
    try { localStorage.setItem('fcuBellSettings', JSON.stringify(s)); } catch (err) {}
    loadSettings();
    var m = $('settings-message');
    if (m) { m.textContent = 'Settings saved locally (no firmware endpoint).'; m.className = 'message success'; }
  });

  // ---------- init ----------
  loadSettings();
  probeAuth();
  loadConfig();
  setInterval(loadTelemetry, 5000);
  setInterval(function () {
    var v = $('attendance-view');
    if (v && v.classList.contains('active')) loadAttendance();
    var a = $('activity-view');
    if (a && a.classList.contains('active')) loadActivity();
  }, 10000);
})();
