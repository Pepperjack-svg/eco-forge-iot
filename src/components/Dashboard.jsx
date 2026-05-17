import React from 'react';
import { Bluetooth, BluetoothOff, Volume2, VolumeX, Mic } from 'lucide-react';
import { useSoundAmp } from '../hooks/useSoundAmp';
import styles from './Dashboard.module.css';

const MODES = [
  { id: 0, label: 'Off',     icon: <VolumeX size={20} /> },
  { id: 1, label: 'Ambient', icon: <Volume2 size={20} /> },
  { id: 2, label: 'ANC',     icon: <Mic     size={20} /> },
];

export default function Dashboard() {
  const {
    isConnected, isConnecting, error, clearError,
    connect, disconnect,
    mode, gain, noiseGate, level,
    sendMode, sendGain, sendNoiseGate,
  } = useSoundAmp();

  const vuPercent = Math.min(100, (level / 255) * 100);

  return (
    <div className={styles.page}>
      {/* Header */}
      <header className={styles.header}>
        <div>
          <h1 className={styles.title}>SoundAmp IoT</h1>
          <p className={styles.subtitle}>ESP32 · INMP441 · BLE</p>
        </div>
        <div className={styles.headerActions}>
          <div className={`${styles.badge} ${isConnected ? styles.badgeOn : ''}`}>
            <span className={`${styles.dot} ${isConnected ? styles.dotOn : ''}`} />
            {isConnected ? 'Connected' : 'Offline'}
          </div>
          {!isConnected ? (
            <button className={styles.btn} onClick={connect} disabled={isConnecting}>
              <Bluetooth size={16} />
              {isConnecting ? 'Scanning…' : 'Connect'}
            </button>
          ) : (
            <button className={`${styles.btn} ${styles.btnSecondary}`} onClick={disconnect}>
              <BluetoothOff size={16} />
              Disconnect
            </button>
          )}
        </div>
      </header>

      {error && (
        <div className={styles.errorBanner}>
          <span>{error}</span>
          <button onClick={clearError} className={styles.errorClose}>✕</button>
        </div>
      )}

      <main className={styles.main}>
        {/* VU Meter */}
        <div className={styles.card}>
          <div className={styles.cardLabel}>Live Level</div>
          <div className={styles.vuTrack}>
            <div className={styles.vuBar} style={{ width: `${vuPercent}%` }} />
          </div>
          <div className={styles.vuValue}>{level}</div>
        </div>

        {/* Mode */}
        <div className={styles.card}>
          <div className={styles.cardLabel}>Audio Mode</div>
          <div className={styles.modeGrid}>
            {MODES.map(m => (
              <button
                key={m.id}
                onClick={() => sendMode(m.id)}
                disabled={!isConnected}
                className={`${styles.modeBtn} ${mode === m.id ? styles.modeBtnActive : ''} ${styles['modeBtn' + m.id]}`}
              >
                {m.icon}
                <span>{m.label}</span>
              </button>
            ))}
          </div>
        </div>

        {/* Gain */}
        <div className={styles.card}>
          <div className={styles.cardLabel}>Gain</div>
          <div className={styles.sliderRow}>
            <input
              type="range"
              min={1} max={10} step={0.5}
              value={gain}
              disabled={!isConnected}
              onChange={e => sendGain(parseFloat(e.target.value))}
              className={styles.slider}
            />
            <span className={styles.sliderVal}>{gain.toFixed(1)}×</span>
          </div>
          {mode === 2 && isConnected && (
            <p className={styles.hint}>
              ANC strength — start low, increase until noise drops.
            </p>
          )}
        </div>

        {/* Noise Gate */}
        <div className={styles.card}>
          <div className={styles.toggleRow} style={{ padding: 0, border: 'none' }}>
            <span className={styles.toggleLabel}>Noise Gate</span>
            <button
              role="switch"
              aria-checked={noiseGate}
              disabled={!isConnected}
              onClick={() => sendNoiseGate(!noiseGate)}
              className={`${styles.toggle} ${noiseGate ? styles.toggleOn : ''}`}
            >
              <span className={styles.toggleThumb} />
            </button>
          </div>
        </div>
      </main>
    </div>
  );
}
