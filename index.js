/*
 * IoT Backend — MQTT ↔ Supabase bridge
 * ─────────────────────────────────────────────────────────────
 * What this does:
 *   - Subscribes to  iot/+/status  on HiveMQ
 *   - Writes every status message to:
 *       device_state  (upserted — always the latest snapshot)
 *       device_logs   (appended — full history)
 *   - Watches Supabase Realtime for new rows in device_commands
 *   - Forwards each command as an MQTT publish to the right device
 *
 * Deploy free on Railway.app:
 *   1. Push this folder to a GitHub repo
 *   2. New project → Deploy from GitHub
 *   3. Add the env vars below in Railway's Variables tab
 *   4. Railway auto-runs "npm start"
 * ─────────────────────────────────────────────────────────────
 */

require('dotenv').config();
const mqtt = require('mqtt');
const { createClient } = require('@supabase/supabase-js');

// ── Validate env ────────────────────────────────────────────────
const required = ['SUPABASE_URL', 'SUPABASE_SERVICE_KEY', 'MQTT_HOST', 'MQTT_USER', 'MQTT_PASS'];
for (const key of required) {
  if (!process.env[key]) {
    console.error(`[Boot] Missing required env var: ${key}`);
    process.exit(1);
  }
}

// ── Supabase client (service role key — full DB access) ─────────
const supabase = createClient(
  process.env.SUPABASE_URL,
  process.env.SUPABASE_SERVICE_KEY,
  { auth: { persistSession: false } }
);

// ── MQTT client ─────────────────────────────────────────────────
const mqttClient = mqtt.connect(
  `mqtts://${process.env.MQTT_HOST}:${process.env.MQTT_PORT || 8883}`,
  {
    username: process.env.MQTT_USER,
    password: process.env.MQTT_PASS,
    rejectUnauthorized: false,   // testing; set true in production with proper cert
    reconnectPeriod: 5000,
    connectTimeout: 10000,
    clientId: `iot-backend-${Math.random().toString(16).slice(2, 8)}`,
  }
);

// ─────────────────────────────────────────────────────────────────
// MQTT: connection events
// ─────────────────────────────────────────────────────────────────
mqttClient.on('connect', () => {
  console.log(`[MQTT] Connected to ${process.env.MQTT_HOST}`);

  // Subscribe to ALL device status topics with wildcard
  mqttClient.subscribe('iot/+/status', { qos: 1 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error:', err.message);
    else     console.log('[MQTT] Subscribed to iot/+/status');
  });
});

mqttClient.on('reconnect', () => console.log('[MQTT] Reconnecting...'));
mqttClient.on('error',     (err) => console.error('[MQTT] Error:', err.message));

// ─────────────────────────────────────────────────────────────────
// MQTT → Supabase: save incoming device status
// Topic format:  iot/IoT-XXXXXX/status
// Payload:       {"relay":"on","duty":128,"uptime":3600,"ip":"192.168.1.42"}
// ─────────────────────────────────────────────────────────────────
mqttClient.on('message', async (topic, message) => {
  const parts    = topic.split('/');
  const deviceId = parts[1];   // e.g. "IoT-BE0911"

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.error(`[MQTT] Non-JSON payload from ${deviceId}:`, message.toString());
    return;
  }

  console.log(`[MQTT] Status from ${deviceId}:`, payload);

  // Upsert latest state row (one row per device, always up to date)
  const { error: stateErr } = await supabase
    .from('device_state')
    .upsert({
      device_id:  deviceId,
      relay:      payload.relay      ?? null,
      duty:       payload.duty       ?? null,
      uptime:     payload.uptime     ?? null,
      ip:         payload.ip         ?? null,
      updated_at: new Date().toISOString(),
    },
    { onConflict: 'device_id' });

  if (stateErr) console.error('[DB] State upsert error:', stateErr.message);

  // Append to history log
  const { error: logErr } = await supabase
    .from('device_logs')
    .insert({
      device_id: deviceId,
      relay:     payload.relay  ?? null,
      duty:      payload.duty   ?? null,
      uptime:    payload.uptime ?? null,
      ip:        payload.ip     ?? null,
    });

  if (logErr) console.error('[DB] Log insert error:', logErr.message);
});

// ─────────────────────────────────────────────────────────────────
// Command dispatch — shared by Realtime AND polling fallback
//
// BUG-1 FIX: Supabase Realtime for postgres_changes only fires when
// the table is added to the supabase_realtime publication:
//   ALTER PUBLICATION supabase_realtime ADD TABLE device_commands;
// Many projects skip this step, so Realtime never fires and commands
// are silently lost.  The polling fallback below catches every command
// within ~5 s regardless of publication state.
// ─────────────────────────────────────────────────────────────────

// Track IDs we've already forwarded so both code paths don't double-publish
const forwardedIds = new Set();

function publishCommand(row) {
  if (!row || !row.id || !row.device_id || !row.payload) {
    console.warn('[CMD] Skipping malformed command row:', row);
    return;
  }
  if (forwardedIds.has(row.id)) return;   // already sent via Realtime or poll
  forwardedIds.add(row.id);

  // Prune old IDs to avoid unbounded memory growth (keep last 500)
  if (forwardedIds.size > 500) {
    const first = forwardedIds.values().next().value;
    forwardedIds.delete(first);
  }

  const topic   = `iot/${row.device_id}/commands`;
  const message = JSON.stringify(row.payload);

  if (!mqttClient.connected) {
    console.warn('[CMD] MQTT not connected — command dropped for', row.device_id);
    return;
  }
  mqttClient.publish(topic, message, { qos: 1 });
  console.log(`[MQTT] Command → ${topic}:`, message);
}

// ── Polling fallback: catch commands every 5 s ──────────────────
// Uses a sliding window of the last 10 s to handle clock skew.
// Once Realtime is properly configured this is a cheap no-op (all
// IDs are already in forwardedIds, so nothing is re-published).
let lastPollTime = new Date(Date.now() - 10000).toISOString();

async function pollDeviceCommands() {
  try {
    const { data: rows, error } = await supabase
      .from('device_commands')
      .select('*')
      .gte('created_at', lastPollTime)
      .order('created_at', { ascending: true });

    if (error) { console.error('[Poll] Query error:', error.message); return; }
    if (!rows || rows.length === 0) return;

    for (const row of rows) publishCommand(row);

    // Advance window to just before the newest row we saw
    // (keep 1 s overlap to handle sub-second clock skew)
    const newest = rows[rows.length - 1].created_at;
    lastPollTime = new Date(new Date(newest).getTime() - 1000).toISOString();
  } catch (err) {
    console.error('[Poll] Unexpected error:', err.message);
  }
}

setInterval(pollDeviceCommands, 5000);

// ─────────────────────────────────────────────────────────────────
// Supabase → MQTT: forward new commands to the right device
// Primary path: Supabase Realtime (instant, requires publication setup)
// Fallback:     polling above    (≤5 s delay, always works)
// ─────────────────────────────────────────────────────────────────
supabase
  .channel('device-commands-channel')
  .on(
    'postgres_changes',
    { event: 'INSERT', schema: 'public', table: 'device_commands' },
    ({ new: row }) => {
      console.log('[Realtime] New command row:', row.id);
      publishCommand(row);
    }
  )
  .subscribe((status, err) => {
    if (err) console.error('[Realtime] Subscription error:', err.message);
    else     console.log('[Realtime] device_commands channel:', status);
  });

// ─────────────────────────────────────────────────────────────────
// Graceful shutdown
// ─────────────────────────────────────────────────────────────────
process.on('SIGINT',  () => { console.log('\n[Boot] Shutting down...'); mqttClient.end(); process.exit(0); });
process.on('SIGTERM', () => { mqttClient.end(); process.exit(0); });

console.log('[Boot] IoT backend started');
