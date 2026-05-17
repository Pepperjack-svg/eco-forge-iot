/*
 * SoundAmp IoT — Noise Blocker  (v3)
 *
 * Hardware
 *   ESP32 DevKit
 *   INMP441  →  SCK=GPIO32  WS=GPIO33  SD=GPIO34  L/R=GND
 *   TRRS     →  TIP=GPIO25  RING1=GPIO26  RING2=GND
 *
 * v3 fixes & improvements
 *   BUG FIX  i2s_pin_config_t now uses designated field names.
 *             Newer ESP-IDF prepended mck_io_num to the struct;
 *             positional init mapped SCK_PIN(32) as MCLK (only
 *             valid on GPIO 0/1/3), causing the fatal "mclk config
 *             failed" error and leaving the mic completely silent.
 *
 *   ANC LP   Lowered cascade cutoff 3.5 kHz → 2 kHz.
 *             At 1 ms DMA latency the phase-inversion comb filter
 *             has a destructive peak at ~500 Hz (anti-noise arrives
 *             in-phase with noise there).  Cutting at 2 kHz reduces
 *             energy in that damaging 400–800 Hz window while still
 *             covering the voice-fundamental band (80–400 Hz) and
 *             the 1 kHz comb cancellation peak.
 *
 *   Shelf    Removed.  A +4 dB shelf at 250 Hz over-drove the ANC
 *             in the 150–300 Hz range where G > 2·cos(2πfτ),
 *             turning partial cancellation into partial addition.
 *
 *   Gain     Ceiling raised 3× → 5× now that the LP is tighter.
 *
 * Modes
 *   OFF     — silent, earphone = passive ear plug
 *   ANC     — continuous phase-inverted output through 2 kHz cascade LP.
 *             Gain slider = cancellation strength (start 2–3×, max 5×).
 *   AMBIENT — simple pass-through amplifier.
 *
 * Partition: Minimal SPIFFS
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <math.h>

// ── BLE UUIDs ────────────────────────────────────────────────
#define SERVICE_UUID   "a1b2c3d4-0001-0001-0001-000000000001"
#define MODE_UUID      "a1b2c3d4-0001-0001-0001-000000000002"
#define GAIN_UUID      "a1b2c3d4-0001-0001-0001-000000000003"
#define NOISEGATE_UUID "a1b2c3d4-0001-0001-0001-000000000004"
#define BASSBOOST_UUID "a1b2c3d4-0001-0001-0001-000000000005"
#define LEVEL_UUID     "a1b2c3d4-0001-0001-0001-000000000006"
#define EQ_UUID        "a1b2c3d4-0001-0001-0001-000000000007"
#define AMBLVL_UUID    "a1b2c3d4-0001-0001-0001-000000000008"
#define DETECT_UUID    "a1b2c3d4-0001-0001-0001-000000000009"

// ── Pins ─────────────────────────────────────────────────────
#define I2S_PORT    I2S_NUM_1
#define SCK_PIN     32
#define WS_PIN      33
#define SD_PIN      34
#define DAC_L       DAC_CHANNEL_1   // GPIO25 → TIP
#define DAC_R       DAC_CHANNEL_2   // GPIO26 → RING1

// ── Audio ────────────────────────────────────────────────────
// 32 kHz: one DMA block = 1 ms latency (was 2 ms at 16 kHz).
// The ANC comb first destructive peak is at 1/(2·1ms) = 500 Hz;
// the cascade LP at 2 kHz is chosen to limit damage there while
// preserving the 1 kHz comb cancellation peak.
#define FS          32000
#define DMA_LEN     32
#define DMA_CNT     8       // 8 × 1 ms = 8 ms headroom; absorbs BLE ISR bursts

// ── State ────────────────────────────────────────────────────
enum Mode : uint8_t { OFF = 0, AMBIENT = 1, ANC = 2 };
volatile Mode  gMode = OFF;
volatile float gGain = 2.0f;

static volatile bool      gConnected = false;
static BLECharacteristic* pLevel     = nullptr;
static BLECharacteristic* pDetect    = nullptr;
static volatile float     gLevel     = 0.0f;

// ── Biquad — Direct Form I ───────────────────────────────────
struct Biquad {
  float b0=1,b1=0,b2=0,a1=0,a2=0, x1=0,x2=0,y1=0,y2=0;

  inline float tick(float x) {
    float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
    x2=x1; x1=x; y2=y1; y1=y;
    return y;
  }

  void setHP(float fc, float Q) {
    double w=2.0*M_PI*fc/FS, c=cos(w), s=sin(w), a=s/(2.0*Q), a0=1+a;
    b0=(float)(( 1+c)*.5/a0); b1=(float)(-(1+c)/a0); b2=b0;
    a1=(float)(-2*c/a0);      a2=(float)((1-a)/a0);
    x1=x2=y1=y2=0;
  }

  void setLP(float fc, float Q) {
    double w=2.0*M_PI*fc/FS, c=cos(w), s=sin(w), a=s/(2.0*Q), a0=1+a;
    b0=(float)(( 1-c)*.5/a0); b1=(float)((1-c)/a0); b2=b0;
    a1=(float)(-2*c/a0);      a2=(float)((1-a)/a0);
    x1=x2=y1=y2=0;
  }
};

static Biquad hp;      // 60 Hz   — removes DC / low-frequency handling rumble
static Biquad lp;      // 10 kHz  — anti-alias for 8-bit DAC at 32 kHz FS
static Biquad ancLp1;  // 1.5 kHz — first ANC LP (12 dB/oct)
static Biquad ancLp2;  // 1.5 kHz — second ANC LP (24 dB/oct total)

// ── TPDF dither ──────────────────────────────────────────────
static uint32_t r0=0xDEADBEEF, r1=0xCAFEBABE;
static inline float tpdf() {
  r0^=r0<<13; r0^=r0>>17; r0^=r0<<5;
  r1^=r1<<13; r1^=r1>>17; r1^=r1<<5;
  return ((float)(r0&0xFF)-(float)(r1&0xFF))/255.0f;
}

// ── BLE ───────────────────────────────────────────────────────
static uint8_t rd8(BLECharacteristic* c) {
  return c->getLength()>=1 ? c->getData()[0] : 0;
}

struct ConnCB : BLEServerCallbacks {
  void onConnect(BLEServer*) override    { gConnected=true;  Serial.println("[BLE] on"); }
  void onDisconnect(BLEServer*) override {
    gConnected=false; BLEDevice::startAdvertising();
    Serial.println("[BLE] off");
  }
};
struct ModeCB : BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    uint8_t v=rd8(c); if(v<=2) gMode=(Mode)v;
  }
};
struct GainCB : BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    gGain=constrain(rd8(c)/10.0f,0.5f,20.0f);
  }
};
struct Stub : BLECharacteristicCallbacks { void onWrite(BLECharacteristic*) override {} };

static void setupBLE() {
  BLEDevice::init("SoundAmp-IoT");
  auto* srv=BLEDevice::createServer(); srv->setCallbacks(new ConnCB());
  auto* svc=srv->createService(BLEUUID(SERVICE_UUID),30);

  const uint32_t RW=BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_WRITE;
  const uint32_t RN=BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY;

  auto mk=[&](const char* u,uint32_t p,BLECharacteristicCallbacks* cb,uint8_t d){
    auto* ch=svc->createCharacteristic(u,p); if(cb)ch->setCallbacks(cb);
    ch->setValue(&d,1); return ch;
  };

  mk(MODE_UUID,      RW, new ModeCB(), 0);
  mk(GAIN_UUID,      RW, new GainCB(), 20);
  mk(NOISEGATE_UUID, RW, new Stub(),   1);
  mk(BASSBOOST_UUID, RW, new Stub(),   0);
  mk(EQ_UUID,        RW, new Stub(),   0);
  mk(AMBLVL_UUID,    RW, new Stub(),   2);

  pLevel  = mk(LEVEL_UUID,  RN, nullptr, 0); pLevel->addDescriptor(new BLE2902());
  pDetect = mk(DETECT_UUID, RN, nullptr, 0); pDetect->addDescriptor(new BLE2902());

  svc->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->setScanResponse(true);
  BLEDevice::getAdvertising()->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] SoundAmp-IoT ready");
}

static void setupI2S() {
  i2s_config_t cfg = {
    .mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate       = FS,
    .bits_per_sample   = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format    = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count     = DMA_CNT,
    .dma_buf_len       = DMA_LEN,
    .use_apll          = false,
    .tx_desc_auto_clear= false,
    .fixed_mclk        = 0,
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);

  // memset to 0xFF so every field starts as I2S_PIN_NO_CHANGE (-1).
  // Works with any ESP32 core version: older cores lack mck_io_num so
  // positional / designated init fails; memset is always safe.
  // Positional init was the root cause of "mclk config failed" — the
  // new first field mck_io_num received SCK_PIN(32), which the ESP32
  // hardware only supports on GPIO 0/1/3.
  i2s_pin_config_t pins;
  memset(&pins, 0xFF, sizeof(pins));   // 0xFF bytes → int = -1 = I2S_PIN_NO_CHANGE
  pins.bck_io_num   = SCK_PIN;
  pins.ws_io_num    = WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = SD_PIN;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ── Audio task ────────────────────────────────────────────────
// ANC feedback gate thresholds (in smoothed absolute sample counts)
#define ANC_GATE_LO  12.0f   // ~2× INMP441 self-noise floor
#define ANC_GATE_HI  28.0f   // full ANC above this (any real ambient noise)

void audioTask(void*) {
  hp.setHP(60.0f,    0.707f);
  lp.setLP(10000.0f, 0.707f);
  ancLp1.setLP(1500.0f, 0.707f);
  ancLp2.setLP(1500.0f, 0.707f);

  dac_output_enable(DAC_L); dac_output_enable(DAC_R);
  dac_output_voltage(DAC_L, 128); dac_output_voltage(DAC_R, 128);

  // ── Comb-matched delay buffer ─────────────────────────────
  // Simple phase inversion with 1 ms DMA latency creates a comb filter
  // whose DESTRUCTIVE peak sits at 1/(2×1ms) = 500 Hz — exactly the
  // middle of the voice/traffic band.  The anti-noise at 500 Hz arrives
  // in-phase with the noise and makes it louder.
  //
  // Fix: feed the ANC path with (x[n] + x[n-32]) / 2 instead of x[n].
  // This comb-matched pre-filter has frequency response |cos(π·f·T)|:
  //   • Zero    at 500 Hz, 1.5 kHz … (destructive peaks → anti-noise silenced)
  //   • Maximum at   0 Hz, 1 kHz, 2 kHz … (cancellation peaks → full anti-noise)
  // The LP at 1.5 kHz then limits the output to the two best cancellation
  // bands: sub-bass (0–250 Hz) and the 1 kHz voice-fundamental zone.
  static float combBuf[DMA_LEN];   // zero-initialised (static)
  static int   combIdx = 0;

  int32_t buf[DMA_LEN];
  size_t  rd;
  float   ramp     = 0.0f;
  bool    dacSleep = false;

  while (true) {
    i2s_read(I2S_PORT, buf, sizeof(buf), &rd, portMAX_DELAY);
    Mode  mode = gMode;
    float gain = fminf(gGain, 5.0f);
    int   n    = (int)(rd / sizeof(int32_t));

    for (int i = 0; i < n; i++) {

      // INMP441: 24-bit left-justified in 32-bit word → >>14 = ±131072
      float x = (float)(buf[i] >> 14);

      // Remove DC/rumble (HP 60 Hz) and anti-alias (LP 10 kHz)
      x = lp.tick(hp.tick(x));

      // VU meter — ~3 ms time constant at 32 kHz
      gLevel = gLevel * 0.99f + fabsf(x) * 0.01f;

      // Comb-matched pre-filter: x[n] averaged with x[n-32] (1 ms ago).
      // Zeros at 500 Hz & 1.5 kHz stop the anti-noise from reinforcing
      // noise at those frequencies.  Peaks at 1 kHz provide cancellation
      // exactly where simple inversion was most harmful before.
      float delayed    = combBuf[combIdx];
      combBuf[combIdx] = x;
      combIdx          = (combIdx + 1) % DMA_LEN;
      float combSig    = (x + delayed) * 0.5f;

      // LP at 1.5 kHz (24 dB/oct) keeps only the two useful bands:
      //   0–250 Hz  (sub-bass: best phase alignment, traffic/HVAC rumble)
      //   ~1 kHz    (comb cancellation peak: voice fundamentals)
      float ancSig = ancLp2.tick(ancLp1.tick(combSig));

      float tr = (mode != OFF) ? 1.0f : 0.0f;
      ramp += (tr - ramp) * 0.00208f;

      // ── DAC sleep ──────────────────────────────────────────
      // When OFF and ramp settled, park DAC at 128 and stop all writes.
      // Continuous writes at 32 kHz produce audible switching artefacts
      // and TPDF noise even with a "zero" signal on sensitive earphones.
      if (mode == OFF && ramp < 0.001f) {
        if (!dacSleep) {
          dac_output_voltage(DAC_L, 128);
          dac_output_voltage(DAC_R, 128);
          dacSleep = true;
        }
        continue;
      }
      dacSleep = false;

      float out = 0.0f;

      if (mode == ANC) {
        // Soft gate: fades ANC to zero in near-silence so the
        // earphone speaker cannot sustain a feedback oscillation.
        float gate = fmaxf(0.0f,
                     fminf(1.0f, (gLevel - ANC_GATE_LO)
                                 / (ANC_GATE_HI - ANC_GATE_LO)));
        out = -ancSig * gain * gate;

      } else if (mode == AMBIENT) {
        out = x * gain;
      }

      out *= ramp;

      // Tanh soft limiter — knee at 8192 keeps output linear to ~80 dB SPL
      float dv = 127.0f * tanhf(out / 8192.0f);
      dv += tpdf();

      uint8_t d = (uint8_t)constrain((int)(dv + 128.5f), 0, 255);
      dac_output_voltage(DAC_L, d);
      dac_output_voltage(DAC_R, d);
    }
  }
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("==== SoundAmp IoT v3 ====");
  setupBLE();
  setupI2S();
  xTaskCreatePinnedToCore(audioTask, "Audio", 12288, NULL,
                          configMAX_PRIORITIES - 1, NULL, 1);
  Serial.println("Ready.");
}

void loop() {
  static unsigned long t = 0;
  if (gConnected && millis() - t >= 200) {
    if (pLevel) {
      uint8_t v = (uint8_t)constrain((int)gLevel, 0, 255);
      pLevel->setValue(&v, 1); pLevel->notify();
    }
    if (pDetect) { uint8_t z = 0; pDetect->setValue(&z, 1); pDetect->notify(); }
    t = millis();
  }
  delay(10);
}
