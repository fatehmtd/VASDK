// The dashboard's shell: the connection to the car, the conversation, talking over WebRTC, and the
// layout. Everything about a feature is in its vertical's panel (verticals/<name>/panel.js):
//
//   export default {
//     slot: 'left' | 'center' | 'right' | 'header',   // where it goes
//     mount(el, car) {},          // draw it in el; car: { control(action, value, extra), simulate(body), say(text), state }
//     render(state, all) {},      // its part of the car's state, and all of it
//     simulator(el, car) {},      // optional: its controls in the simulator
//     onReply(reply) {},          // optional: what the assistant said, with its ui card ({card: "routes"})
//   }

const $ = (id) => document.getElementById(id);
const post = (path, body) => fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
  .then((r) => r.json()).catch(() => ({}));
const say = (text) => post('/say', { text });
let all = {};
const panels = [];

function carFor(name) {
  return {
    control: (action, value, extra = {}) => post('/control', { vertical: name, action, value, ...extra }),
    simulate: (body) => post('/simulate', { vertical: name, ...body }),
    say,
    get state() { return all[name] || {}; },
  };
}

async function loadPanels(names) {
  for (const name of names) {
    let panel;
    try { panel = (await import(`/verticals/${name}/panel.js`)).default; } catch (e) { console.warn(name, 'has no panel', e); continue; }
    const car = carFor(name);
    const el = document.createElement(panel.slot === 'header' ? 'span' : 'section');
    el.className = panel.slot === 'header' ? 'row' : 'panel';
    el.dataset.vertical = name;
    (panel.slot === 'header' ? $('headerSlot') : $(panel.slot || 'center')).appendChild(el);
    panel.mount(el, car);
    if (panel.simulator) {
      const sim = document.createElement('div');
      sim.className = 'sim';
      $('simulator').appendChild(sim);
      panel.simulator(sim, car);
    }
    panels.push({ name, panel, el });
  }
  render(all);
}

function render(state) {
  all = state;
  for (const { name, panel } of panels) if (state[name]) panel.render(state[name], state);
}

//  The conversation
function addMessage(kind, text, meta) {
  const div = document.createElement('div');
  div.className = 'msg ' + kind;
  div.textContent = text;
  if (meta) { const span = document.createElement('span'); span.className = 'meta'; span.textContent = meta; div.appendChild(span); }
  $('convo').appendChild(div);
  $('convo').scrollTop = $('convo').scrollHeight;
  return div;
}
function showChoices(labels, values) {
  $('choices').innerHTML = '';
  (labels || []).forEach((label, i) => {
    const b = document.createElement('button');
    b.textContent = label;
    b.onclick = () => say((values && values[i]) || label);
    $('choices').appendChild(b);
  });
}
let lastUser = null;
function onEvent(event) {
  switch (event.type) {
    case 'state': render(event.state); break;
    case 'hearing': $('hearing').textContent = event.text ? '“' + event.text + '…”' : ''; break;
    case 'user': lastUser = addMessage('user', event.text); showChoices([]); break;
    case 'assistant':
      addMessage('assistant', event.text);
      showChoices(event.choices, event.choice_values);
      for (const { panel, el } of panels) if (panel.onReply) panel.onReply(event, el);
      break;
    case 'understood':
      if (lastUser && !lastUser.dataset.understood) {
        lastUser.dataset.understood = 1;
        const span = document.createElement('span'); span.className = 'meta';
        span.textContent = event.matched ? `${event.intent || 'understood'} · ${event.page}` : 'not understood by the agent';
        lastUser.appendChild(span);
      }
      break;
    case 'agent':
      $('orb').className = 'orb ' + (event.state || '');
      $('agentState').textContent = (event.state || 'assistant').toLowerCase().replace(/_/g, ' ');
      break;
    case 'trigger': if (event.decision !== 'STARTED') addMessage('note', `${event.name}: ${event.decision.toLowerCase()}`); break;
    case 'provider': addMessage('note', `${event.kind}: ${event.from} → ${event.to} (${event.reason})`); break;
    case 'error': addMessage('note', `${event.source}: ${event.message}`); break;
  }
}
$('sayForm').onsubmit = (e) => {
  e.preventDefault();
  const text = $('sayText').value.trim();
  if (text) say(text);
  $('sayText').value = '';
};
$('stopReply').onclick = () => post('/stop', {});

//  Talking from this browser: WebRTC to the car (VASDK_Car --webrtc)
let pc = null;
function hangUp(status) {
  if (pc) { pc.getSenders().forEach((s) => s.track && s.track.stop()); pc.close(); pc = null; }
  $('talk').classList.remove('live');
  $('talk').textContent = '🎙 Talk';
  $('talkStatus').textContent = status || '';
}
async function talk() {
  if (pc) return hangUp();
  $('talk').disabled = true;
  try {
    const mic = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
    pc = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.l.google.com:19302' }] });
    mic.getTracks().forEach((t) => pc.addTrack(t, mic));
    pc.ontrack = (e) => { $('carVoice').srcObject = e.streams[0] || new MediaStream([e.track]); };
    pc.onconnectionstatechange = () => {
      const st = pc && pc.connectionState;
      if (st === 'connected') { $('talk').classList.add('live'); $('talk').textContent = '● Hang up'; $('talkStatus').textContent = ''; }
      if (st === 'failed' || st === 'closed') hangUp('connection ' + st);
    };
    pc.createDataChannel('events');
    await pc.setLocalDescription(await pc.createOffer());
    $('talkStatus').textContent = 'connecting…';
    await new Promise((resolve) => {   // one answer, no trickle ICE: send every candidate with the offer
      if (pc.iceGatheringState === 'complete') return resolve();
      pc.onicegatheringstatechange = () => pc.iceGatheringState === 'complete' && resolve();
      setTimeout(resolve, 3000);
    });
    const res = await fetch('/offer', { method: 'POST', headers: { 'Content-Type': 'application/sdp' }, body: pc.localDescription.sdp });
    if (!res.ok) throw new Error(res.status === 409 ? 'someone is already talking to the car' : 'the car said ' + res.status);
    await pc.setRemoteDescription({ type: 'answer', sdp: await res.text() });
  } catch (err) {
    hangUp(err.message);
  } finally {
    $('talk').disabled = false;
  }
}
$('talk').onclick = talk;

//  Start
function connect() {
  fetch('/state').then((r) => r.json()).then(render).catch(() => {});
  const events = new EventSource('/events');
  events.onopen = () => $('offline').classList.remove('show');
  events.onmessage = (m) => { try { onEvent(JSON.parse(m.data)); } catch (e) { console.error(e); } };
  events.onerror = () => { $('offline').classList.add('show'); events.close(); setTimeout(() => { $('convo').innerHTML = ''; connect(); }, 2000); };
}
const tick = () => { $('clock').textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }); };
tick();
setInterval(tick, 1000);
fetch('/info').then((r) => r.json()).then(async (info) => {
  if (info.webrtc) $('talk').style.display = 'inline-block';
  await loadPanels(info.verticals || []);
  connect();
});
