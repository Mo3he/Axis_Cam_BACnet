/*
 * Sample data for the public preview: an AXIS D6310 Air Quality Sensor running
 * the app with the object list from examples/axis-d6310-air-quality.json.
 * Answers the page's api/... and /vapix/services requests in memory; nothing
 * is sent to a camera and changes are lost on reload.
 * The other files in this folder are copies of app/html (refresh them by
 * re-copying and re-adding the mock.js script tag in <head>).
 */
(() => {
  "use strict";

  const IDENTITY = { model: "AXIS D6310", serial: "B8A44F7E2A15", version: "0.1.2", address: "192.168.0.90" };
  const MAX_INSTANCE = 4194302;

  const params = {
    Enabled: "yes",
    DeviceInstance: "",
    DeviceName: "",
    UdpPort: "47808",
    Location: "Building A, floor 2, room 2.14",
    Description: "Classroom air quality",
  };

  /* ------------------------------------------------------------ events */

  const METADATA = ["tnsaxis:AirQualityMonitor", "tnsaxis:Metadata"];
  const AQ_SOURCES = ["AQI", "CO2", "Humidity", "NOx", "PM1.0", "PM2.5", "PM4.0", "PM10.0", "Temperature", "VOC"];

  /* What the sensor declares (GetEventInstances). Each event: topic levels as
     [namespace, name, NiceName], sources and data fields. */
  const CATALOGUE = [
    {
      topic: [["tns1", "Device", ""], ["tnsaxis", "Status", "Status"], ["", "SystemReady", "System ready"]],
      sources: [],
      data: [{ name: "ready", type: "boolean", state: true }],
    },
    {
      topic: [["tns1", "Device", ""], ["tnsaxis", "IO", "I/O"], ["", "VirtualInput", "Virtual input"]],
      sources: [{ name: "port", type: "int", values: Array.from({ length: 32 }, (_, i) => String(i + 1)) }],
      data: [{ name: "active", type: "boolean", state: true }],
    },
    {
      topic: [["tns1", "Device", ""], ["tnsaxis", "IO", "I/O"], ["", "VirtualPort", "Manual trigger"]],
      sources: [{ name: "port", type: "int", values: ["1"] }],
      data: [{ name: "state", type: "boolean", state: true }],
    },
    {
      topic: [["tns1", "Device", ""], ["tnsaxis", "Network", "Network"], ["", "Lost", "Network lost"]],
      sources: [{ name: "interface", type: "string", values: ["All"] }],
      data: [{ name: "lost", type: "boolean", state: true }],
    },
    {
      topic: [["tnsaxis", "AirQualityMonitor", "Air quality"], ["", "Metadata", "Metadata"]],
      sources: [],
      data: [
        ["Temperature", "double"], ["Humidity", "double"], ["CO2", "int"], ["PM1.0", "double"],
        ["PM2.5", "double"], ["PM4.0", "double"], ["PM10.0", "double"], ["VOC", "int"], ["NOx", "int"],
        ["AQI", "int"], ["HeatIndex", "double"], ["Humidex", "int"],
      ].map(([name, type]) => ({ name, type, state: false })),
    },
    {
      topic: [["tnsaxis", "AirQualityMonitor", "Air quality"], ["", "Alarm", "Alarm"]],
      sources: [{ name: "source", type: "string", values: AQ_SOURCES }],
      data: [{ name: "active", type: "boolean", state: true }],
    },
    {
      topic: [["tnsaxis", "AirQualityMonitor", "Air quality"], ["", "Vaping", "Vaping or smoking"]],
      sources: [],
      data: [{ name: "active", type: "boolean", state: true }],
    },
    {
      topic: [["tnsaxis", "AirQualityMonitor", "Air quality"], ["", "Masking", "Sensor masked"]],
      sources: [],
      data: [{ name: "active", type: "boolean", state: true }],
    },
  ];

  /* The topic as the page and the API write it: deeper levels without a
     namespace inherit their parent's. */
  function topicKey(levels) {
    let ns = "";
    return levels.map(([levelNs, name]) => `${(ns = levelNs || ns)}:${name}`).join("/");
  }
  const EVENTS = new Map(CATALOGUE.map((event) => [topicKey(event.topic), event]));

  /* Slowly drifting readings: smooth functions of the time, so values change
     a little between the page's 5 s polls. */
  function readings(t) {
    const s = (period, phase) => Math.sin(t / period + phase);
    const r1 = (v) => Math.round(v * 10) / 10;
    const temperature = r1(22.2 + 0.6 * s(900, 0) + 0.15 * s(97, 1));
    const humidity = r1(41 + 3 * s(1300, 1) + 0.5 * s(111, 2));
    const pm1 = 3.2 + 0.8 * s(600, 3) + 0.3 * s(47, 4);
    const pm25 = r1(pm1 * 1.45);
    const e = ((6.112 * 10 ** ((7.5 * temperature) / (237.7 + temperature))) * humidity) / 100;
    return {
      Temperature: temperature,
      Humidity: humidity,
      CO2: Math.round(640 + 120 * s(1500, 2) + 15 * s(73, 5)),
      "PM1.0": r1(pm1),
      "PM2.5": pm25,
      "PM4.0": r1(pm1 * 1.7),
      "PM10.0": r1(pm1 * 1.9),
      VOC: Math.round(100 + 25 * s(1100, 0.5) + 4 * s(61, 6)),
      NOx: 1,
      AQI: Math.round(pm25 * 4.17),
      HeatIndex: r1(temperature - 0.4 + 0.02 * (humidity - 40)),
      Humidex: Math.round(temperature + 0.5555 * (e - 10)),
    };
  }

  /* Current state of the stateful events. */
  function stateValue(key, data) {
    if (key.endsWith("/tnsaxis:SystemReady")) return data === "ready" ? true : undefined;
    return data === "active" || data === "state" || data === "lost" ? false : undefined;
  }

  /* The last event an object would have received, or null. */
  function lastEvent(binding, since, now) {
    const key = binding.topic.join("/");
    const event = EVENTS.get(key);
    if (!event) return null;
    const source = event.sources.find((s) => s.name === binding.source_key);
    if (binding.source_key && (!source || !source.values.includes(binding.source_value))) return null;
    const field = event.data.find((d) => d.name === binding.data_key);
    if (!field) return null;
    if (key === METADATA.join("/")) {
      const time = Math.floor(now);
      return { value: readings(time)[field.name], time, count: Math.max(1, Math.floor(now - since)) };
    }
    const value = stateValue(key, field.name);
    return value === undefined ? null : { value, time: Math.floor(since), count: 1 };
  }

  /* ------------------------------------------------- values as the app */

  /* printf("%g"); analog present values are REAL (float). */
  const g = (n) => String(Number(n.toPrecision(6)));
  const fmt = (n) => g(Math.fround(n));
  const raw = (v) => (typeof v === "boolean" ? (v ? "1" : "0") : typeof v === "number" ? g(v) : String(v));

  function convert(object, v) {
    if (object.type === "analog-value") {
      if (typeof v === "boolean") return v ? 1 : 0;
      const n = typeof v === "number" ? v : Number(String(v).trim());
      return String(v).trim() !== "" && Number.isFinite(n) ? n : undefined;
    }
    if (object.type === "binary-value") {
      let on;
      if (typeof v === "boolean") on = v;
      else if (typeof v === "number") on = v !== 0;
      else if (/^(true|on|active|yes|high|1)$/i.test(v)) on = true;
      else if (/^(false|off|inactive|no|low|0)$/i.test(v)) on = false;
      else return undefined;
      return object.invert ? !on : on;
    }
    if (typeof v === "string") {
      const index = object.states.findIndex((s) => s.toLowerCase() === v.toLowerCase());
      if (index >= 0) return index + 1;
    }
    const n = Number(v);
    return Number.isInteger(n) && n >= 1 && n <= object.states.length ? n : undefined;
  }

  function valueText(object, v) {
    if (object.type === "analog-value") return fmt(v ?? 0);
    if (object.type === "binary-value") return v ? "active" : "inactive";
    const state = v ?? 1;
    return state >= 1 && state <= object.states.length ? `${state} (${object.states[state - 1]})` : String(state);
  }

  /* ----------------------------------------------------------- the app */

  let objects = [
    ...[
      ["Temperature", 62, 0.2, "Temperature"], ["Humidity", 29, 1, "Humidity"], ["CO2", 96, 10, "CO2"],
      ["PM1.0", 219, 1, "PM1.0"], ["PM2.5", 219, 1, "PM2.5"], ["PM4.0", 219, 1, "PM4.0"],
      ["PM10", 219, 1, "PM10.0"], ["VOC index", 95, 5, "VOC"], ["NOx index", 95, 5, "NOx"],
      ["AQI", 95, 1, "AQI"], ["Heat index", 62, 1, "HeatIndex"], ["Humidex", 95, 1, "Humidex"],
    ].map(([name, units, cov, key], i) => ({
      type: "analog-value",
      instance: i + 1,
      name,
      description: `AXIS D6310 ${name}`,
      units,
      cov_increment: cov,
      event: { topic: METADATA, source_key: "", source_value: "", data_key: key },
    })),
    ...[
      ["Vaping or smoking", "tnsaxis:Vaping", "", ""], ["Sensor masked", "tnsaxis:Masking", "", ""],
      ["CO2 alarm", "tnsaxis:Alarm", "source", "CO2"], ["PM2.5 alarm", "tnsaxis:Alarm", "source", "PM2.5"],
    ].map(([name, topic, sourceKey, sourceValue], i) => ({
      type: "binary-value",
      instance: i + 1,
      name,
      description: "",
      invert: false,
      active_text: "Active",
      inactive_text: "Normal",
      event: {
        topic: ["tnsaxis:AirQualityMonitor", topic],
        source_key: sourceKey,
        source_value: sourceValue,
        data_key: "active",
      },
    })),
  ];

  /* When the device (and its event subscriptions) last started. */
  let startedAt = Date.now() / 1000 - 3 * 3600 - 17 * 60;

  function effective() {
    const tail = IDENTITY.serial.slice(-6);
    const auto = Math.min(parseInt(tail, 16) & 0x3fffff, MAX_INSTANCE);
    const instance = /^\d+$/.test(params.DeviceInstance) && +params.DeviceInstance <= MAX_INSTANCE
      ? +params.DeviceInstance
      : auto;
    const port = /^\d+$/.test(params.UdpPort) && +params.UdpPort >= 1024 && +params.UdpPort <= 65535
      ? +params.UdpPort
      : 47808;
    const name = params.DeviceName || `${IDENTITY.model} ${IDENTITY.serial}`;
    return { instance, name, port };
  }

  function settings() {
    const e = effective();
    return { ...params, effective: { DeviceInstance: e.instance, DeviceName: e.name, UdpPort: e.port } };
  }

  function status() {
    const running = params.Enabled === "yes";
    const now = Date.now() / 1000;
    const e = effective();
    return {
      enabled: running,
      running,
      error: "",
      device: {
        instance: e.instance,
        name: e.name,
        address: running ? IDENTITY.address : "",
        port: e.port,
        model: IDENTITY.model,
        version: IDENTITY.version,
      },
      objects: !running ? [] : objects.map((object) => {
        const last = lastEvent(object.event, startedAt, now);
        const value = last ? convert(object, last.value) : undefined;
        return {
          type: object.type,
          instance: object.instance,
          name: object.name,
          present_value: valueText(object, value),
          topic: object.event.topic.join("/"),
          subscribed: true,
          has_value: value !== undefined,
          events: last ? last.count : 0,
          last_event_value: last ? raw(last.value) : "",
          last_event_time: last ? last.time : 0,
          error: last && value === undefined
            ? `value '${raw(last.value)}' of ${object.event.data_key} cannot be converted`
            : "",
        };
      }),
    };
  }

  function restart() {
    startedAt = Date.now() / 1000;
  }

  /* The checks the app makes, with its error messages. */
  function validateObjects(body) {
    const fail = (index, field, reason) => {
      throw new Error(index >= 0 ? `objects[${index}].${field}: ${reason}` : `${field}: ${reason}`);
    };
    const identifier = /^[A-Za-z0-9_.\-]+$/;
    if (!body || !Array.isArray(body.objects)) fail(-1, "objects", "must be an array");
    if (body.objects.length > 64) fail(-1, "objects", "has more than 64 entries");
    body.objects.forEach((o, i) => {
      if (!o || typeof o !== "object") fail(i, "", "must be an object");
      if (!TYPES.includes(o.type)) fail(i, "type", "must be analog-value, binary-value or multistate-value");
      if (!Number.isInteger(o.instance) || o.instance < 0 || o.instance > MAX_INSTANCE)
        fail(i, "instance", "must be a whole number from 0 to 4194302");
      if (typeof o.name !== "string" || !o.name) fail(i, "name", "is required");
      if (o.name.length > 64) fail(i, "name", "is longer than 64 characters");
      if (o.type === "multistate-value") {
        if (!Array.isArray(o.states) || o.states.length < 2 || o.states.length > 16)
          fail(i, "states", "must be a list of 2 to 16 state names");
        o.states.forEach((s, n) => {
          if (typeof s !== "string" || !s) fail(i, `states[${n}]`, "must be a non-empty string");
          if (s.length > 32) fail(i, `states[${n}]`, "is longer than 32 characters");
          if (o.states.slice(0, n).some((p) => p.toLowerCase() === s.toLowerCase()))
            fail(i, `states[${n}]`, "duplicates another state");
        });
      }
      const event = o.event;
      if (!event || typeof event !== "object") fail(i, "event", "is required");
      if (!Array.isArray(event.topic) || !event.topic.length) fail(i, "event.topic", "must be a non-empty array");
      if (event.topic.length > 6) fail(i, "event.topic", "has more than 6 levels");
      event.topic.forEach((level, n) => {
        const match = /^(tns1|tnsaxis):([A-Za-z0-9_.\-]+)$/.exec(level);
        if (!match) fail(i, `event.topic[${n}]`, "must be namespace:name, e.g. tns1:Device");
      });
      if (!event.data_key) fail(i, "event.data_key", "is required");
      if (!identifier.test(event.data_key)) fail(i, "event.data_key", "has invalid characters");
      if (event.source_key && !identifier.test(event.source_key))
        fail(i, "event.source_key", "has invalid characters");
      if (event.source_value && !event.source_key)
        fail(i, "event.source_key", "is required when source_value is set");
      body.objects.slice(0, i).forEach((p) => {
        if (p.type === o.type && p.instance === o.instance)
          fail(i, "instance", "is already used by another object of this type");
        if (p.name === o.name) fail(i, "name", "is already used by another object");
      });
    });
  }
  const TYPES = ["analog-value", "binary-value", "multistate-value"];

  function normalize(o) {
    const out = { type: o.type, instance: o.instance, name: o.name, description: o.description || "" };
    if (o.type === "analog-value") {
      out.units = o.units ?? 95;
      out.cov_increment = o.cov_increment ?? 0;
    } else if (o.type === "binary-value") {
      out.invert = Boolean(o.invert);
      out.active_text = o.active_text || "";
      out.inactive_text = o.inactive_text || "";
    } else {
      out.states = o.states.slice();
    }
    out.event = {
      topic: o.event.topic.slice(),
      source_key: o.event.source_key || "",
      source_value: o.event.source_value || "",
      data_key: o.event.data_key,
    };
    return out;
  }

  function applySettings(body) {
    const values = new URLSearchParams(body);
    for (const [name, value] of values) {
      if (!(name in params)) throw new Error(`unknown setting ${name}`);
      if (name === "Enabled" && value !== "yes" && value !== "no") throw new Error("Enabled must be yes or no");
      if (name === "DeviceInstance" && value && !(/^\d+$/.test(value) && +value <= MAX_INSTANCE))
        throw new Error("DeviceInstance must be empty (automatic) or 0 to 4194302");
      if (name === "UdpPort" && !(/^\d+$/.test(value) && +value >= 1024 && +value <= 65535))
        throw new Error("UdpPort must be 1024 to 65535");
      if (value.length > 64) throw new Error(`${name} is longer than 64 characters`);
    }
    let changed = false;
    for (const [name, value] of values) {
      if (params[name] !== value) changed = true;
      params[name] = value;
    }
    if (changed) restart();
  }

  /* -------------------------------------------------- GetEventInstances */

  function eventInstancesXml() {
    const NS = { tns1: "http://www.onvif.org/ver10/topics", tnsaxis: "http://www.axis.com/2009/event/topics" };
    const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
    const items = (list, state) =>
      list.map((item) => {
        const values = (item.values || []).map((v) => `<aev:Value>${esc(v)}</aev:Value>`).join("");
        const property = state && item.state ? ' isPropertyState="true"' : "";
        return `<aev:SimpleItemInstance Name="${esc(item.name)}" Type="xsd:${item.type}"${property}>${values}</aev:SimpleItemInstance>`;
      }).join("");

    /* Merge the topics into one tree. */
    const root = { children: new Map() };
    for (const event of CATALOGUE) {
      let node = root;
      event.topic.forEach(([ns, name, nice], i) => {
        const key = `${ns}:${name}`;
        if (!node.children.has(key)) node.children.set(key, { ns, name, nice, children: new Map() });
        node = node.children.get(key);
        if (i === event.topic.length - 1) node.event = event;
      });
    }
    const render = (node) => {
      const tag = node.ns ? `${node.ns}:${node.name}` : node.name;
      let attrs = node.nice ? ` aev:NiceName="${esc(node.nice)}"` : "";
      let body = "";
      if (node.event) {
        const stateful = node.event.data.some((d) => d.state);
        attrs += ' wstop:topic="true"';
        body += `<aev:MessageInstance aev:isProperty="${stateful}">` +
          (node.event.sources.length ? `<aev:SourceInstance>${items(node.event.sources)}</aev:SourceInstance>` : "") +
          `<aev:DataInstance>${items(node.event.data, true)}</aev:DataInstance></aev:MessageInstance>`;
      }
      for (const child of node.children.values()) body += render(child);
      return `<${tag}${attrs}>${body}</${tag}>`;
    };
    return '<?xml version="1.0" encoding="UTF-8"?>' +
      '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope"' +
      ' xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:wstop="http://docs.oasis-open.org/wsn/t-1"' +
      ` xmlns:tns1="${NS.tns1}" xmlns:tnsaxis="${NS.tnsaxis}" xmlns:aev="http://www.axis.com/vapix/ws/event1">` +
      "<SOAP-ENV:Body><aev:GetEventInstancesResponse><wstop:TopicSet>" +
      [...root.children.values()].map(render).join("") +
      "</wstop:TopicSet></aev:GetEventInstancesResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>";
  }

  /* ------------------------------------------------------------- fetch */

  const realFetch = window.fetch.bind(window);
  const apiBase = new URL("api/", document.baseURI).pathname;
  const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  function json(body, statusCode = 200) {
    return new Response(JSON.stringify(body), {
      status: statusCode,
      headers: { "Content-Type": "application/json" },
    });
  }

  async function handle(url, method, body) {
    await delay(60 + Math.random() * 90);
    if (url.pathname === "/vapix/services") {
      if (method !== "POST" || !/GetEventInstances/.test(body)) return new Response("", { status: 400 });
      return new Response(eventInstancesXml(), { headers: { "Content-Type": "application/soap+xml; charset=utf-8" } });
    }
    const route = url.pathname.slice(apiBase.length);
    try {
      if (method === "GET" && route === "settings") return json(settings());
      if (method === "POST" && route === "settings") {
        applySettings(body);
        return json({ status: "ok" });
      }
      if (method === "GET" && route === "objects") return json({ objects: objects.map(normalize) });
      if (method === "POST" && route === "objects") {
        let parsed;
        try {
          parsed = JSON.parse(body);
        } catch {
          throw new Error("body is not valid JSON");
        }
        validateObjects(parsed);
        objects = parsed.objects.map(normalize);
        restart();
        return json({ status: "ok" });
      }
      if (method === "GET" && route === "status") return json(status());
      return json({ error: "not found" }, 404);
    } catch (error) {
      return json({ error: error.message }, 400);
    }
  }

  window.fetch = async (input, init = {}) => {
    const request = input instanceof Request ? input : null;
    const url = new URL(request ? request.url : String(input), document.baseURI);
    const mocked = url.pathname === "/vapix/services" || url.pathname.startsWith(apiBase);
    if (!mocked) return realFetch(input, init);
    const method = (init.method || (request && request.method) || "GET").toUpperCase();
    let body = init.body ?? (request ? await request.text() : "");
    if (body instanceof URLSearchParams) body = body.toString();
    return handle(url, method, typeof body === "string" ? body : "");
  };
})();
