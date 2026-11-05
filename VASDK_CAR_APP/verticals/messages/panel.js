// Messages on the dashboard: the latest ones, in and out; a message arriving in the simulator.
export default {
  slot: 'left',
  mount(el, car) {
    el.innerHTML = `<h2>Messages <span data-k="unread" class="dim"></span></h2><div data-k="list" style="display:grid;gap:6px;max-height:140px;overflow:auto"></div>
      <div class="row"><button data-k="read">Read new</button><button data-k="write">Write one</button></div>`;
    el.querySelector('[data-k=read]').onclick = () => car.say('read my messages');
    el.querySelector('[data-k=write]').onclick = () => car.say('send a message');
    this.el = el;
  },
  render(m) {
    this.el.querySelector('[data-k=unread]').textContent = m.unread ? `· ${m.unread} new` : '';
    const list = this.el.querySelector('[data-k=list]');
    list.innerHTML = '';
    for (const message of m.messages.slice(-4).reverse()) {
      const div = document.createElement('div');
      div.className = 'card';
      div.style.padding = '8px 10px';
      div.style.opacity = message.read || message.sent ? .7 : 1;
      div.innerHTML = `<div class="dim" style="font-size:12px"></div><div></div>`;
      div.children[0].textContent = (message.sent ? 'To ' : 'From ') + message.from + (!message.sent && !message.read ? ' · new' : '');
      div.children[1].textContent = message.body;
      list.appendChild(div);
    }
    if (!m.messages.length) list.innerHTML = '<div class="dim">No messages yet.</div>';
  },
  simulator(el, car) {
    el.innerHTML = `<h3>Messages</h3><div class="row"><input type="text" value="Sam Carter" style="width:110px">
      <input type="text" value="Running ten minutes late, sorry!" style="flex:1"><button>Message arrives</button></div>`;
    const [from, body] = el.querySelectorAll('input');
    el.querySelector('button').onclick = () => car.simulate({ event: 'message', from: from.value, body: body.value });
  },
};
