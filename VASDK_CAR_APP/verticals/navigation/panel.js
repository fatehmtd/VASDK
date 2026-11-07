// Navigation on the dashboard: places to go, the three routes while one is being chosen (the
// agent's "routes" card), and the guidance: what's left, the ETA, the stops.
const places = [['home', 'Home'], ['work', 'Work'], ['the mall', 'Harbor Mall'], ['the airport', 'City Airport'], ['the cafe', 'Pine Street Cafe'], ['a gas station', 'Gas']];

export default {
  slot: 'center',
  mount(el, car) {
    el.innerHTML = `<h2>Navigation</h2>
      <div data-k="status" class="card"></div>
      <div data-k="options" style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px"></div>
      <div class="chips">${places.map(([said, label]) => `<button data-go="${said}">${label}</button>`).join('')}<button data-k="stop">Stop</button></div>`;
    el.querySelectorAll('[data-go]').forEach((b) => b.onclick = () => car.say('take me to ' + b.dataset.go));   // through the agent: it offers the routes
    el.querySelector('[data-k=stop]').onclick = () => car.say('stop navigation');
    this.el = el;
    this.car = car;
  },
  render(n) {
    const status = this.el.querySelector('[data-k=status]');
    if (n.state === 'guiding') {
      status.innerHTML = `<div class="big"></div><div class="dim"></div>`;
      status.children[0].textContent = `${n.eta_min} min · ${n.remaining_km} km to ${n.destination_name}`;
      status.children[1].textContent = `by ${n.via}` + (n.stops.length ? ` · stop: ${n.stops.join(', ')}` : '');
    } else if (n.state === 'arrived') {
      status.innerHTML = `<div class="big"></div>`;
      status.children[0].textContent = `Arrived at ${n.destination_name}`;
    } else if (n.state === 'planned') {
      status.innerHTML = `<div class="big"></div><div class="dim">Pick a route, or say “the second one”.</div>`;
      status.children[0].textContent = `To ${n.destination_name}`;
    } else {
      status.innerHTML = `<div class="dim">Not navigating. Say “take me home”.</div>`;
    }
    const options = this.el.querySelector('[data-k=options]');
    options.innerHTML = '';
    if (n.state === 'planned') {
      n.options.forEach((o, i) => {
        const b = document.createElement('button');
        b.style.textAlign = 'left';
        b.innerHTML = `<div class="dim" style="font-size:12px">${i + 1}</div><b></b><div class="dim" style="font-size:12px"></div>`;
        b.children[1].textContent = `${o.minutes} min`;
        b.children[2].textContent = `${o.km} km · ${o.via}`;
        b.onclick = () => this.car.say(o.id.replace('_', ' '));   // as if said: the conversation moves on
        options.appendChild(b);
      });
    }
    this.el.querySelector('[data-k=stop]').disabled = n.state === 'idle';
  },
  onReply(reply, el) {   // the agent's "routes" card: draw attention to the options
    if (reply.ui && reply.ui.card === 'routes') { el.classList.remove('flash'); void el.offsetWidth; el.classList.add('flash'); }
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Navigation</h3><div class="row"><button>Almost there</button><button>Arrive</button></div>`;
    const [almost, arrive] = el.querySelectorAll('button');
    almost.onclick = () => car.simulate({ remaining_km: 0.5 });
    arrive.onclick = () => car.simulate({ remaining_km: 0 });
  },
};
