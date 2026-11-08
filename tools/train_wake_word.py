import os
import glob
import wave
import json
import time
import random
import io
import urllib.request
import numpy as np
from scipy import signal
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim

SAMPLE_RATE = 16000
DURATION_SEC = 1.5
TOTAL_SAMPLES = 24000
FRAME_LEN = 480
FRAME_STEP = 160
N_MELS = 40
N_FFT = 512
NUM_FRAMES = 148

API_URL = "http://127.0.0.1:5300/api/speech"
API_TOKEN = "kouki-demo"

def synthesize_audio(text, voice=None):
    payload = {"text": text}
    if voice: payload["voice"] = voice
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        API_URL, data=data,
        headers={"Authorization": f"Bearer {API_TOKEN}", "Content-Type": "application/json"}
    )
    for _ in range(3):
        try:
            with urllib.request.urlopen(req, timeout=10.0) as resp:
                if resp.status == 200:
                    raw = resp.read()
                    with wave.open(io.BytesIO(raw), "rb") as w:
                        n = w.getnframes()
                        b = w.readframes(n)
                        return np.frombuffer(b, dtype=np.int16).astype(np.float32) / 32768.0
        except Exception:
            time.sleep(0.3)
    return None

def mel_filterbank(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS, fmin=80.0, fmax=7600.0):
    hz_to_mel = lambda hz: 2595.0 * np.log10(1.0 + hz / 700.0)
    mel_to_hz = lambda mel: 700.0 * (10.0 ** (mel / 2595.0) - 1.0)
    mel_points = np.linspace(hz_to_mel(fmin), hz_to_mel(fmax), n_mels + 2)
    hz_points = mel_to_hz(mel_points)
    bins = np.floor((n_fft + 1) * hz_points / sr).astype(int)
    weights = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for i in range(1, n_mels + 1):
        left, center, right = bins[i-1], bins[i], bins[i+1]
        for j in range(left, center):
            if center > left: weights[i-1, j] = (j - left) / (center - left)
        for j in range(center, right):
            if right > center: weights[i-1, j] = (right - j) / (right - center)
    return weights

MEL_FB = mel_filterbank()
HANN_WINDOW = np.hanning(FRAME_LEN).astype(np.float32)

def compute_mel_spectrogram(pcm):
    if len(pcm) < TOTAL_SAMPLES:
        pcm = np.pad(pcm, (0, TOTAL_SAMPLES - len(pcm)))
    else:
        pcm = pcm[:TOTAL_SAMPLES]
    spec = np.zeros((NUM_FRAMES, N_MELS), dtype=np.float32)
    for i in range(NUM_FRAMES):
        start = i * FRAME_STEP
        frame = pcm[start:start + FRAME_LEN] * HANN_WINDOW
        fft_vals = np.fft.rfft(frame, n=N_FFT)
        power_spectrum = (np.abs(fft_vals) ** 2) / FRAME_LEN
        mel_energies = np.dot(MEL_FB, power_spectrum)
        spec[i] = np.log(mel_energies + 1e-6)
    mean = np.mean(spec)
    std = np.std(spec) + 1e-6
    return (spec - mean) / std

def generate_pink_noise(n_samples):
    white = np.random.normal(0, 1, n_samples).astype(np.float32)
    fft_vals = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n_samples, d=1.0 / SAMPLE_RATE)
    freqs[0] = 1.0
    alpha = random.uniform(0.7, 1.6)
    pink_fft = fft_vals / (freqs ** (alpha / 2.0))
    pink = np.fft.irfft(pink_fft, n=n_samples)
    scale = np.max(np.abs(pink)) + 1e-6
    return (pink / scale * random.uniform(0.003, 0.05)).astype(np.float32)

def generate_impulse_noise(n_samples):
    out = np.zeros(n_samples, dtype=np.float32)
    n_clicks = random.randint(1, 4)
    for _ in range(n_clicks):
        pos = random.randint(0, n_samples - 800)
        dur = random.randint(50, 400)
        decay = np.exp(-np.linspace(0, 5, dur))
        freq = random.uniform(800, 4000)
        t = np.arange(dur) / float(SAMPLE_RATE)
        tone = np.sin(2 * np.pi * freq * t) * decay
        amp = random.uniform(0.1, 0.8)
        out[pos:pos+dur] += tone * amp
    return np.clip(out, -1.0, 1.0)

print("[1] Загрузка 4 живых записей «эй гин» пользователя со станции...")
user_snippets = []
for i in range(1, 5):
    path = f"/srv/kouki-workspace/_scratch/user_ey_gin_{i}.wav"
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        pcm = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float32) / 32768.0
        user_snippets.append(pcm)

print("[2] Загрузка фоновых шумов и негативных срезов станции с шагом 100мс...")
all_station_files = sorted(glob.glob("/var/lib/kouki/audio/station/*/*.wav"))
user_raw_files = ["084613", "085638", "085702", "085840"]
negative_station_files = [f for f in all_station_files if not any(uf in f for uf in user_raw_files)]

station_slices = []
room_noises = []
hard_190939_slices = []

for f in negative_station_files:
    try:
        with wave.open(f, "rb") as w:
            n = w.getnframes()
            pcm = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float32) / 32768.0
        is_190939 = "190939" in f
        step = int(0.08 * SAMPLE_RATE) if is_190939 else int(0.10 * SAMPLE_RATE)
        for i in range(0, len(pcm) - TOTAL_SAMPLES, step):
            sl = pcm[i:i + TOTAL_SAMPLES]
            amp = np.max(np.abs(sl))
            if amp > 0.04:
                station_slices.append(sl)
                if is_190939:
                    hard_190939_slices.append(sl)
            elif amp > 0.002:
                room_noises.append(sl)
    except Exception:
        pass

print(f"Собрано {len(station_slices)} срезов реальной речи станции (включая hard 190939: {len(hard_190939_slices)})")
print(f"Собрано {len(room_noises)} фрагментов комнатного шума станции")

print("[3] Синтез мульти-голосов Piper...")
piper_voices = [None, "ru_RU-dmitri-medium", "ru_RU-denis-medium"]
piper_positives = []
pos_texts = ["эй гин", "Эй, гин", "эй гин!", "эйгин", "эй, гин."]
for v in piper_voices:
    for t in pos_texts:
        pcm = synthesize_audio(t, voice=v)
        if pcm is not None:
            piper_positives.append(pcm)

piper_negatives = []
neg_texts = [
    "привет", "пельмени", "расскажи про пельмени", "супер", "понятно",
    "эй", "гин", "эти", "игры", "один", "деньги", "книги", "режим",
    "да", "нет", "стоп", "включи", "выключи", "смешка", "нормально",
    "странно работает", "я не понимаю", "что делаешь", "сколько времени",
    "проверяйте", "запиши и проверяйте", "алиса", "яндекс", "сири"
]
for v in piper_voices:
    for t in neg_texts:
        pcm = synthesize_audio(t, voice=v)
        if pcm is not None:
            piper_negatives.append(pcm)

def augment_sample(pcm, is_positive=True, is_user=False):
    speed = random.uniform(0.85, 1.15)
    new_len = int(len(pcm) / speed)
    pcm_aug = signal.resample(pcm, new_len) if new_len > 100 else pcm.copy()

    if not is_user and random.random() < 0.6:
        pitch = random.uniform(0.80, 1.25)
        pitch_len = int(len(pcm_aug) / pitch)
        if pitch_len > 100:
            pcm_aug = signal.resample(pcm_aug, pitch_len)
            pcm_aug = signal.resample(pcm_aug, new_len)

    pcm_aug = pcm_aug * random.uniform(0.5, 1.3)

    out = np.zeros(TOTAL_SAMPLES, dtype=np.float32)
    if len(pcm_aug) < TOTAL_SAMPLES:
        if is_positive:
            min_shift = max(0, TOTAL_SAMPLES - len(pcm_aug) - int(0.35 * SAMPLE_RATE))
            max_shift = TOTAL_SAMPLES - len(pcm_aug)
            shift = random.randint(min_shift, max_shift)
        else:
            shift = random.randint(0, TOTAL_SAMPLES - len(pcm_aug))
        out[shift:shift + len(pcm_aug)] = pcm_aug
    else:
        out = pcm_aug[:TOTAL_SAMPLES]

    # ДОБАВЛЕНИЕ РЕАЛЬНОГО ШУМА МИКРОФОНА СТАНЦИИ
    noise_choice = random.random()
    if len(room_noises) > 0 and noise_choice < 0.60:
        noise = random.choice(room_noises) * random.uniform(0.3, 1.0)
    elif len(station_slices) > 0 and noise_choice < 0.85:
        noise = random.choice(station_slices) * random.uniform(0.1, 0.35)
    else:
        noise = generate_pink_noise(TOTAL_SAMPLES)

    return np.clip(out + noise, -1.0, 1.0)

print("[4] Сбор сбалансированного датасета (3000 pos vs 8000 neg)...")
X = []
y = []

# 1) Живой голос пользователя (1800 позитивов)
for _ in range(1800):
    src = random.choice(user_snippets)
    aug = augment_sample(src, is_positive=True, is_user=True)
    X.append(compute_mel_spectrogram(aug))
    y.append(1.0)

# 2) Мульти-голоса Piper (1200 позитивов)
for _ in range(1200):
    src = random.choice(piper_positives)
    aug = augment_sample(src, is_positive=True, is_user=False)
    X.append(compute_mel_spectrogram(aug))
    y.append(1.0)

# 3) Негативы (всего 8000):
# - 4500 реальной речи со станции
for src in random.sample(station_slices, min(4500, len(station_slices))):
    gain = random.uniform(0.6, 1.4)
    X.append(compute_mel_spectrogram(np.clip(src * gain, -1.0, 1.0)))
    y.append(0.0)

# - 1000 срезов hard 190939 с аугментацией
if len(hard_190939_slices) > 0:
    for _ in range(1000):
        src = random.choice(hard_190939_slices)
        gain = random.uniform(0.7, 1.3)
        X.append(compute_mel_spectrogram(np.clip(src * gain, -1.0, 1.0)))
        y.append(0.0)

# - 2000 синтетических негативных фраз мульти-голосом
for _ in range(2000):
    src = random.choice(piper_negatives)
    aug = augment_sample(src, is_positive=False, is_user=False)
    X.append(compute_mel_spectrogram(aug))
    y.append(0.0)

# - 500 импульсных шумов и щелчков
for _ in range(500):
    imp = generate_impulse_noise(TOTAL_SAMPLES)
    if random.random() < 0.5:
        imp += generate_pink_noise(TOTAL_SAMPLES)
    X.append(compute_mel_spectrogram(imp))
    y.append(0.0)

X = np.array(X, dtype=np.float32)
y = np.array(y, dtype=np.float32)
perm = np.random.permutation(len(X))
X = X[perm]
y = y[perm]
print(f"  Датасет готов: {len(X)} сэмплов (pos: {int(np.sum(y))}, neg: {len(y) - int(np.sum(y))})")

class MicroWakeWordModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(1, 16, kernel_size=(5, 5), stride=(2, 2), padding=2, bias=True)
        self.bn1 = nn.BatchNorm2d(16)
        self.relu1 = nn.ReLU()

        self.dw2 = nn.Conv2d(16, 16, kernel_size=(3, 3), stride=(2, 2), padding=1, groups=16, bias=True)
        self.pw2 = nn.Conv2d(16, 32, kernel_size=1, bias=True)
        self.bn2 = nn.BatchNorm2d(32)
        self.relu2 = nn.ReLU()

        self.dw3 = nn.Conv2d(32, 32, kernel_size=(3, 3), stride=(2, 2), padding=1, groups=32, bias=True)
        self.pw3 = nn.Conv2d(32, 64, kernel_size=1, bias=True)
        self.bn3 = nn.BatchNorm2d(64)
        self.relu3 = nn.ReLU()

        self.fc1 = nn.Linear(64 * 4, 32)
        self.relu_fc = nn.ReLU()
        self.drop = nn.Dropout(0.2)
        self.fc2 = nn.Linear(32, 1)

    def forward(self, x):
        x = self.relu1(self.bn1(self.conv1(x)))
        x = self.relu2(self.bn2(self.pw2(self.dw2(x))))
        x = self.relu3(self.bn3(self.pw3(self.dw3(x))))
        x = torch.mean(x, dim=3)
        x = F.adaptive_avg_pool1d(x, 4)
        x = torch.flatten(x, 1)
        x = self.drop(self.relu_fc(self.fc1(x)))
        x = self.fc2(x)
        return x

split_idx = int(0.85 * len(X))
X_train = torch.tensor(X[:split_idx], dtype=torch.float32).unsqueeze(1)
y_train = torch.tensor(y[:split_idx], dtype=torch.float32).unsqueeze(1)
X_val = torch.tensor(X[split_idx:], dtype=torch.float32).unsqueeze(1)
y_val = torch.tensor(y[split_idx:], dtype=torch.float32).unsqueeze(1)

loader = torch.utils.data.DataLoader(torch.utils.data.TensorDataset(X_train, y_train), batch_size=64, shuffle=True)
model = MicroWakeWordModel()

criterion = nn.BCEWithLogitsLoss(pos_weight=torch.tensor([0.6]))
optimizer = optim.Adam(model.parameters(), lr=0.001)

print("[5] Обучение модели...")
for epoch in range(1, 16):
    model.train()
    total_loss = 0.0
    for bx, by in loader:
        optimizer.zero_grad()
        out = model(bx)
        loss = criterion(out, by)
        loss.backward()
        optimizer.step()
        total_loss += loss.item()
    model.eval()
    with torch.no_grad():
        logits = model(X_val)
        probs = torch.sigmoid(logits)
        val_acc = ((probs >= 0.5) == y_val).float().mean().item()
        neg_mask = (y_val == 0.0)
        pos_mask = (y_val == 1.0)
        fp_rate = (probs[neg_mask] >= 0.5).float().mean().item()
        tp_rate = (probs[pos_mask] >= 0.5).float().mean().item()
        print(f"  Эпоха {epoch:02d}: loss={total_loss/len(loader):.4f}, val_acc={val_acc*100:.2f}%, Recall={tp_rate*100:.2f}%, FP_rate={fp_rate*100:.2f}%")

print("[6] INT8 квантование и экспорт...")
def fold_conv_bn(conv, bn):
    gamma = bn.weight.detach().numpy()
    beta = bn.bias.detach().numpy()
    mean = bn.running_mean.detach().numpy()
    var = bn.running_var.detach().numpy()
    eps = bn.eps
    w = conv.weight.detach().numpy()
    b = conv.bias.detach().numpy() if conv.bias is not None else np.zeros(w.shape[0], dtype=np.float32)
    scale = gamma / np.sqrt(var + eps)
    w_folded = w * scale[:, None, None, None]
    b_folded = (b - mean) * scale + beta
    return w_folded, b_folded

def quantize_weights(w):
    max_val = np.max(np.abs(w))
    scale = float(max_val / 127.0) if max_val > 1e-7 else 1.0
    int8_w = np.clip(np.round(w / scale), -128, 127).astype(np.int8)
    return int8_w, scale

model.eval()
w1, b1 = fold_conv_bn(model.conv1, model.bn1)
w2_dw = model.dw2.weight.detach().numpy()
b2_dw = model.dw2.bias.detach().numpy() if model.dw2.bias is not None else np.zeros(16, dtype=np.float32)
w2_pw, b2_pw = fold_conv_bn(model.pw2, model.bn2)
b2_pw += np.sum(w2_pw * b2_dw[None, :, None, None], axis=(1, 2, 3))

w3_dw = model.dw3.weight.detach().numpy()
b3_dw = model.dw3.bias.detach().numpy() if model.dw3.bias is not None else np.zeros(32, dtype=np.float32)
w3_pw, b3_pw = fold_conv_bn(model.pw3, model.bn3)
b3_pw += np.sum(w3_pw * b3_dw[None, :, None, None], axis=(1, 2, 3))

w_fc1 = model.fc1.weight.detach().numpy()
b_fc1 = model.fc1.bias.detach().numpy()
w_fc2 = model.fc2.weight.detach().numpy()
b_fc2 = model.fc2.bias.detach().numpy()

w1_q, s_w1 = quantize_weights(w1)
w2_dw_q, s_w2_dw = quantize_weights(w2_dw)
w2_pw_q, s_w2_pw = quantize_weights(w2_pw)
w3_dw_q, s_w3_dw = quantize_weights(w3_dw)
w3_pw_q, s_w3_pw = quantize_weights(w3_pw)
w_fc1_q, s_fc1 = quantize_weights(w_fc1)
w_fc2_q, s_fc2 = quantize_weights(w_fc2)

total_weights_bytes = (
    w1_q.size + w2_dw_q.size + w2_pw_q.size +
    w3_dw_q.size + w3_pw_q.size +
    w_fc1_q.size + w_fc2_q.size
)
print(f"Размер INT8 весов: {total_weights_bytes} байт ({total_weights_bytes / 1024.0:.2f} КБ)")

out_h = "/srv/kouki-workspace/kouki-station/firmware/main/wake_model_ey_gin.h"

def write_arr(f, name, arr, dtype="int8_t"):
    flat = arr.flatten()
    f.write(f"static const {dtype} {name}[{len(flat)}] = {{\n")
    for i in range(0, len(flat), 16):
        chunk = flat[i:i+16]
        line = ", ".join(f"{v:d}" if "int" in dtype else f"{v:.7f}f" for v in chunk)
        comma = "," if i + 16 < len(flat) else ""
        f.write(f"    {line}{comma}\n")
    f.write("};\n\n")

with open(out_h, "w", encoding="utf-8") as f:
    f.write("""/*
 * wake_model_ey_gin.h — microWakeWord детектор «эй гин» для ESP32-S3 (INT8)
 * Обучен на живых записях станции и мульти-голосах Piper.
 */

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define WAKE_MODEL_EY_GIN_LEN """ + str(total_weights_bytes) + """
#define WAKE_MODEL_FRAMES     148
#define WAKE_MODEL_MEL_BINS   40
#define WAKE_MODEL_MELS       40
#define WAKE_MODEL_ARENA_FLOATS 35520

""")
    write_arr(f, "g_w1", w1_q, "int8_t")
    write_arr(f, "g_b1", b1, "float")
    f.write(f"static const float g_s_w1 = {s_w1:.8f}f;\n\n")

    write_arr(f, "g_w2_dw", w2_dw_q, "int8_t")
    f.write(f"static const float g_s_w2_dw = {s_w2_dw:.8f}f;\n\n")
    write_arr(f, "g_w2_pw", w2_pw_q, "int8_t")
    write_arr(f, "g_b2_pw", b2_pw, "float")
    f.write(f"static const float g_s_w2_pw = {s_w2_pw:.8f}f;\n\n")

    write_arr(f, "g_w3_dw", w3_dw_q, "int8_t")
    f.write(f"static const float g_s_w3_dw = {s_w3_dw:.8f}f;\n\n")
    write_arr(f, "g_w3_pw", w3_pw_q, "int8_t")
    write_arr(f, "g_b3_pw", b3_pw, "float")
    f.write(f"static const float g_s_w3_pw = {s_w3_pw:.8f}f;\n\n")

    write_arr(f, "g_w_fc1", w_fc1_q, "int8_t")
    write_arr(f, "g_b_fc1", b_fc1, "float")
    f.write(f"static const float g_s_fc1 = {s_fc1:.8f}f;\n\n")

    write_arr(f, "g_w_fc2", w_fc2_q, "int8_t")
    write_arr(f, "g_b_fc2", b_fc2, "float")
    f.write(f"static const float g_s_fc2 = {s_fc2:.8f}f;\n\n")

    f.write("""__attribute__((aligned(16))) static const uint8_t g_wake_model_ey_gin_data[WAKE_MODEL_EY_GIN_LEN] = {0};\n\n""")

    f.write("""
static inline float wake_model_ey_gin_infer(const float *spec, float *arena)
{
    float *dyn_arena = NULL;
    if (!arena) {
        dyn_arena = (float *)malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float));
        if (!dyn_arena) return 0.0f;
        arena = dyn_arena;
    }

    float *act1 = arena;
    float *act2 = arena + (74 * 20 * 16);
    float *act3 = act1;

    for (int oh = 0; oh < 74; oh++) {
        for (int ow = 0; ow < 20; ow++) {
            for (int oc = 0; oc < 16; oc++) {
                float sum = g_b1[oc];
                for (int kh = 0; kh < 5; kh++) {
                    int ih = oh * 2 - 2 + kh;
                    if (ih < 0 || ih >= 148) continue;
                    for (int kw = 0; kw < 5; kw++) {
                        int iw = ow * 2 - 2 + kw;
                        if (iw < 0 || iw >= 40) continue;
                        int w_idx = oc * 25 + kh * 5 + kw;
                        sum += spec[ih * 40 + iw] * (g_w1[w_idx] * g_s_w1);
                    }
                }
                act1[(oh * 20 + ow) * 16 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    for (int oh = 0; oh < 37; oh++) {
        for (int ow = 0; ow < 10; ow++) {
            float dw_out[16];
            for (int c = 0; c < 16; c++) {
                float sum = 0.0f;
                for (int kh = 0; kh < 3; kh++) {
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 74) continue;
                    for (int kw = 0; kw < 3; kw++) {
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 20) continue;
                        int w_idx = c * 9 + kh * 3 + kw;
                        sum += act1[(ih * 20 + iw) * 16 + c] * (g_w2_dw[w_idx] * g_s_w2_dw);
                    }
                }
                dw_out[c] = sum;
            }
            for (int oc = 0; oc < 32; oc++) {
                float sum = g_b2_pw[oc];
                const int8_t *w_ptr = &g_w2_pw[oc * 16];
                for (int ic = 0; ic < 16; ic++) {
                    sum += dw_out[ic] * (w_ptr[ic] * g_s_w2_pw);
                }
                act2[(oh * 10 + ow) * 32 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    for (int oh = 0; oh < 19; oh++) {
        for (int ow = 0; ow < 5; ow++) {
            float dw_out[32];
            for (int c = 0; c < 32; c++) {
                float sum = 0.0f;
                for (int kh = 0; kh < 3; kh++) {
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 37) continue;
                    for (int kw = 0; kw < 3; kw++) {
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 10) continue;
                        int w_idx = c * 9 + kh * 3 + kw;
                        sum += act2[(ih * 10 + iw) * 32 + c] * (g_w3_dw[w_idx] * g_s_w3_dw);
                    }
                }
                dw_out[c] = sum;
            }
            for (int oc = 0; oc < 64; oc++) {
                float sum = g_b3_pw[oc];
                const int8_t *w_ptr = &g_w3_pw[oc * 32];
                for (int ic = 0; ic < 32; ic++) {
                    sum += dw_out[ic] * (w_ptr[ic] * g_s_w3_pw);
                }
                act3[(oh * 5 + ow) * 64 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    float *t_feats = act2;
    float *c_in = act2 + (19 * 64);

    for (int t = 0; t < 19; t++) {
        for (int c = 0; c < 64; c++) {
            float msum = act3[(t * 5 + 0) * 64 + c] +
                         act3[(t * 5 + 1) * 64 + c] +
                         act3[(t * 5 + 2) * 64 + c] +
                         act3[(t * 5 + 3) * 64 + c] +
                         act3[(t * 5 + 4) * 64 + c];
            t_feats[t * 64 + c] = msum * 0.2f;
        }
    }

    for (int c = 0; c < 64; c++) {
        float s0 = (t_feats[0 * 64 + c] + t_feats[1 * 64 + c] + t_feats[2 * 64 + c] +
                    t_feats[3 * 64 + c] + t_feats[4 * 64 + c]) * 0.2f;
        float s1 = (t_feats[4 * 64 + c] + t_feats[5 * 64 + c] + t_feats[6 * 64 + c] +
                    t_feats[7 * 64 + c] + t_feats[8 * 64 + c] + t_feats[9 * 64 + c]) * (1.0f / 6.0f);
        float s2 = (t_feats[9 * 64 + c] + t_feats[10 * 64 + c] + t_feats[11 * 64 + c] +
                    t_feats[12 * 64 + c] + t_feats[13 * 64 + c] + t_feats[14 * 64 + c]) * (1.0f / 6.0f);
        float s3 = (t_feats[14 * 64 + c] + t_feats[15 * 64 + c] + t_feats[16 * 64 + c] +
                    t_feats[17 * 64 + c] + t_feats[18 * 64 + c]) * 0.2f;

        c_in[c * 4 + 0] = s0;
        c_in[c * 4 + 1] = s1;
        c_in[c * 4 + 2] = s2;
        c_in[c * 4 + 3] = s3;
    }

    float fc1[32];
    for (int i = 0; i < 32; i++) {
        float sum = g_b_fc1[i];
        const int8_t *w_ptr = &g_w_fc1[i * 256];
        for (int j = 0; j < 256; j++) {
            sum += c_in[j] * (w_ptr[j] * g_s_fc1);
        }
        fc1[i] = (sum > 0.0f) ? sum : 0.0f;
    }

    float logit = g_b_fc2[0];
    for (int i = 0; i < 32; i++) {
        logit += fc1[i] * (g_w_fc2[i] * g_s_fc2);
    }

    if (dyn_arena) free(dyn_arena);
    return 1.0f / (1.0f + expf(-logit));
}
""")

print("Экспорт успешно завершен в wake_model_ey_gin.h!")
