// Drive on the dashboard: speed, gear and fuel in the header; the road in the simulator.
const fuelBar = (percent) => `<span style="display:inline-block;width:70px;height:8px;border-radius:4px;background:var(--line);vertical-align:middle;overflow:hidden">` +
  `<i style="display:block;height:100%;width:${percent}%;background:${percent < 15 ? 'var(--bad)' : 'var(--ok)'}"></i></span>`;

export default {
  slot: 'header',
  mount(el) {
    el.innerHTML = `<span class="stat"><b data-k="gear">P</b> <b data-k="speed">0</b> km/h</span>
      <span class="stat">Outside <b data-k="outside">--</b>°</span>
      <span class="stat">Fuel <span data-k="fuel"></span> <b data-k="range">--</b> km</span>`;
    this.el = el;
  },
  render(s) {
    const k = (key) => this.el.querySelector(`[data-k=${key}]`);
    k('gear').textContent = s.gear;
    k('speed').textContent = s.speed_kmh;
    k('outside').textContent = s.outside_temp;
    k('fuel').innerHTML = fuelBar(s.fuel_percent);
    k('range').textContent = s.range_km;
    if (this.sim && this.dragging !== 'speed') this.sim.querySelector('[data-s=speed]').value = s.speed_kmh;
    if (this.sim && this.dragging !== 'fuel') this.sim.querySelector('[data-s=fuel]').value = s.fuel_percent;
    if (this.sim && this.dragging !== 'outside') this.sim.querySelector('[data-s=outside]').value = s.outside_temp;
    if (this.sim) {
      this.sim.querySelectorAll('[data-gear]').forEach((b) => b.classList.toggle('on', b.dataset.gear === s.gear));
      this.sim.querySelector('[data-v=speed]').textContent = s.speed_kmh + ' km/h';
      this.sim.querySelector('[data-v=fuel]').textContent = s.fuel_percent + '%';
      this.sim.querySelector('[data-v=outside]').textContent = s.outside_temp + '°';
    }
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Drive</h3>
      <div class="row chips">${['P', 'R', 'N', 'D'].map((g) => `<button data-gear="${g}">${g}</button>`).join('')}</div>
      <label>Speed <input type="range" data-s="speed" min="0" max="160" step="5"><b data-v="speed"></b></label>
      <label>Fuel <input type="range" data-s="fuel" min="0" max="100"><b data-v="fuel"></b></label>
      <label>Outside <input type="range" data-s="outside" min="-10" max="40"><b data-v="outside"></b></label>`;
    el.querySelectorAll('[data-gear]').forEach((b) => b.onclick = () => car.simulate({ gear: b.dataset.gear }));
    const fields = { speed: 'speed_kmh', fuel: 'fuel_percent', outside: 'outside_temp' };
    for (const [key, field] of Object.entries(fields)) {
      const input = el.querySelector(`[data-s=${key}]`);
      input.oninput = () => { this.dragging = key; car.simulate({ [field]: Number(input.value), ...(key === 'speed' && Number(input.value) > 0 ? { gear: 'D' } : {}) }); };
      input.onchange = () => { this.dragging = null; };
    }
    this.sim = el;
  },
};
