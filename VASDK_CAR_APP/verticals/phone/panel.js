// The phone on the dashboard: the contacts to call, a banner while a call rings or is on, and an
// incoming call in the simulator. Answering goes through the conversation, so the agent knows.
export default {
  slot: 'left',
  mount(el, car) {
    el.innerHTML = `<h2>Phone</h2><div class="chips" data-k="contacts"></div>
      <div data-k="call" style="display:none;position:fixed;left:50%;top:18px;transform:translateX(-50%);background:#102a1f;border:1px solid var(--ok);
        border-radius:16px;padding:14px 18px;align-items:center;gap:16px;box-shadow:0 10px 40px rgba(0,0,0,.5);z-index:5">
        <div><div class="big" data-k="who"></div><div class="dim" data-k="what"></div></div>
        <button data-k="answer" style="background:var(--ok);color:#05150e;border:none">Answer</button>
        <button data-k="end" style="background:var(--bad);color:#1a0505;border:none">Decline</button></div>`;
    const k = (key) => el.querySelector(`[data-k=${key}]`);
    k('answer').onclick = () => car.say('answer');
    k('end').onclick = () => car.say(car.state.state === 'ringing' ? 'decline' : 'hang up');
    this.el = el;
    this.car = car;
  },
  render(p) {
    const k = (key) => this.el.querySelector(`[data-k=${key}]`);
    if (k('contacts').childElementCount !== p.contacts.length) {
      k('contacts').innerHTML = '';
      for (const name of p.contacts) {
        const b = document.createElement('button');
        b.textContent = '📞 ' + name;
        b.onclick = () => this.car.say('call ' + name);
        k('contacts').appendChild(b);
      }
    }
    k('contacts').querySelectorAll('button').forEach((b) => b.disabled = p.state !== 'idle');
    k('call').style.display = p.state === 'idle' ? 'none' : 'flex';
    k('who').textContent = p.caller;
    k('what').textContent = p.state === 'ringing' ? 'Incoming call' : p.direction === 'outgoing' ? 'Calling…' : 'On a call';
    k('answer').style.display = p.state === 'ringing' ? '' : 'none';
    k('end').textContent = p.state === 'ringing' ? 'Decline' : 'Hang up';
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Phone</h3><div class="row"><input type="text" value="Mom" style="width:120px"><button>Incoming call</button></div>`;
    el.querySelector('button').onclick = () => car.simulate({ event: 'incoming_call', caller: el.querySelector('input').value || 'Mom' });
  },
};
