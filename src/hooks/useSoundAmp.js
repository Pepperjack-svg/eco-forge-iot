import { useState, useCallback, useRef, useEffect } from 'react';

const SERVICE_UUID   = 'a1b2c3d4-0001-0001-0001-000000000001';
const MODE_UUID      = 'a1b2c3d4-0001-0001-0001-000000000002';
const GAIN_UUID      = 'a1b2c3d4-0001-0001-0001-000000000003';
const NOISEGATE_UUID = 'a1b2c3d4-0001-0001-0001-000000000004';
const BASSBOOST_UUID = 'a1b2c3d4-0001-0001-0001-000000000005'; // stub
const LEVEL_UUID     = 'a1b2c3d4-0001-0001-0001-000000000006';
const EQ_UUID        = 'a1b2c3d4-0001-0001-0001-000000000007'; // stub
const AMBLVL_UUID    = 'a1b2c3d4-0001-0001-0001-000000000008'; // stub
const DETECT_UUID    = 'a1b2c3d4-0001-0001-0001-000000000009'; // stub

export function useSoundAmp() {
  const [isConnected,  setIsConnected]  = useState(false);
  const [isConnecting, setIsConnecting] = useState(false);
  const [error,        setError]        = useState(null);
  const [mode,         setMode]         = useState(0);
  const [gain,         setGain]         = useState(2.0);
  const [noiseGate,    setNoiseGate]    = useState(true);
  const [level,        setLevel]        = useState(0);

  const deviceRef    = useRef(null);
  const charsRef     = useRef({});
  const levelListRef = useRef(null);

  const onDisconnected = useCallback(() => {
    setIsConnected(false);
    setLevel(0);
    charsRef.current = {};
  }, []);

  const cleanup = useCallback(async () => {
    const { level: lc } = charsRef.current;
    if (lc && levelListRef.current) {
      lc.removeEventListener('characteristicvaluechanged', levelListRef.current);
      try { await lc.stopNotifications(); } catch { /* gone */ }
    }
    levelListRef.current = null;
    charsRef.current = {};
  }, []);

  useEffect(() => {
    return () => {
      cleanup();
      if (deviceRef.current?.gatt?.connected) deviceRef.current.gatt.disconnect();
    };
  }, [cleanup]);

  const connect = useCallback(async () => {
    const isLocal = ['localhost', '127.0.0.1', '::1'].includes(location.hostname);
    if (!window.isSecureContext && !isLocal) {
      setError('HTTPS required. Use the dev server or deploy to Vercel.');
      return;
    }
    if (/ipad|iphone|ipod/i.test(navigator.userAgent)) {
      setError('Web Bluetooth is not supported on iOS.');
      return;
    }
    if (!navigator.bluetooth) {
      setError('Web Bluetooth not available. Use Chrome or Edge.');
      return;
    }

    setIsConnecting(true);
    setError(null);

    try {
      if (deviceRef.current)
        deviceRef.current.removeEventListener('gattserverdisconnected', onDisconnected);
      await cleanup();

      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [SERVICE_UUID] }],
      });
      deviceRef.current = device;
      device.addEventListener('gattserverdisconnected', onDisconnected);

      const server  = await device.gatt.connect();
      const service = await server.getPrimaryService(SERVICE_UUID);

      // Fetch all 8 characteristics (stubs still exist in firmware)
      const [modeC, gainC, ngC, , levelC] = await Promise.all([
        service.getCharacteristic(MODE_UUID),
        service.getCharacteristic(GAIN_UUID),
        service.getCharacteristic(NOISEGATE_UUID),
        service.getCharacteristic(BASSBOOST_UUID),
        service.getCharacteristic(LEVEL_UUID),
        service.getCharacteristic(EQ_UUID),
        service.getCharacteristic(AMBLVL_UUID),
        service.getCharacteristic(DETECT_UUID),
      ]);
      charsRef.current = { mode: modeC, gain: gainC, ng: ngC, level: levelC };

      const [mv, gv, nv] = await Promise.all([
        modeC.readValue(),
        gainC.readValue(),
        ngC.readValue(),
      ]);
      setMode(mv.getUint8(0));
      setGain(gv.getUint8(0) / 10);
      setNoiseGate(nv.getUint8(0) !== 0);

      const levelListener = (e) => setLevel(e.target.value.getUint8(0));
      levelListRef.current = levelListener;
      levelC.addEventListener('characteristicvaluechanged', levelListener);
      await levelC.startNotifications();

      setIsConnected(true);
      console.log('[BLE] Connected');
    } catch (err) {
      await cleanup();
      setError(err.message || 'Failed to connect');
    } finally {
      setIsConnecting(false);
    }
  }, [onDisconnected, cleanup]);

  const disconnect = useCallback(async () => {
    await cleanup();
    if (deviceRef.current?.gatt?.connected) deviceRef.current.gatt.disconnect();
  }, [cleanup]);

  const writeUint8 = useCallback(async (char, value) => {
    if (!char) return;
    try { await char.writeValueWithResponse(new Uint8Array([value])); }
    catch (err) { console.error('[BLE] Write failed:', err); }
  }, []);

  const sendMode     = useCallback((m) => { setMode(m);      writeUint8(charsRef.current.mode, m); }, [writeUint8]);
  const sendGain     = useCallback((g) => { setGain(g);      writeUint8(charsRef.current.gain, Math.round(g * 10)); }, [writeUint8]);
  const sendNoiseGate= useCallback((v) => { setNoiseGate(v); writeUint8(charsRef.current.ng, v ? 1 : 0); }, [writeUint8]);
  const clearError   = useCallback(() => setError(null), []);

  return {
    isConnected, isConnecting, error, clearError,
    connect, disconnect,
    mode, gain, noiseGate, level,
    sendMode, sendGain, sendNoiseGate,
  };
}
