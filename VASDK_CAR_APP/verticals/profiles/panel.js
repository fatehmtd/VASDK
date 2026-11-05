// Profiles on the dashboard: who is driving, in the header; a key fob for each driver in the simulator.
export default {
  slot: 'header',
  mount(el) {
    el.innerHTML = `<span class="stat">Driver <b data-k="driver">Guest</b></span>`;
    this.el = el;
  },
  render(p) {
    this.el.querySelector('[data-k=driver]').textContent = p.driver.name;
    if (!this.sim) return;
    if (this.sim.querySelectorAll('[data-fob]').length !== p.profiles.length) {
      this.sim.querySelector('.chips').innerHTML = p.profiles.map((name) => `<button data-fob="${name}">🔑 ${name}</button>`).join('');
      this.sim.querySelectorAll('[data-fob]').forEach((b) => b.onclick = () => this.car.simulate({ event: 'key_fob', driver: b.dataset.fob }));
    }
    this.sim.querySelectorAll('[data-fob]').forEach((b) => b.classList.toggle('on', b.dataset.fob === p.driver.name));
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Key fob</h3><div class="chips"></div>`;
    this.sim = el;
    this.car = car;
  },
};
