/* mtrxAI web GUI front-end.
   Owns: SSE stream, log rendering, approval/proposal cards, slash commands,
   theme toggle. Every action maps to the same core call the REPL uses. */

(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const logEl = $("log"), cardsEl = $("cards"), taskEl = $("task");
  const state = { agents: [], router: "router", busy: false, pending: null };

  /* ------------------------------------------------------------ theme */

  function applyTheme(name) {
    document.documentElement.dataset.theme = name;
    localStorage.setItem("mtrxai-theme", name);
  }
  applyTheme(localStorage.getItem("mtrxai-theme") || "dark");
  $("btn-theme").addEventListener("click", () => {
    applyTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark");
  });

  /* -------------------------------------------------------------- log */

  const esc = (s) => String(s ?? "");
  const short = (s, n) => (esc(s).length <= n ? esc(s) : esc(s).slice(0, n - 1) + "…");

  function row(cls, html) {
    const d = document.createElement("div");
    d.className = "row " + cls;
    d.innerHTML = html;
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
    return d;
  }

  function line(text) { row("status", esc(text)); }
  function errorLine(text) { row("error", esc(text)); }

  function userMessage(text) {
    const d = document.createElement("div");
    d.className = "row user";
    d.textContent = text;
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
  }

  function answer(agent, text) {
    const d = document.createElement("div");
    d.className = "row answer";
    const who = document.createElement("span");
    who.className = "who";
    who.textContent = agent;
    const body = document.createElement("span");
    body.className = "body";
    body.textContent = text;
    d.append(who, body);
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
  }

  function meta(tags, cls) {
    const d = document.createElement("div");
    d.className = "row meta " + (cls || "");
    tags.forEach(([txt, tcls]) => {
      const s = document.createElement("span");
      s.className = "tag " + (tcls || "");
      s.textContent = txt;
      d.appendChild(s);
    });
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
  }

  /* ----------------------------------------------------------- events */

  function renderEvent(evt) {
    switch (evt.type) {
      case "routed":
        meta([["⚡ routed → " + evt.agent], [evt.model || "", "accent"]], "route");
        break;
      case "routing_fallback":
        meta([["route fallback"], [String(evt.reason || "").slice(0, 90), ""]]);
        break;
      case "proposal":
        line("✦ router proposes a new agent");
        break;
      case "agent_created":
        meta([["✓ agent created"], [evt.name], [evt.model || "", "accent"]]);
        refreshState();
        break;
      case "agent_declined":
        line("agent creation declined");
        break;
      case "tool_call":
        meta([["🔧 " + evt.agent + " → " + evt.tool],
              [JSON.stringify(evt.args || {}).slice(0, 160), ""]], "tool");
        break;
      case "tool_denied":
        row("denied", "⊘ " + esc(evt.agent) + " · " + esc(evt.tool) + " denied");
        break;
      case "tool_result":
        row("toolresult", "↳ " + short(evt.result, 400));
        break;
      case "status":
        line(evt.text || "");
        break;
      case "pull":
        line("⇣ " + String(evt.line || "").slice(0, 120));
        break;
      case "error":
        errorLine(String(evt.text || "").slice(0, 400));
        break;
      case "answer":
        answer(evt.agent || "mtrxAI", evt.text || "");
        break;
      default:
        break;
    }
  }

  /* ------------------------------------------------------------- cards */

  function clearCard(id) {
    const el = document.getElementById(id);
    if (el) el.remove();
    if (state.pending === id) state.pending = null;
  }

  function cardShell(card) {
    const el = document.createElement("section");
    el.className = "card" + (card.card === "confirm" ? " warn" : "");
    el.id = card.id;
    cardsEl.appendChild(el);
    el.scrollIntoView({ block: "nearest" });
    return el;
  }

  function keysHint(el, pairs) {
    const k = document.createElement("div");
    k.className = "keys";
    pairs.forEach(([key, what]) => {
      const kb = document.createElement("kbd");
      kb.textContent = key;
      const sp = document.createElement("span");
      sp.textContent = what;
      const wrap = document.createElement("span");
      wrap.append(kb, document.createTextNode(" "), sp);
      k.appendChild(wrap);
    });
    el.appendChild(k);
  }

  async function replyCard(id, payload) {
    clearCard(id);
    try {
      await fetch("/api/card/" + encodeURIComponent(id), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
    } catch (e) {
      errorLine("could not deliver decision: " + e);
    }
  }

  function approvalCard(card) {
    const el = cardShell(card);
    el.innerHTML = "<h3>approval</h3>";
    const lead = document.createElement("p");
    lead.className = "lead";
    lead.textContent = card.title;
    const pre = document.createElement("pre");
    pre.textContent = card.detail || "{}";
    const actions = document.createElement("div");
    actions.className = "actions";
    const mk = (label, cls, payload) => {
      const b = document.createElement("button");
      b.className = "btn " + cls;
      b.textContent = label;
      b.addEventListener("click", () => replyCard(card.id, payload));
      actions.appendChild(b);
    };
    mk("allow once", "primary", { answer: "yes" });
    mk("always this session", "accent-outline", { answer: "always" });
    mk("deny", "danger", { answer: "no" });
    el.append(lead, pre, actions);
    keysHint(el, [["y", "allow"], ["a", "always"], ["n", "deny"]]);
    el.dataset.keys = "ya n";
  }

  function confirmCard(card) {
    const el = cardShell(card);
    el.innerHTML = "<h3>confirm</h3>";
    const lead = document.createElement("p");
    lead.className = "lead";
    lead.textContent = card.title + " " + (card.detail || "");
    const actions = document.createElement("div");
    actions.className = "actions";
    const yes = document.createElement("button");
    yes.className = "btn primary";
    yes.textContent = "yes";
    yes.addEventListener("click", () => replyCard(card.id, { answer: "yes" }));
    const no = document.createElement("button");
    no.className = "btn ghost";
    no.textContent = "no";
    no.addEventListener("click", () => replyCard(card.id, { answer: "no" }));
    actions.append(yes, no);
    el.append(lead, actions);
    keysHint(el, [["y", "yes"], ["n", "no"]]);
  }

  function proposalCard(card) {
    const p = card.proposal || {}, model = card.default_model || "";
    const el = cardShell(card);
    el.innerHTML = "<h3>proposed new agent</h3>";
    const lead = document.createElement("p");
    lead.className = "lead";
    lead.textContent = "Review the router's proposal — edit anything before creating.";
    const grid = document.createElement("div");
    grid.className = "grid";
    const field = (label, value, ph) => {
      const l = document.createElement("label");
      l.textContent = label;
      const i = document.createElement("input");
      i.value = value ?? "";
      if (ph) i.placeholder = ph;
      i.dataset.k = label;
      grid.append(l, i);
      return i;
    };
    field("name", p.name, "kebab-case");
    field("description", p.description, "one-line description");
    field("model", model, "provider:model");
    field("tools", (p.tools || []).join(", "), "comma-separated");
    const msg = document.createElement("p");
    msg.className = "msg";
    const actions = document.createElement("div");
    actions.className = "actions";
    const submit = async (cancel) => {
      if (cancel) return replyCard(card.id, { action: "cancel" });
      const val = (k) => grid.querySelector('[data-k="' + k + '"]').value.trim();
      const name = val("name");
      if (!/^[a-z0-9][a-z0-9-]*$/.test(name)) {
        msg.textContent = "name must be kebab-case, e.g. data-analyst";
        return;
      }
      if (!val("description") || !val("model")) {
        msg.textContent = "description and model are required";
        return;
      }
      await replyCard(card.id, {
        action: "create",
        name,
        description: val("description"),
        model: val("model"),
        tools: val("tools").split(",").map((t) => t.trim()).filter(Boolean),
      });
    };
    const create = document.createElement("button");
    create.className = "btn primary";
    create.type = "button";
    create.textContent = "create agent";
    create.addEventListener("click", () => submit(false));
    const cancel = document.createElement("button");
    cancel.className = "btn ghost";
    cancel.type = "button";
    cancel.textContent = "cancel";
    cancel.addEventListener("click", () => submit(true));
    actions.append(cancel, create);
    const hint = document.createElement("p");
    hint.className = "hint";
    hint.textContent = "Writes agents/<name>.yaml and registers it immediately.";
    el.append(lead, grid, msg, actions, hint);
  }

  function renderCard(card) {
    if (document.getElementById(card.id)) return;   // replay-safe
    state.pending = card.id;
    if (card.card === "approval") approvalCard(card);
    else if (card.card === "confirm") confirmCard(card);
    else if (card.card === "agent_proposal") proposalCard(card);
  }

  /* ------------------------------------------------------------ state */

  function renderAgents() {
    const ul = $("agent-list");
    ul.innerHTML = "";
    state.agents.forEach((a) => {
      const li = document.createElement("li");
      li.className = "agent" + (a.router ? " router" : "");
      const name = document.createElement("div");
      name.className = "name";
      const dot = document.createElement("span");
      dot.className = "dot";
      dot.textContent = "●";
      name.append(dot, document.createTextNode(a.name));
      if (a.router) {
        const b = document.createElement("span");
        b.className = "badge";
        b.textContent = "router";
        name.appendChild(b);
      }
      const model = document.createElement("div");
      model.className = "model";
      model.textContent = a.model;
      const tools = document.createElement("div");
      tools.className = "tools";
      tools.textContent = (a.tools && a.tools.length) ? a.tools.join(" · ") : "no tools";
      li.append(name, model, tools);
      li.title = a.description || "";
      ul.appendChild(li);
    });
  }

  function renderRuntime() {
    const dl = $("runtime-info");
    dl.innerHTML = "";
    (state.info || []).forEach(([k, v]) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      dd.title = v;
      dl.append(dt, dd);
    });
  }

  function setBusy(busy) {
    state.busy = busy;
    $("btn-send").disabled = busy;
    $("btn-stop").hidden = !busy;
    const left = $("status-left");
    left.textContent = busy ? "⟳ working…" : "idle";
    left.className = busy ? "working" : "";
  }

  function applySnapshot(s) {
    if (!s) return;
    state.agents = s.agents || [];
    state.router = s.router || "router";
    state.info = [
      ["version", "v" + s.version],
      ["project", s.project_root || ""],
      ["workspace", s.workspace || ""],
      ["permission", s.default_permission || "ask"],
      ["default", s.default_model || ""],
      ["voice", s.voice && s.voice.enabled ? "on" : "off"],
    ];
    $("ver").textContent = "v" + s.version;
    $("status-right").textContent = (s.workspace || "") + "  ·  " + (s.default_permission || "");
    renderAgents();
    renderRuntime();
    setBusy(!!s.busy);
    (s.warnings || []).forEach((w) => errorLine("⚠ " + w));
  }

  async function refreshState() {
    try {
      const r = await fetch("/api/state");
      applySnapshot(await r.json());
    } catch (e) { /* server gone; the SSE 'down' indicator covers it */ }
  }

  /* --------------------------------------------------------------- SSE */

  function connect() {
    const es = new EventSource("/api/events");
    const conn = $("conn");
    es.onopen = () => { conn.className = "conn live"; conn.title = "event stream: live"; };
    es.onerror = () => { conn.className = "conn down"; conn.title = "event stream: reconnecting…"; };
    es.onmessage = (m) => {
      let d;
      try { d = JSON.parse(m.data); } catch (e) { return; }
      if (d.kind === "event") renderEvent(d.evt || {});
      else if (d.kind === "card") renderCard(d);
      else if (d.kind === "state") applySnapshot(d.state);
    };
  }

  /* -------------------------------------------------------- task submit */

  async function send(text) {
    const value = (text ?? taskEl.value).trim();
    if (!value) return;
    if (!text) taskEl.value = "";
    if (value.startsWith("/")) {
      userMessage(value);
      return slash(value);
    }
    userMessage(value);
    setBusy(true);
    try {
      const r = await fetch("/api/task", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ text: value }),
      });
      if (!r.ok) {
        const e = await r.json().catch(() => ({}));
        errorLine(e.error || ("request failed: " + r.status));
        setBusy(false);
      }
    } catch (e) {
      errorLine("could not reach the server: " + e);
      setBusy(false);
    }
  }

  const LOCAL = {
    "/help": () => {
      line("type a task to route it · /agents · /swap <agent> <model> · /reload · " +
           "/create [name] · /trace · /quit · theme button toggles dark/light");
    },
    "/agents": () => {
      state.agents.forEach((a) =>
        meta([[a.name + (a.router ? " (router)" : "")], [a.model, "accent"],
              [(a.tools || []).join(", ") || "no tools", ""]]));
    },
    "/reload": () => post("/api/reload"),
    "/trace": () => showTrace(),
    "/create": () => {
      // '/create <name>' pre-seeds the flow like the REPL; bare '/create' opens the wizard
      const name = (window.__slashValue || "").split(" ").slice(1).join(" ").trim();
      if (name) return post("/api/create", { name }).then(refreshState);
      return showCreate();
    },
    "/swap": () => showSwap(),
    "/quit": () => post("/api/shutdown"),
  };

  async function post(url, body) {
    try {
      const r = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body || {}),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) errorLine(d.error || ("request failed: " + r.status));
      else {
        if (d.message) line("✓ " + d.message);
        (d.warnings || []).forEach((w) => errorLine("⚠ " + w));
        if (d.state) applySnapshot(d.state);
        else if (d.ok) refreshState();
      }
      return d;
    } catch (e) {
      errorLine("request failed: " + e);
      return {};
    }
  }

  function slash(value) {
    const cmd = value.split(" ")[0].toLowerCase();
    const hasArgs = value.trim().includes(" ");
    // '/swap a b' with args goes straight to the server path (same as the REPL);
    // bare '/swap' opens the picker modal.
    if (cmd === "/swap" && !hasArgs) return showSwap();
    window.__slashValue = value;  // for /create <name>
    const fn = LOCAL[cmd];
    if (fn) return fn();
    return post("/api/task", { text: value });
  }

  /* -------------------------------------------------------------- modal */

  const modal = $("modal");

  function openModal({ title, detail, fields, actions, className }) {
    modal.className = "modal" + (className ? " " + className : "");
    $("modal-title").textContent = title;
    $("modal-detail").textContent = detail || "";
    $("modal-detail").hidden = !detail;
    const f = $("modal-fields");
    f.innerHTML = "";
    (fields || []).forEach((fd) => {
      const wrap = document.createElement("div");
      wrap.className = "field";
      const l = document.createElement("label");
      l.textContent = fd.label;
      const i = document.createElement("input");
      i.value = fd.value ?? "";
      i.placeholder = fd.placeholder || "";
      i.dataset.k = fd.label;
      wrap.append(l, i);
      f.appendChild(wrap);
    });
    const b = $("modal-buttons");
    b.className = "actions";
    b.innerHTML = "";
    (actions || []).forEach((a) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn " + (a.cls || "ghost");
      btn.textContent = a.label;
      btn.addEventListener("click", () => a.onClick(collect(f), modal));
      b.appendChild(btn);
    });
    if (!modal.open) modal.showModal();
    const first = f.querySelector("input");
    if (first) first.focus();
  }

  function collect(container) {
    const out = {};
    container.querySelectorAll("[data-k]").forEach((i) => { out[i.dataset.k] = i.value.trim(); });
    return out;
  }

  function closeModal() { if (modal.open) modal.close(); }
  modal.addEventListener("cancel", (e) => { e.preventDefault(); closeModal(); });
  modal.addEventListener("click", (e) => { if (e.target === modal) closeModal(); });

  function showTrace() {
    fetch("/api/trace").then((r) => r.json()).then((d) => {
      openModal({
        title: "last run trace",
        detail: d.trace || "(no run yet — send a task first)",
        className: "trace",
        actions: [{ label: "close", cls: "ghost", onClick: closeModal }],
      });
    }).catch((e) => errorLine("trace failed: " + e));
  }

  function showSwap() {
    const swappable = state.agents.filter((a) => !a.router);
    if (!swappable.length) return line("no swappable agents");
    openModal({
      title: "hot-swap model",
      detail: "Writes the new reference into agents/<name>.yaml immediately.",
      fields: [
        { label: "agent", value: swappable[0].name },
        { label: "model", value: swappable[0].model, placeholder: "provider:model" },
      ],
      actions: [
        { label: "cancel", cls: "ghost", onClick: closeModal },
        {
          label: "swap", cls: "primary",
          onClick: async (v) => {
            const d = await post("/api/swap", { agent: v.agent, ref: v.model });
            if (d && d.ok) { closeModal(); refreshState(); }
          },
        },
      ],
    });
  }

  function showCreate() {
    openModal({
      title: "new agent",
      detail: "Same approval flow as the router's proposals — nothing is written without you.",
      fields: [
        { label: "name", value: "", placeholder: "data-analyst" },
        { label: "description", value: "", placeholder: "what it is for" },
        { label: "model", value: (state.info || []).find((r) => r[0] === "default")?.[1] || "",
          placeholder: "provider:model" },
        { label: "tools", value: "", placeholder: "read_file, write_file, list_dir" },
      ],
      actions: [
        { label: "cancel", cls: "ghost", onClick: closeModal },
        {
          label: "propose", cls: "primary",
          onClick: async (v) => {
            if (!/^[a-z0-9][a-z0-9-]*$/.test(v.name || "")) {
              $("modal-detail").hidden = false;
              $("modal-detail").textContent = "name must be kebab-case, e.g. data-analyst";
              return;
            }
            closeModal();
            await post("/api/create", {
              name: v.name,
              description: v.description,
              model: v.model,
              tools: (v.tools || "").split(",").map((t) => t.trim()).filter(Boolean),
            });
          },
        },
      ],
    });
  }

  /* ------------------------------------------------------------ wiring */

  $("composer").addEventListener("submit", (e) => { e.preventDefault(); send(); });
  $("btn-stop").addEventListener("click", () => post("/api/stop"));
  $("btn-trace").addEventListener("click", showTrace);
  $("btn-reload").addEventListener("click", () => post("/api/reload"));
  $("btn-swap").addEventListener("click", showSwap);
  $("btn-create").addEventListener("click", showCreate);
  $("btn-quit").addEventListener("click", () => post("/api/shutdown"));

  document.addEventListener("keydown", (e) => {
    const inField = /^(input|textarea|select)$/i.test(e.target.tagName);
    if (modal.open) return;
    const meta = e.ctrlKey || e.metaKey;
    if (meta && e.key === "t") { e.preventDefault(); showTrace(); }
    else if (meta && e.key === "r") { e.preventDefault(); post("/api/reload"); }
    else if (meta && e.key === "s") { e.preventDefault(); showSwap(); }
    else if (meta && e.key === "k") { e.preventDefault(); taskEl.focus(); taskEl.select(); }
    else if (!meta && !inField && e.key === "y" && state.pending) {
      const el = document.getElementById(state.pending);
      el?.querySelector(".actions .primary")?.click();
    } else if (!meta && !inField && e.key === "n" && state.pending) {
      const el = document.getElementById(state.pending);
      const btns = el?.querySelectorAll(".actions button");
      btns?.[btns.length - 1]?.click();
    } else if (e.key === "/" && document.activeElement !== taskEl) {
      e.preventDefault(); taskEl.focus();
    }
  });

  connect();
  refreshState();
  taskEl.focus();
})();
