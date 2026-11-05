// Media on the dashboard: the source, what's playing, the transport, the volume, stations and
// genres. Every touch is the media tool, as the agent calls it.
const colors = { jazz: '#7b4bd6', rock: '#c2412d', pop: '#e0459b', classical: '#2f8f83', chill: '#3d6fd6', news: '#5b6b7d' };
const stations = [['coastal_fm', 'Coastal FM 101.5'], ['metro_news', 'Metro News 94.1'], ['highway_rock', 'Highway Rock 104.7'], ['velvet_jazz', 'Velvet Jazz 88.3']];
const genres = ['jazz', 'rock', 'pop', 'classical', 'chill'];

export default {
  slot: 'center',
  mount(el, car) {
    el.innerHTML = `<h2>Media</h2>
      <div class="row">${['radio', 'bluetooth', 'usb'].map((s) => `<button data-source="${s}">${s === 'usb' ? 'USB' : s[0].toUpperCase() + s.slice(1)}</button>`).join('')}</div>
      <div class="card" style="display:grid;grid-template-columns:84px 1fr;gap:14px;align-items:center">
        <div data-k="cover" style="width:84px;height:84px;border-radius:10px;display:grid;place-items:center;font-size:28px;font-weight:700"></div>
        <div><div class="big" data-k="title">—</div><div class="dim" data-k="artist"></div>
          <div style="height:4px;background:var(--line);border-radius:2px;margin-top:10px;overflow:hidden"><i data-k="progress" style="display:block;height:100%;background:var(--accent);width:0"></i></div></div>
      </div>
      <div class="row" style="justify-content:center">
        <button data-m="previous">⏮</button><button data-k="play" style="width:60px;height:60px;border-radius:50%;background:var(--accent);color:#04121d;border:none">▶</button><button data-m="next">⏭</button>
        <button data-k="video">🎬 Video</button></div>
      <div style="display:grid;grid-template-columns:auto 1fr 32px;gap:10px;align-items:center">
        <button data-k="mute">Mute</button><input type="range" data-k="volume" min="0" max="30"><b data-k="vol">--</b></div>
      <div class="chips">${stations.map(([id, label]) => `<button data-station="${id}">${label}</button>`).join('')}</div>
      <div class="chips">${genres.map((g) => `<button data-genre="${g}">${g[0].toUpperCase() + g.slice(1)}</button>`).join('')}</div>`;
    const k = (key) => el.querySelector(`[data-k=${key}]`);
    el.querySelectorAll('[data-source]').forEach((b) => b.onclick = () => car.control('source', b.dataset.source));
    el.querySelectorAll('[data-m]').forEach((b) => b.onclick = () => car.control(b.dataset.m));
    el.querySelectorAll('[data-station]').forEach((b) => b.onclick = () => car.control('station', b.dataset.station));
    el.querySelectorAll('[data-genre]').forEach((b) => b.onclick = () => car.control('genre', b.dataset.genre));
    k('play').onclick = () => car.control(car.state.playing ? 'pause' : 'play');
    k('mute').onclick = () => car.control('mute', car.state.muted ? 'off' : 'on');
    k('video').onclick = () => car.say(car.state.video ? 'stop the video' : 'play a video');   // through the agent: its parked-only rule
    k('volume').oninput = () => { this.dragging = true; car.control('volume', Number(k('volume').value)); };
    k('volume').onchange = () => { this.dragging = false; };
    this.el = el;
  },
  render(m) {
    const k = (key) => this.el.querySelector(`[data-k=${key}]`);
    this.el.querySelectorAll('[data-source]').forEach((b) => b.classList.toggle('on', b.dataset.source === m.source));
    this.el.querySelectorAll('[data-station]').forEach((b) => b.classList.toggle('on', m.source === 'radio' && b.dataset.station === m.station));
    this.el.querySelectorAll('[data-genre]').forEach((b) => b.classList.toggle('on', m.source !== 'radio' && b.dataset.genre === m.genre));
    const radio = m.source === 'radio';
    k('title').textContent = m.video ? 'Video' : radio ? m.station_name : m.title;
    k('artist').textContent = m.on_call ? 'paused for the call' : radio ? 'FM ' + m.frequency : `${m.artist} · ${m.source_name}`;
    k('progress').style.width = radio ? '0' : (100 * m.position_s / m.duration_s) + '%';
    k('cover').textContent = radio ? m.frequency : m.title.split(' ').map((w) => w[0]).slice(0, 2).join('');
    k('cover').style.background = colors[m.genre] || '#334';
    k('cover').style.opacity = m.playing ? 1 : .5;
    k('play').textContent = m.playing ? '⏸' : '▶';
    k('video').classList.toggle('on', m.video);
    if (!this.dragging) k('volume').value = m.volume;
    k('vol').textContent = m.muted ? '—' : m.volume;
    k('mute').classList.toggle('on', m.muted);
    this.el.querySelectorAll('[data-m]').forEach((b) => b.disabled = m.on_call);
  },
};
