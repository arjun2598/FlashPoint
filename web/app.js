// Drives the WebAssembly module and renders what it returns.
//
// The C++ side does the matching and hands back JSON; everything here is
// presentation. That split is deliberate: the engine has no idea a browser
// exists, and this file has no idea how matching works.

import createFlashPoint from "./flashpoint.js";

const $ = (id) => document.getElementById(id);

let engine = null;

// --- calling into wasm -----------------------------------------------------

function callJson(name, argTypes, args) {
  const raw = engine.ccall(name, "string", argTypes, args);
  try {
    return JSON.parse(raw);
  } catch (err) {
    throw new Error(`${name} returned malformed JSON: ${err.message}`);
  }
}

// --- formatting ------------------------------------------------------------

const groups = new Intl.NumberFormat("en-GB");
const fmt = (n) => groups.format(n);

function fmtPrice(cents) {
  // Prices are integer ticks, so this is a display choice rather than
  // arithmetic. The engine never sees a decimal (DD-009).
  return groups.format(cents);
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

// --- book ladder -----------------------------------------------------------

function renderLadder(book) {
  const wrap = el("div", "ladder");

  const head = el("div", "ladder-head");
  head.append(
    el("span"),                       // depth bar column
    el("span", null, "bid size"),
    el("span", null, "bid"),
    el("span", null, "ask"),
    el("span", null, "ask size"),
    el("span"),                       // depth bar column
  );
  wrap.append(head);

  const bids = book.bids ?? [];
  const asks = book.asks ?? [];

  if (bids.length === 0 && asks.length === 0) {
    wrap.append(el("div", "ladder-empty", "book is empty"));
    return wrap;
  }

  // One scale across both sides, so the two are visually comparable.
  const largest = Math.max(
    1,
    ...bids.map((r) => r.quantity),
    ...asks.map((r) => r.quantity),
  );

  const rows = Math.max(bids.length, asks.length);
  for (let i = 0; i < rows; i++) {
    const row = el("div", "ladder-row");
    const bid = bids[i];
    const ask = asks[i];

    // A zero-width bar would still show its border as a sliver on a row where
    // that side has no level, so an absent side gets no bar at all.
    const bidBar = el("div", bid ? "depth bid-bar" : "depth");
    if (bid) bidBar.style.width = `${(bid.quantity / largest) * 100}%`;
    row.append(bidBar);

    row.append(el("div", "qty", bid ? fmt(bid.quantity) : ""));
    row.append(el("div", "price bid", bid ? fmtPrice(bid.price) : ""));
    row.append(el("div", "price ask", ask ? fmtPrice(ask.price) : ""));
    row.append(el("div", "qty ask-qty", ask ? fmt(ask.quantity) : ""));

    const askBar = el("div", ask ? "depth ask-bar" : "depth");
    if (ask) askBar.style.width = `${(ask.quantity / largest) * 100}%`;
    row.append(askBar);

    wrap.append(row);
  }

  const foot = el("div", "ladder-foot");
  const top = book.top ?? {};
  foot.append(labelled("bid", top.hasBid ? fmtPrice(top.bidPrice) : "—"));
  foot.append(labelled("ask", top.hasAsk ? fmtPrice(top.askPrice) : "—"));
  foot.append(labelled("spread", top.spread !== undefined ? `${top.spread} ticks` : "—"));
  if (book.resting !== undefined) {
    foot.append(labelled("resting", fmt(book.resting)));
  }
  wrap.append(foot);

  return wrap;
}

function labelled(label, value) {
  const span = el("span");
  span.append(document.createTextNode(`${label} `));
  span.append(el("b", null, value));
  return span;
}

// --- events ----------------------------------------------------------------

function eventDetail(ev) {
  const price = ev.hasPrice ? fmtPrice(ev.price) : "market";
  switch (ev.type) {
    case "Accepted":
      return `#${ev.order}  ${ev.side.toLowerCase()} ${fmt(ev.quantity)} @ ${price}`;
    case "Trade":
      return `#${ev.order} took #${ev.counterparty}  ${fmt(ev.quantity)} @ ${fmtPrice(ev.price)}`;
    case "Cancelled":
      return `#${ev.order}  ${fmt(ev.quantity)} removed`;
    case "Modified":
      return `#${ev.order}  now ${fmt(ev.quantity)} @ ${price}  · priority ${ev.priority.toLowerCase()}`;
    case "Rejected":
      return `#${ev.order}  ${ev.reason}`;
    default:
      return `#${ev.order}`;
  }
}

function renderEvents(events) {
  const wrap = el("div", "events");
  for (const ev of events) {
    const row = el("div", `event ${ev.type.toLowerCase()}`);
    row.append(el("span", "seq", ev.sequence));
    row.append(el("span", "kind", ev.type.toUpperCase()));
    row.append(el("span", "detail", eventDetail(ev)));
    wrap.append(row);
  }
  return wrap;
}

// --- scenario --------------------------------------------------------------

function renderScenario(result) {
  const out = $("scenario-output");
  out.replaceChildren();

  // Consecutive events are grouped into one block so the output reads as
  // alternating narration, activity and book state.
  let pending = [];
  const flush = () => {
    if (pending.length > 0) {
      out.append(renderEvents(pending));
      pending = [];
    }
  };

  for (const step of result.steps) {
    if (step.kind === "event") {
      pending.push(step);
      continue;
    }
    flush();
    if (step.kind === "heading") {
      out.append(el("h3", "step-heading", step.text));
    } else if (step.kind === "error") {
      out.append(el("div", "error-line", step.text.trimEnd()));
    } else if (step.kind === "book") {
      out.append(renderLadder(step.book));
    }
  }
  flush();

  if (result.steps.length === 0) {
    out.append(el("p", "note", "Nothing to run. Add some commands to the script."));
  }
}

// --- generator -------------------------------------------------------------

function renderGenerate(result, elapsedMs) {
  const out = $("generate-output");
  out.replaceChildren();

  const t = result.tally;
  const last = result.chunks[result.chunks.length - 1];

  const stats = el("div", "stat-grid");
  const stat = (label, value) => {
    const box = el("div", "stat");
    box.append(el("div", "label", label));
    box.append(el("div", "value", value));
    return box;
  };
  stats.append(stat("orders", fmt(result.submitted)));
  stats.append(stat("trades", fmt(t.trades)));
  stats.append(stat("volume", fmt(t.volume)));
  stats.append(stat("ns / order", last ? last.nsPerOrder.toFixed(0) : "—"));
  stats.append(
    stat("orders / sec", last ? `${(last.ordersPerSecond / 1e6).toFixed(2)} M` : "—"),
  );
  stats.append(stat("wall clock", `${(elapsedMs / 1000).toFixed(2)} s`));
  out.append(stats);

  out.append(
    Object.assign(el("p", "note"), {
      textContent:
        "Throughput is reported per chunk rather than as one figure, because " +
        "fragmentation would show up as a trend rather than a level. It stays " +
        "flat, which is the evidence that reverting the pooled allocator was right.",
    }),
  );

  out.append(el("p", "section-title", "Throughput across the run"));
  const table = el("table", "chunks");
  const thead = el("thead");
  const hrow = el("tr");
  for (const h of ["orders", "ns/order", "orders/sec", "resting", "bid levels", "ask levels"]) {
    hrow.append(el("th", null, h));
  }
  thead.append(hrow);
  table.append(thead);

  const tbody = el("tbody");
  for (const c of result.chunks) {
    const tr = el("tr");
    tr.append(el("td", null, fmt(c.orders)));
    tr.append(el("td", null, c.nsPerOrder.toFixed(1)));
    tr.append(el("td", null, fmt(Math.round(c.ordersPerSecond))));
    tr.append(el("td", null, fmt(c.resting)));
    tr.append(el("td", null, fmt(c.bidLevels)));
    tr.append(el("td", null, fmt(c.askLevels)));
    tbody.append(tr);
  }
  table.append(tbody);
  out.append(table);

  out.append(el("p", "section-title", "Events published"));
  const counts = el("div", "stat-grid");
  counts.append(stat("accepted", fmt(t.accepted)));
  counts.append(stat("cancelled", fmt(t.cancelled)));
  counts.append(stat("modified", fmt(t.modified)));
  counts.append(stat("rejected", fmt(t.rejected)));
  counts.append(stat("sequence numbers", fmt(t.sequences)));
  out.append(counts);

  if (result.trades.length > 0) {
    out.append(el("p", "section-title", "Last executions"));
    const tape = el("div", "events");
    for (const trade of result.trades) {
      const row = el("div", "event trade");
      row.append(el("span", "seq", ""));
      row.append(el("span", "kind", "TRADE"));
      row.append(
        el(
          "span",
          "detail",
          `#${trade.taker} took #${trade.maker}  ${fmt(trade.quantity)} @ ${fmtPrice(trade.price)}  · ${trade.aggressor.toLowerCase()} aggressed`,
        ),
      );
      tape.append(row);
    }
    out.append(tape);
  }

  out.append(el("p", "section-title", "Closing book"));
  out.append(renderLadder(result.book));
}

// --- wiring ----------------------------------------------------------------

function showTab(which) {
  const scenario = which === "scenario";
  $("panel-scenario").classList.toggle("is-hidden", !scenario);
  $("panel-generate").classList.toggle("is-hidden", scenario);
  $("tab-scenario").classList.toggle("is-active", scenario);
  $("tab-generate").classList.toggle("is-active", !scenario);
  $("tab-scenario").setAttribute("aria-selected", String(scenario));
  $("tab-generate").setAttribute("aria-selected", String(!scenario));
}

async function main() {
  $("version").textContent = "loading engine…";

  engine = await createFlashPoint();

  const version = engine.ccall("fp_version", "string", [], []);
  $("version").textContent = `engine v${version} · WebAssembly`;

  const tour = engine.ccall("fp_default_scenario", "string", [], []);
  $("script").value = tour;

  $("tab-scenario").addEventListener("click", () => showTab("scenario"));
  $("tab-generate").addEventListener("click", () => showTab("generate"));

  $("reset-scenario").addEventListener("click", () => {
    $("script").value = tour;
    $("scenario-status").textContent = "reset";
  });

  $("run-scenario").addEventListener("click", () => {
    const status = $("scenario-status");
    status.textContent = "running…";
    try {
      const started = performance.now();
      const result = callJson("fp_run_script", ["string"], [$("script").value]);
      renderScenario(result);
      const ms = performance.now() - started;
      status.textContent = result.hadError
        ? `finished in ${ms.toFixed(1)} ms — some lines were rejected`
        : `finished in ${ms.toFixed(1)} ms`;
    } catch (err) {
      status.textContent = err.message;
    }
  });

  $("run-generate").addEventListener("click", () => {
    const button = $("run-generate");
    const status = $("generate-status");
    const orders = Number($("gen-orders").value) || 1000;
    const seed = Number($("gen-seed").value) || 0;
    const protection = Number($("gen-protection").value) || 0;

    button.disabled = true;
    status.textContent = `running ${fmt(orders)} orders…`;

    // Yield once so the button state and status actually paint before the
    // engine blocks the main thread. A worker would be the real answer for a
    // multi-million-order run; this keeps the page honest about being busy.
    setTimeout(() => {
      try {
        const started = performance.now();
        const result = callJson(
          "fp_generate",
          ["number", "number", "number", "number"],
          [orders, seed, protection, 12],
        );
        const elapsed = performance.now() - started;
        renderGenerate(result, elapsed);
        status.textContent = `done in ${(elapsed / 1000).toFixed(2)} s`;
      } catch (err) {
        status.textContent = err.message;
      } finally {
        button.disabled = false;
      }
    }, 0);
  });

  // Open on the tour already run, so the page shows something immediately.
  $("run-scenario").click();
}

main().catch((err) => {
  $("version").textContent = `failed to load: ${err.message}`;
});
