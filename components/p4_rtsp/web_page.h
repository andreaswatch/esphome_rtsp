#pragma once

namespace esphome {
namespace p4_rtsp {

static const char WEB_PAGE_HTML[] = R"P4WMX(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>P4 RTSP Stream Test</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; background: #16181d; color: #e8eaf0; margin: 0; }
  main { max-width: 900px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 1.2rem; }
  video { width: 100%; background: #000; border-radius: 8px; aspect-ratio: 16/9; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; margin: 12px 0; }
  button { padding: 10px 16px; border: 0; border-radius: 8px; background: #2a2f3a; color: #e8eaf0; cursor: pointer; font-size: .95rem; }
  button.on { background: #1f6feb; }
  button:disabled { opacity: .4; cursor: not-allowed; }
  #status { font-size: .85rem; color: #9aa2b1; margin-bottom: 8px; }
  .hint { font-size: .8rem; color: #7c8494; margin-top: 16px; line-height: 1.6; word-break: break-all; }
</style>
</head>
<body>
<main>
  <h1>P4 RTSP Stream &mdash; Browser Test</h1>
  <div id="status">Verbinde&hellip;</div>
  <video id="video" autoplay muted playsinline></video>
  <div class="row">
    <button id="btnVideo" disabled>Video</button>
    <button id="btnMic" disabled>Mic (Geraet -&gt; Browser)</button>
    <button id="btnSpk" disabled>Speaker (Browser -&gt; Geraet)</button>
  </div>
  <div class="hint">
    Hinweise: Diese Seite nutzt kein WebRTC. Das Bild wird als H264 &uuml;ber einen WebSocket geliefert und im
    Browser per <b>jmuxer.js</b> dekodiert. Audio wird als unkomprimiertes PCM16 (16 kHz) &uuml;ber denselben
    WebSocket transportiert. RTSP ist weiterhin auf Port 554 verf&uuml;gbar.<br>
    RTSP-URL: <code>rtsp://<span id="host"></span>:554/p4</code>
  </div>
</main>

<script src="/jmuxer.min.js"></script>
<script>
(function () {
  'use strict';
  var host = location.host;
  document.getElementById('host').textContent = host;

  var video = document.getElementById('video');
  var btnVideo = document.getElementById('btnVideo');
  var btnMic = document.getElementById('btnMic');
  var btnSpk = document.getElementById('btnSpk');
  var status = document.getElementById('status');

  var ws = null;
  var connected = false;
  var videoOn = false, micOn = false, spkOn = false;
  var stream = null;      // getUserMedia stream (browser mic)
  var audioCtx = null;    // playback context
  var playQueue = [];
  var playTimer = null;

  var jmuxer = new JMuxer({
    node: 'video',
    mode: 'video',
    flushingTime: 0,
    fps: 25,
    clearBuffer: true,
    onError: function (err) { status.textContent = 'jmuxer: ' + err; }
  });

  function setStatus(s) { status.textContent = s; }

  function send(text) {
    if (connected && ws.readyState === 1) ws.send(text);
  }

  function connect() {
    var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    ws = new WebSocket(proto + host + '/ws');
    ws.binaryType = 'arraybuffer';

    ws.onopen = function () {
      connected = true;
      setStatus('Verbunden. ' + host);
      enableButtons();
      send('video-on');
    };
    ws.onclose = function () {
      connected = false;
      setStatus('Verbindung getrennt - versuche neu zu verbinden...');
      disableButtons();
      setTimeout(connect, 2000);
    };
    ws.onerror = function () { ws.close(); };
    ws.onmessage = function (ev) {
      if (typeof ev.data === 'string') {
        if (ev.data === 'video-on') { videoOn = true; btnVideo.classList.add('on'); }
        else if (ev.data === 'video-off') { videoOn = false; btnVideo.classList.remove('on'); }
        else if (ev.data === 'audio-on') { micOn = true; btnMic.classList.add('on'); }
        else if (ev.data === 'audio-off') { micOn = false; btnMic.classList.remove('on'); }
        return;
      }
      var buf = new Uint8Array(ev.data);
      if (buf.length < 1) return;
      var tag = buf[0];
      if (tag === 1) {
        jmuxer.feed({ video: buf.subarray(1) });
      } else if (tag === 2) {
        pushAudio(buf.subarray(1));
      }
    };
  }

  function enableButtons() { btnVideo.disabled = false; btnMic.disabled = false; btnSpk.disabled = false; }
  function disableButtons() { btnVideo.disabled = true; btnMic.disabled = true; btnSpk.disabled = true; }
  disableButtons();

  /* ------------------------- Audio-Playback (Geraet -> Browser) ------------------------- */

  function ensurePlaybackCtx() {
    if (audioCtx) return audioCtx;
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
    } catch (e) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    return audioCtx;
  }

  function resample(src, fromRate, toRate) {
    if (fromRate === toRate) return src;
    var ratio = fromRate / toRate;
    var out = new Float32Array(Math.ceil(src.length * toRate / fromRate));
    for (var i = 0; i < out.length; i++) {
      var pos = i * ratio;
      var i0 = Math.floor(pos);
      var i1 = Math.min(i0 + 1, src.length - 1);
      var frac = pos - i0;
      out[i] = src[i0] * (1 - frac) + src[i1] * frac;
    }
    return out;
  }

  function pushAudio(bytes) {
    if (micOn === false) return;
    var ctx = ensurePlaybackCtx();
    var ints = new Int16Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 2);
    var float = new Float32Array(ints.length);
    for (var i = 0; i < ints.length; i++) float[i] = ints[i] / 32768;
    playQueue.push(resample(float, 16000, ctx.sampleRate));
    schedulePlay();
  }

  var nextTime = 0;
  function schedulePlay() {
    if (playTimer) return;
    playTimer = setInterval(function () {
      if (!playQueue.length) { clearInterval(playTimer); playTimer = null; return; }
      var ctx = ensurePlaybackCtx();
      if (ctx.state === 'suspended') ctx.resume();
      while (playQueue.length && (nextTime - ctx.currentTime) < 0.25) {
        var chunk = playQueue.shift();
        var buffer = ctx.createBuffer(1, chunk.length, ctx.sampleRate);
        buffer.copyToChannel(chunk, 0);
        var src = ctx.createBufferSource();
        src.buffer = buffer;
        src.connect(ctx.destination);
        var when = Math.max(nextTime, ctx.currentTime + 0.05);
        src.start(when);
        nextTime = when + chunk.length / ctx.sampleRate;
      }
    }, 40);
  }

  /* ------------------------- Mikrofon senden (Browser -> Geraet) ------------------------- */

  function startSpeaker() {
    var constraints = {
      audio: { sampleRate: 16000, channelCount: 1, echoCancellation: false,
               noiseSuppression: false, autoGainControl: false }
    };
    return navigator.mediaDevices.getUserMedia(constraints).then(function (s) {
      stream = s;
      var ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
      var src = ctx.createMediaStreamSource(stream);
      var proc = ctx.createScriptProcessor(1024, 1, 1);
      proc.onaudioprocess = function (e) {
        var input = e.inputBuffer.getChannelData(0);
        var out = new Int16Array(input.length);
        for (var i = 0; i < input.length; i++) out[i] = Math.max(-32768, Math.min(32767, input[i] * 32768));
        send(out.buffer);
        e.outputBuffer.getChannelData(0).fill(0);
      };
      src.connect(proc);
      proc.connect(ctx.destination);
    });
  }

  function stopSpeaker() {
    if (stream) { stream.getTracks().forEach(function (t) { t.stop(); }); stream = null; }
  }

  /* ------------------------- Buttons ------------------------- */

  btnVideo.onclick = function () {
    if (videoOn) { send('video-off'); videoOn = false; btnVideo.classList.remove('on'); }
    else { send('video-on'); }
  };

  btnMic.onclick = function () {
    if (micOn) { send('audio-off'); micOn = false; btnMic.classList.remove('on'); }
    else { send('audio-on'); }
  };

  btnSpk.onclick = function () {
    if (spkOn) {
      send('speaker-off');
      spkOn = false;
      btnSpk.classList.remove('on');
      stopSpeaker();
    } else {
      startSpeaker().then(function () {
        spkOn = true;
        btnSpk.classList.add('on');
        send('speaker-on');
      }).catch(function (err) {
        setStatus('Mikrofon-Zugriff verweigert: ' + err.message);
      });
    }
  };

  connect();
})();
</script>
</body>
</html>
)P4WMX";

}  // namespace p4_rtsp
}  // namespace esphome
