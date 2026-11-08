#!/usr/bin/env python3
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
import sqlite3

SAMPLE_RATE = 16000
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

print("[1] Извлечение живых фрагментов «эй гин» пользователя...")
station_files = sorted(glob.glob('/var/lib/kouki/audio/station/20261108/*.wav'))
conn = sqlite3.connect('/var/lib/kouki/db/kouki.sqlite')
c = conn.cursor()

pos_files = []
for fpath in station_files:
    fname = os.path.basename(fpath)
    c.execute("SELECT input_text FROM classifications WHERE audio_path LIKE ?", (f"%{fname}%",))
    r = c.fetchall()
    txt = r[0][0] if r else ""
    t_low = txt.lower()
    if "родина" in t_low or "один" in t_low or "сегодня" in t_low:
        continue
    if any(k in t_low for k in ['гин', 'гим', 'гил', 'дин']) or ('эй' in t_low):
        pos_files.append((fname, fpath))
conn.close()

user_snippets = []
for fname, fpath in pos_files:
    try:
        with wave.open(fpath, 'rb') as w:
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
        win = int(0.2 * 16000)
        energy = np.convolve(np.abs(pcm), np.ones(win)/win, mode='same')
        peak_idx = np.argmax(energy)
        start = max(0, peak_idx - int(0.35 * 16000))
        end = min(len(pcm), peak_idx + int(0.55 * 16000))
        snip = pcm[start:end]
        if len(snip) > 1000 and np.max(np.abs(snip)) > 0.05:
            user_snippets.append(snip)
    except Exception:
        pass

print(f"Извлечено {len(user_snippets)} живых сэмплов пользователя.")

print("[2] Загрузка фоновых шумов и негативных срезов станции с шагом 100мс...")
pos_fnames = set(pf[0] for pf in pos_files)
negative_station_files = [f for f in station_files if os.path.basename(f) not in pos_fnames]

station_slices = []
room_noises = []
hard_negative_slices = []

for f in negative_station_files:
    try:
        with wave.open(f, "rb") as w:
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
        is_hard = any(k in f for k in ["190939", "095610", "102439", "102455", "101815", "101838"])
        step = int(0.08 * SAMPLE_RATE) if is_hard else int(0.10 * SAMPLE_RATE)
        for i in range(0, len(pcm) - TOTAL_SAMPLES, step):
            sl = pcm[i:i + TOTAL_SAMPLES]
            amp = np.max(np.abs(sl))
            if amp > 0.04:
                station_slices.append(sl)
                if is_hard:
                    hard_negative_slices.append(sl)
            elif amp > 0.002:
                room_noises.append(sl)
    except Exception:
        pass

print(f"Собрано {len(station_slices)} срезов реальной речи станции (hard: {len(hard_negative_slices)})")
print(f"Собрано {len(room_noises)} фрагментов комнатного шума станции")

print("[3] Синтез мульти-голосов Piper...")
piper_voices = [None, "ru_RU-dmitri-medium", "ru_RU-denis-medium"]
piper_positives = []
pos_texts = ["эй гин", "Эй, гин", "эй гин!", "эйгин", "эй, гин.", "эй, дин", "эй, гим"]
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
    "проверяйте", "запиши и проверяйте", "алиса", "яндекс", "сири", "здорово"
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

    pcm_aug = pcm_aug * random.uniform(0.5, 1.4)

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

    noise_choice = random.random()
    if noise_choice < 0.70 and len(room_noises) > 0:
        room_bg = random.choice(room_noises)
        noise_gain = random.uniform(0.3, 1.5)
        out = out + room_bg * noise_gain
    elif noise_choice < 0.85:
        out = out + generate_pink_noise(TOTAL_SAMPLES)

    return np.clip(out, -1.0, 1.0)

print("[4] Формирование сбалансированного датасета...")
X = []
y = []

# 1) Живой голос пользователя (2200 позитивов)
for _ in range(2200):
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

# - 1000 срезов hard negatives
if len(hard_negative_slices) > 0:
    for _ in range(1000):
        src = random.choice(hard_negative_slices)
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

split = int(0.85 * len(X))
X_train, X_val = X[:split], X[split:]
y_train, y_val = y[:split], y[split:]

print(f"Датасет: train={len(X_train)} (pos={np.sum(y_train==1)}), val={len(X_val)} (pos={np.sum(y_val==1)})")

class TemporalPoolingWakeNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(1, 16, kernel_size=5, stride=2, padding=2)
        self.bn1 = nn.BatchNorm2d(16)
        self.conv2_dw = nn.Conv2d(16, 16, kernel_size=3, stride=2, padding=1, groups=16, bias=False)
        self.conv2_pw = nn.Conv2d(16, 32, kernel_size=1, bias=True)
        self.bn2 = nn.BatchNorm2d(32)
        self.conv3_dw = nn.Conv2d(32, 32, kernel_size=3, stride=2, padding=1, groups=32, bias=False)
        self.conv3_pw = nn.Conv2d(32, 64, kernel_size=1, bias=True)
        self.bn3 = nn.BatchNorm2d(64)
        self.fc1 = nn.Linear(64 * 4, 32)
        self.fc2 = nn.Linear(32, 1)

    def forward(self, x):
        x = x.unsqueeze(1)
        x = F.relu(self.bn1(self.conv1(x)))
        x = self.conv2_dw(x)
        x = F.relu(self.bn2(self.conv2_pw(x)))
        x = self.conv3_dw(x)
        x = F.relu(self.bn3(self.conv3_pw(x)))
        t_feats = torch.mean(x, dim=3)
        t_pool = F.adaptive_avg_pool1d(t_feats, 4).flatten(1)
        x = F.relu(self.fc1(t_pool))
        x = self.fc2(x)
        return x

model = TemporalPoolingWakeNet()
criterion = nn.BCEWithLogitsLoss(pos_weight=torch.tensor([2.5]))
optimizer = optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=25)

train_ds = torch.utils.data.TensorDataset(torch.tensor(X_train), torch.tensor(y_train).unsqueeze(1))
val_ds = torch.utils.data.TensorDataset(torch.tensor(X_val), torch.tensor(y_val).unsqueeze(1))
train_loader = torch.utils.data.DataLoader(train_ds, batch_size=64, shuffle=True)
val_loader = torch.utils.data.DataLoader(val_ds, batch_size=64, shuffle=False)

print("[5] Обучение модели...")
for epoch in range(1, 26):
    model.train()
    total_loss = 0.0
    for bx, by in train_loader:
        optimizer.zero_grad()
        out = model(bx)
        loss = criterion(out, by)
        loss.backward()
        optimizer.step()
        total_loss += loss.item() * len(bx)
    scheduler.step()

    model.eval()
    val_preds, val_targets = [], []
    with torch.no_grad():
        for bx, by in val_loader:
            out = torch.sigmoid(model(bx))
            val_preds.extend(out.squeeze(1).tolist())
            val_targets.extend(by.squeeze(1).tolist())

    val_preds = np.array(val_preds)
    val_targets = np.array(val_targets)
    pred_bin = (val_preds >= 0.70).astype(float)
    acc = np.mean(pred_bin == val_targets)
    recall = np.sum((pred_bin == 1) & (val_targets == 1)) / (np.sum(val_targets == 1) + 1e-6)
    fpr = np.sum((pred_bin == 1) & (val_targets == 0)) / (np.sum(val_targets == 0) + 1e-6)

    if epoch % 5 == 0 or epoch == 25:
        print(f"Epoch {epoch:2d}/25 | Loss: {total_loss/len(train_ds):.4f} | Val Acc: {acc*100:.2f}% | Recall: {recall*100:.2f}% | FPR: {fpr*100:.2f}%")

print("[6] Квантование и экспорт C-заголовка...")
def quantize_int8(tensor):
    t = tensor.detach().cpu().numpy()
    max_val = np.max(np.abs(t)) + 1e-9
    scale = max_val / 127.0
    q = np.clip(np.round(t / scale), -127, 127).astype(np.int8)
    return q, float(scale)

# Merge BN into Conv
w1 = model.conv1.weight.detach().cpu()
b1 = model.conv1.bias.detach().cpu()
bn1_w = model.bn1.weight.detach().cpu()
bn1_b = model.bn1.bias.detach().cpu()
bn1_mean = model.bn1.running_mean.detach().cpu()
bn1_var = model.bn1.running_var.detach().cpu()
bn1_scale = bn1_w / torch.sqrt(bn1_var + 1e-5)
w1_fused = w1 * bn1_scale.view(-1, 1, 1, 1)
b1_fused = (b1 - bn1_mean) * bn1_scale + bn1_b

w2_dw = model.conv2_dw.weight.detach().cpu()
w2_pw = model.conv2_pw.weight.detach().cpu()
b2_pw = model.conv2_pw.bias.detach().cpu()
bn2_w = model.bn2.weight.detach().cpu()
bn2_b = model.bn2.bias.detach().cpu()
bn2_mean = model.bn2.running_mean.detach().cpu()
bn2_var = model.bn2.running_var.detach().cpu()
bn2_scale = bn2_w / torch.sqrt(bn2_var + 1e-5)
w2_pw_fused = w2_pw * bn2_scale.view(-1, 1, 1, 1)
b2_pw_fused = (b2_pw - bn2_mean) * bn2_scale + bn2_b

w3_dw = model.conv3_dw.weight.detach().cpu()
w3_pw = model.conv3_pw.weight.detach().cpu()
b3_pw = model.conv3_pw.bias.detach().cpu()
bn3_w = model.bn3.weight.detach().cpu()
bn3_b = model.bn3.bias.detach().cpu()
bn3_mean = model.bn3.running_mean.detach().cpu()
bn3_var = model.bn3.running_var.detach().cpu()
bn3_scale = bn3_w / torch.sqrt(bn3_var + 1e-5)
w3_pw_fused = w3_pw * bn3_scale.view(-1, 1, 1, 1)
b3_pw_fused = (b3_pw - bn3_mean) * bn3_scale + bn3_b

q_w1, s_w1 = quantize_int8(w1_fused)
q_w2_dw, s_w2_dw = quantize_int8(w2_dw)
q_w2_pw, s_w2_pw = quantize_int8(w2_pw_fused)
q_w3_dw, s_w3_dw = quantize_int8(w3_dw)
q_w3_pw, s_w3_pw = quantize_int8(w3_pw_fused)
q_w_fc1, s_fc1 = quantize_int8(model.fc1.weight)
q_w_fc2, s_fc2 = quantize_int8(model.fc2.weight)

def format_array(name, arr, is_int=True, per_line=16):
    flat = arr.flatten()
    out = [f"static const {'int8_t' if is_int else 'float'} {name}[{len(flat)}] = {{\n    "]
    for i, v in enumerate(flat):
        if is_int:
            out.append(f"{int(v):4d}")
        else:
            out.append(f"{float(v):.7f}f")
        if i < len(flat) - 1:
            out.append(", ")
            if (i + 1) % per_line == 0:
                out.append("\n    ")
    out.append("\n};\n\n")
    return "".join(out)

header_content = f"""/*
 * wake_model_ey_gin.h — microWakeWord детектор «эй гин» для ESP32-S3 (INT8)
 * Обучен на живых записях станции (23 сэмпла) и мульти-голосах Piper.
 */

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define WAKE_MODEL_EY_GIN_LEN 11616
#define WAKE_MODEL_FRAMES     148
#define WAKE_MODEL_MEL_BINS   40
#define WAKE_MODEL_MELS       40
#define WAKE_MODEL_ARENA_FLOATS 35520

{format_array("g_w1", q_w1, is_int=True)}
{format_array("g_b1", b1_fused.numpy(), is_int=False)}
static const float g_s_w1 = {s_w1:.8f}f;

{format_array("g_w2_dw", q_w2_dw, is_int=True)}
static const float g_s_w2_dw = {s_w2_dw:.8f}f;
{format_array("g_w2_pw", q_w2_pw, is_int=True)}
{format_array("g_b2_pw", b2_pw_fused.numpy(), is_int=False)}
static const float g_s_w2_pw = {s_w2_pw:.8f}f;

{format_array("g_w3_dw", q_w3_dw, is_int=True)}
static const float g_s_w3_dw = {s_w3_dw:.8f}f;
{format_array("g_w3_pw", q_w3_pw, is_int=True)}
{format_array("g_b3_pw", b3_pw_fused.numpy(), is_int=False)}
static const float g_s_w3_pw = {s_w3_pw:.8f}f;

{format_array("g_w_fc1", q_w_fc1, is_int=True)}
{format_array("g_b_fc1", model.fc1.bias.detach().cpu().numpy(), is_int=False)}
static const float g_s_fc1 = {s_fc1:.8f}f;

{format_array("g_w_fc2", q_w_fc2, is_int=True)}
{format_array("g_b_fc2", model.fc2.bias.detach().cpu().numpy(), is_int=False)}
static const float g_s_fc2 = {s_fc2:.8f}f;

static inline float wake_model_ey_gin_infer(const float *spec_normalized, float *arena)
{{
    float *dyn_arena = NULL;
    if (!arena) {{
        dyn_arena = (float *)malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float));
        if (!dyn_arena) return 0.0f;
        arena = dyn_arena;
    }}

    float *act1 = arena;                      /* 74 * 20 * 16 = 23680 */
    float *act2 = act1 + (74 * 20 * 16);      /* 37 * 10 * 32 = 11840 */
    float *act3 = act1;                       /* 19 * 5 * 64 = 6080 */

    /* Слой 1: Conv2D(k=5x5, s=2, p=2) + ReLU, вход 148x40x1, выход 74x20x16 */
    for (int oc = 0; oc < 16; oc++) {{
        float bias = g_b1[oc];
        const int8_t *w_ptr = &g_w1[oc * 25];
        for (int oh = 0; oh < 74; oh++) {{
            for (int ow = 0; ow < 20; ow++) {{
                float sum = bias;
                for (int kh = 0; kh < 5; kh++) {{
                    int ih = oh * 2 - 2 + kh;
                    if (ih < 0 || ih >= 148) continue;
                    for (int kw = 0; kw < 5; kw++) {{
                        int iw = ow * 2 - 2 + kw;
                        if (iw < 0 || iw >= 40) continue;
                        float val = spec_normalized[ih * 40 + iw];
                        int8_t w = w_ptr[kh * 5 + kw];
                        sum += val * (w * g_s_w1);
                    }}
                }}
                act1[(oh * 20 + ow) * 16 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }}
        }}
    }}

    /* Слой 2: Depthwise Conv2D(k=3x3, s=2, p=1) + Pointwise Conv2D(16 -> 32) + ReLU, выход 37x10x32 */
    for (int oh = 0; oh < 37; oh++) {{
        for (int ow = 0; ow < 10; ow++) {{
            float dw_out[16];
            for (int c = 0; c < 16; c++) {{
                float sum = 0.0f;
                for (int kh = 0; kh < 3; kh++) {{
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 74) continue;
                    for (int kw = 0; kw < 3; kw++) {{
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 20) continue;
                        int w_idx = c * 9 + kh * 3 + kw;
                        sum += act1[(ih * 20 + iw) * 16 + c] * (g_w2_dw[w_idx] * g_s_w2_dw);
                    }}
                }}
                dw_out[c] = sum;
            }}
            for (int oc = 0; oc < 32; oc++) {{
                float sum = g_b2_pw[oc];
                const int8_t *w_ptr = &g_w2_pw[oc * 16];
                for (int ic = 0; ic < 16; ic++) {{
                    sum += dw_out[ic] * (w_ptr[ic] * g_s_w2_pw);
                }}
                act2[(oh * 10 + ow) * 32 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }}
        }}
    }}

    /* Слой 3: Depthwise Conv2D(k=3x3, s=2, p=1) + Pointwise Conv2D(32 -> 64) + ReLU, выход 19x5x64 */
    for (int oh = 0; oh < 19; oh++) {{
        for (int ow = 0; ow < 5; ow++) {{
            float dw_out[32];
            for (int c = 0; c < 32; c++) {{
                float sum = 0.0f;
                for (int kh = 0; kh < 3; kh++) {{
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 37) continue;
                    for (int kw = 0; kw < 3; kw++) {{
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 10) continue;
                        int w_idx = c * 9 + kh * 3 + kw;
                        sum += act2[(ih * 10 + iw) * 32 + c] * (g_w3_dw[w_idx] * g_s_w3_dw);
                    }}
                }}
                dw_out[c] = sum;
            }}
            for (int oc = 0; oc < 64; oc++) {{
                float sum = g_b3_pw[oc];
                const int8_t *w_ptr = &g_w3_pw[oc * 32];
                for (int ic = 0; ic < 32; ic++) {{
                    sum += dw_out[ic] * (w_ptr[ic] * g_s_w3_pw);
                }}
                act3[(oh * 5 + ow) * 64 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }}
        }}
    }}

    /* Temporal Pooling: среднее по частотам 5 -> 19x64, затем адаптивный пулинг 19 -> 4, выход 256 */
    float *t_feats = act2;
    float *c_in = act2 + (19 * 64);

    for (int t = 0; t < 19; t++) {{
        for (int c = 0; c < 64; c++) {{
            float msum = act3[(t * 5 + 0) * 64 + c] +
                         act3[(t * 5 + 1) * 64 + c] +
                         act3[(t * 5 + 2) * 64 + c] +
                         act3[(t * 5 + 3) * 64 + c] +
                         act3[(t * 5 + 4) * 64 + c];
            t_feats[t * 64 + c] = msum * 0.2f;
        }}
    }}

    for (int c = 0; c < 64; c++) {{
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
    }}

    /* FC1: 256 -> 32 + ReLU */
    float fc1[32];
    for (int i = 0; i < 32; i++) {{
        float sum = g_b_fc1[i];
        const int8_t *w_ptr = &g_w_fc1[i * 256];
        for (int j = 0; j < 256; j++) {{
            sum += c_in[j] * (w_ptr[j] * g_s_fc1);
        }}
        fc1[i] = (sum > 0.0f) ? sum : 0.0f;
    }}

    /* FC2: 32 -> 1 + Sigmoid */
    float logit = g_b_fc2[0];
    for (int i = 0; i < 32; i++) {{
        logit += fc1[i] * (g_w_fc2[i] * g_s_fc2);
    }}

    if (dyn_arena) free(dyn_arena);
    return 1.0f / (1.0f + expf(-logit));
}}
"""

out_header = "/srv/kouki-workspace/kouki-station/firmware/main/wake_model_ey_gin.h"
with open(out_header, "w") as f:
    f.write(header_content)

print(f"[OK] Экспортирован обновленный C-заголовок в {out_header}!")
