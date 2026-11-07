// Climate on the dashboard: both sides' temperature, seats, fan and the toggles; the cabin's
// temperature in the simulator. Every touch is the climate tool, as the agent calls it.
const fmt = (t) => (Math.round(t * 2) / 2).toFixed(1).replace('.0', '');

export default {
  slot: 'left',
  mount(el, car) {
    el.innerHTML = `<h2>Climate <span class="dim" style="text-transform:none;letter-spacing:0">· cabin <b data-k="cabin">--</b>°</span></h2>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px" data-k="zones">
        ${['driver', 'passenger'].map((z) => `<div class="card" style="text-align:center">
          <div class="dim" style="font-size:12px;text-transform:uppercase;letter-spacing:1px">${z}</div>
          <div style="font-size:44px;font-weight:300;font-variant-numeric:tabular-nums"><span data-k="${z}">--</span><sup style="font-size:18px" class="dim">°C</sup></div>
          <div class="row" style="justify-content:center"><button data-t="-0.5" data-z="${z}">−</button><button data-t="0.5" data-z="${z}">+</button></div>
          <div style="margin-top:8px"><button data-seat="${z}">Seat <span data-k="seat_${z}"></span></button></div></div>`).join('')}
      </div>
      <div class="row" style="flex-wrap:nowrap"><button data-fan="-1">−</button>
        <div data-k="fan" style="display:flex;gap:4px;align-items:flex-end;height:26px;flex:1"></div><button data-fan="1">+</button></div>
      <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px">
        ${[['ac', 'A/C'], ['auto', 'Auto'], ['power', 'Power'], ['defrost', 'Defrost'], ['recirculation', 'Recirculate']]
          .map(([k, label]) => `<button data-toggle="${k}">${label}</button>`).join('')}
        <button data-reset>Reset 21°</button>
      </div>`;
    el.querySelectorAll('[data-t]').forEach((b) => b.onclick = () => car.control('adjust_temperature', Number(b.dataset.t), { zone: b.dataset.z }));
    el.querySelectorAll('[data-fan]').forEach((b) => b.onclick = () => car.control('adjust_fan', Number(b.dataset.fan)));
    el.querySelectorAll('[data-toggle]').forEach((b) => b.onclick = () => car.control(b.dataset.toggle, car.state[b.dataset.toggle] ? 'off' : 'on'));
    el.querySelectorAll('[data-seat]').forEach((b) => b.onclick = () =>
      car.control('seat_heat', ((car.state['seat_heat_' + b.dataset.seat] || 0) + 1) % 4, { zone: b.dataset.seat }));
    el.querySelector('[data-reset]').onclick = () => car.control('set_temperature', 21);
    this.el = el;
  },
  render(s) {
    const k = (key) => this.el.querySelector(`[data-k=${key}]`);
    k('cabin').textContent = fmt(s.cabin_temp);
    k('driver').textContent = fmt(s.driver_temp);
    k('passenger').textContent = fmt(s.passenger_temp);
    k('zones').style.opacity = s.power ? 1 : .45;
    for (const z of ['driver', 'passenger']) {
      const level = s['seat_heat_' + z];
      k('seat_' + z).innerHTML = [0, 1, 2].map((i) => `<i style="display:inline-block;width:8px;height:8px;border-radius:50%;margin-left:2px;background:${i < level ? 'var(--warm)' : 'var(--line)'}"></i>`).join('');
      this.el.querySelector(`[data-seat=${z}]`).classList.toggle('on', level > 0);
    }
    k('fan').innerHTML = Array.from({ length: 7 }, (_, i) =>
      `<i style="flex:1;border-radius:3px;height:${30 + i * 10}%;background:${i < s.fan ? 'var(--accent)' : 'var(--line)'}"></i>`).join('');
    this.el.querySelectorAll('[data-toggle]').forEach((b) => b.classList.toggle('on', !!s[b.dataset.toggle]));
    if (this.sim && !this.dragging) this.sim.querySelector('input').value = s.cabin_temp;
    if (this.sim) this.sim.querySelector('b').textContent = fmt(s.cabin_temp) + '°';
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Climate</h3><label>Cabin <input type="range" min="10" max="40" step="0.5"><b></b></label>`;
    const input = el.querySelector('input');
    input.oninput = () => { this.dragging = true; car.simulate({ cabin_temp: Number(input.value) }); };
    input.onchange = () => { this.dragging = false; };
    this.sim = el;
  },
};
