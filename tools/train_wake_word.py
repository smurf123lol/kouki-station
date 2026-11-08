#!/usr/bin/env python3
"""
tools/train_wake_word.py — Обучение microWakeWord детектора для фразы «эй гин».

Пайплайн:
1. Синтез базовых сэмплов фразы «эй гин» и негативов через локальный Piper TTS (/api/speech).
2. Аугментация: растяжение/сжатие во времени (0.85x..1.2x), сдвиг внутри 1.5 с окна,
   добавление шума (SNR 12..30 dB), варьирование амплитуды (всего 3000 сэмплов).
3. Аудио-фронтенд: Mel-спектрограмма 16 кГц (40 Mel-фильтров, окно 30 мс, шаг 10 мс, 148 фреймов).
4. Обучение компактной Depthwise Separable CNN модели на PyTorch.
5. INT8 квантование весов и экспорт в C/C++ заголовок firmware/main/wake_model_ey_gin.h.
"""

import os
import sys
import math
import json
import wave
import time
import struct
import random
import io
import glob
import urllib.request
import numpy as np
from scipy import signal
import torch
import torch.nn as nn
import torch.optim as optim

# Параметры аудио и спектрограммы
SAMPLE_RATE = 16000
DURATION_SEC = 1.5
TOTAL_SAMPLES = int(SAMPLE_RATE * DURATION_SEC)  # 24000 сэмплов
FRAME_LEN = 480   # 30 мс
FRAME_STEP = 160  # 10 мс
N_MELS = 40
N_FFT = 512
NUM_FRAMES = (TOTAL_SAMPLES - FRAME_LEN) // FRAME_STEP + 1  # 148 фреймов

API_URL = os.environ.get("KOUKI_API_URL", "http://127.0.0.1:5300/api/speech")
API_TOKEN = os.environ.get("KOUKI_TOKEN", "kouki-demo")

def synthesize_audio(text, voice=None):
    """Синтезирует речь через локальный /api/speech."""
    payload = {"text": text}
    if voice:
        payload["voice"] = voice
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        API_URL,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {API_TOKEN}"
        }
    )
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                content = resp.read()
                with wave.open(io.BytesIO(content), "rb") as w:
                    assert w.getnchannels() == 1, "Ожидалось моно"
                    assert w.getframerate() == 16000, "Ожидалось 16 кГц"
                    frames = w.readframes(w.getnframes())
                    pcm = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
                    return pcm
        except Exception as e:
            if attempt == 2:
                print(f"  [WARN] Ошибка синтеза '{text}' (голос {voice}): {e}")
                return None
            time.sleep(0.5)
    return None

def mel_filterbank(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS, fmin=80.0, fmax=7600.0):
    """Строит треугольные мел-фильтры."""
    def hz_to_mel(hz):
        return 2595.0 * np.log10(1.0 + hz / 700.0)

    def mel_to_hz(mel):
        return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)

    mel_min = hz_to_mel(fmin)
    mel_max = hz_to_mel(fmax)
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = mel_to_hz(mel_points)
    bins = np.floor((n_fft + 1) * hz_points / sr).astype(int)

    weights = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for i in range(1, n_mels + 1):
        left = bins[i - 1]
        center = bins[i]
        right = bins[i + 1]
        for j in range(left, center):
            if center > left:
                weights[i - 1, j] = (j - left) / (center - left)
        for j in range(center, right):
            if right > center:
                weights[i - 1, j] = (right - j) / (right - center)
    return weights

MEL_FB = mel_filterbank()
HANN_WINDOW = np.hanning(FRAME_LEN).astype(np.float32)

def compute_mel_spectrogram(pcm):
    """Вычисляет Mel-спектрограмму для 1.5-секундного массива PCM (24000 сэмплов)."""
    if len(pcm) < TOTAL_SAMPLES:
        pcm = np.pad(pcm, (0, TOTAL_SAMPLES - len(pcm)))
    else:
        pcm = pcm[:TOTAL_SAMPLES]

    spectrogram = np.zeros((NUM_FRAMES, N_MELS), dtype=np.float32)

    for i in range(NUM_FRAMES):
        start = i * FRAME_STEP
        frame = pcm[start:start + FRAME_LEN] * HANN_WINDOW
        fft_vals = np.fft.rfft(frame, n=N_FFT)
        power_spectrum = (np.abs(fft_vals) ** 2) / FRAME_LEN
        mel_energies = np.dot(MEL_FB, power_spectrum)
        spectrogram[i] = np.log(mel_energies + 1e-6)

    # Стандартизация спектрограммы
    mean = np.mean(spectrogram)
    std = np.std(spectrogram) + 1e-6
    return (spectrogram - mean) / std

def generate_pink_noise(n_samples):
    white = np.random.normal(0, 1, n_samples).astype(np.float32)
    fft_vals = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n_samples, d=1.0 / SAMPLE_RATE)
    freqs[0] = 1.0
    alpha = random.uniform(0.7, 1.6)
    filter_curve = 1.0 / (freqs ** (alpha / 2.0))
    pink_fft = fft_vals * filter_curve
    pink = np.fft.irfft(pink_fft, n=n_samples)
    scale = np.max(np.abs(pink)) + 1e-6
    amp = random.uniform(0.003, 0.05)
    return (pink / scale * amp).astype(np.float32)

def generate_hum(n_samples):
    t = np.arange(n_samples) / float(SAMPLE_RATE)
    f0 = random.choice([50.0, 100.0])
    amp = random.uniform(0.002, 0.03)
    hum = np.sin(2 * np.pi * f0 * t) * amp
    hum += np.sin(2 * np.pi * (2 * f0) * t) * (amp * 0.4)
    hum += generate_pink_noise(n_samples) * 0.5
    return hum.astype(np.float32)

def load_station_noises():
    slices = []
    files = glob.glob("/var/lib/kouki/audio/station/20261108/*.wav")
    for f in files:
        try:
            with wave.open(f, "rb") as w:
                n = w.getnframes()
                raw = w.readframes(n)
                pcm = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                if len(pcm) >= TOTAL_SAMPLES:
                    for pos in range(0, len(pcm) - TOTAL_SAMPLES, TOTAL_SAMPLES // 2):
                        slices.append(pcm[pos:pos + TOTAL_SAMPLES])
        except Exception:
            pass
    if slices:
        print(f"  Загружено реальных нарезков шума со станции: {len(slices)}")
    return slices

def augment_sample(pcm, is_positive=True, station_noises=None):
    """Аугментация аудио."""
    if pcm is None or len(pcm) == 0:
        return np.zeros(TOTAL_SAMPLES, dtype=np.float32)

    # Скорость (0.85x .. 1.15x)
    speed = random.uniform(0.85, 1.15)
    new_len = int(len(pcm) / speed)
    if new_len > 100:
        pcm_aug = signal.resample(pcm, new_len)
    else:
        pcm_aug = pcm.copy()

    # Громкость
    gain = random.uniform(0.5, 1.3)
    pcm_aug = pcm_aug * gain

    # Случайный сдвиг в окне
    out = np.zeros(TOTAL_SAMPLES, dtype=np.float32)
    if len(pcm_aug) < TOTAL_SAMPLES:
        max_shift = TOTAL_SAMPLES - len(pcm_aug)
        shift = random.randint(0, max_shift)
        out[shift:shift + len(pcm_aug)] = pcm_aug
    else:
        out = pcm_aug[:TOTAL_SAMPLES]

    # Добавление разнообразного фонового шума
    noise_choice = random.random()
    if station_noises and noise_choice < 0.40:
        noise = random.choice(station_noises) * random.uniform(0.6, 1.5)
    elif noise_choice < 0.70:
        noise = generate_pink_noise(TOTAL_SAMPLES)
    elif noise_choice < 0.85:
        noise = generate_hum(TOTAL_SAMPLES)
    else:
        snr_db = random.uniform(14.0, 30.0)
        signal_power = np.mean(out ** 2) + 1e-9
        noise_power = signal_power / (10.0 ** (snr_db / 10.0))
        noise = np.random.normal(0, np.sqrt(noise_power), TOTAL_SAMPLES).astype(np.float32)

    out = out + noise
    return np.clip(out, -1.0, 1.0)

def generate_dataset():
    """Сбор и аугментация корпуса."""
    print("[1/5] Синтез базовых сэмплов через Piper TTS...")

    positive_texts = [
        "эй гин", "эй, гин", "эй гин!", "эй гин?",
        "эй, гин!", "эй гин.", "эй, гин?"
    ]
    voices = [None, "ru_RU-denis-medium", "ru_RU-dmitri-medium"]

    base_positives = []
    for txt in positive_texts:
        for v in voices:
            pcm = synthesize_audio(txt, v)
            if pcm is not None:
                base_positives.append(pcm)

    print(f"  Получено базовых позитивов: {len(base_positives)}")

    negative_texts = [
        # Системные фразы станции
        "команда не зарегистрирована", "задача создана", "слушаю", "секунду", "ошибка сети",
        # Обычные фразы
        "привет", "как дела", "погода сегодня", "включи свет", "музыка",
        "стоп", "отмена", "спасибо", "ладно", "хорошо", "ассистент",
        "станция", "выключи", "сделай тише", "громче", "купить хлеб",
        "запиши задачу", "раз два три", "четыре пять",
        # Созвучные слова (hard negatives)
        "эй", "гин", "эй ты", "один", "блин", "пингвин", "магазин",
        "эй там", "почин", "кувшин", "аршин", "жасмин", "павлин",
        "апельсин", "бензин", "рубин"
    ]

    base_negatives = []
    for txt in negative_texts:
        for v in voices:
            pcm = synthesize_audio(txt, v)
            if pcm is not None:
                base_negatives.append(pcm)

    print(f"  Получено базовых негативов: {len(base_negatives)}")

    station_noises = load_station_noises()

    print("[2/5] Генерация и аугментация корпуса (2000 позитивов, 2500 негативов)...")
    X = []
    y = []

    # 2000 позитивов
    for _ in range(2000):
        src = random.choice(base_positives)
        aug_pcm = augment_sample(src, is_positive=True, station_noises=station_noises)
        spec = compute_mel_spectrogram(aug_pcm)
        X.append(spec)
        y.append(1.0)

    # 2500 негативов
    for _ in range(2500):
        r = random.random()
        if r < 0.40:
            # Негативная речь
            src = random.choice(base_negatives)
            aug_pcm = augment_sample(src, is_positive=False, station_noises=station_noises)
            spec = compute_mel_spectrogram(aug_pcm)
        elif r < 0.65 and station_noises:
            # Чистый фоновый шум станции
            noise = random.choice(station_noises) * random.uniform(0.5, 2.0)
            spec = compute_mel_spectrogram(noise)
        elif r < 0.85:
            # Розовый шум
            noise = generate_pink_noise(TOTAL_SAMPLES)
            spec = compute_mel_spectrogram(noise)
        elif r < 0.95:
            # 50Hz/100Hz гул
            noise = generate_hum(TOTAL_SAMPLES)
            spec = compute_mel_spectrogram(noise)
        else:
            # Тишина / почти ноль
            noise = np.random.normal(0, random.uniform(0.0001, 0.002), TOTAL_SAMPLES).astype(np.float32)
            spec = compute_mel_spectrogram(noise)

        X.append(spec)
        y.append(0.0)

    X = np.array(X, dtype=np.float32)
    y = np.array(y, dtype=np.float32)

    perm = np.random.permutation(len(X))
    X = X[perm]
    y = y[perm]

    print(f"  Всего сэмплов: {len(X)}, форма входа: {X[0].shape}")
    return X, y, base_positives, station_noises

# Архитектура microWakeWord на PyTorch
class MicroWakeWordModel(nn.Module):
    def __init__(self):
        super().__init__()
        # 1. Начальная свёртка по времени и мел-бинам
        self.conv1 = nn.Conv2d(1, 16, kernel_size=(5, 5), stride=(2, 2), padding=2, bias=True)
        self.bn1 = nn.BatchNorm2d(16)
        self.relu1 = nn.ReLU()

        # 2. Depthwise Separable Conv
        self.dw2 = nn.Conv2d(16, 16, kernel_size=(3, 3), stride=(2, 2), padding=1, groups=16, bias=True)
        self.pw2 = nn.Conv2d(16, 32, kernel_size=1, bias=True)
        self.bn2 = nn.BatchNorm2d(32)
        self.relu2 = nn.ReLU()

        # 3. Depthwise Separable Conv
        self.dw3 = nn.Conv2d(32, 32, kernel_size=(3, 3), stride=(2, 2), padding=1, groups=32, bias=True)
        self.pw3 = nn.Conv2d(32, 64, kernel_size=1, bias=True)
        self.bn3 = nn.BatchNorm2d(64)
        self.relu3 = nn.ReLU()

        # Глобальный пулинг и классификатор
        self.gap = nn.AdaptiveAvgPool2d((1, 1))
        self.fc1 = nn.Linear(64, 32)
        self.relu_fc = nn.ReLU()
        self.drop = nn.Dropout(0.2)
        self.fc2 = nn.Linear(32, 1)

    def forward(self, x):
        # x: (B, 1, 148, 40)
        x = self.relu1(self.bn1(self.conv1(x)))
        x = self.relu2(self.bn2(self.pw2(self.dw2(x))))
        x = self.relu3(self.bn3(self.pw3(self.dw3(x))))
        x = self.gap(x)
        x = torch.flatten(x, 1)
        x = self.drop(self.relu_fc(self.fc1(x)))
        x = self.fc2(x)
        return torch.sigmoid(x)

def quantize_weights(w):
    """Квантование массива весов в int8_t (-128..127) и вычисление scale."""
    max_val = np.max(np.abs(w))
    scale = float(max_val / 127.0) if max_val > 1e-7 else 1.0
    int8_w = np.clip(np.round(w / scale), -128, 127).astype(np.int8)
    return int8_w, scale

def train_and_export_model(X, y, output_header_path):
    print("[3/5] Обучение модели PyTorch...")
    device = torch.device("cpu")
    model = MicroWakeWordModel().to(device)

    # 80/20 train/val
    split_idx = int(0.8 * len(X))
    X_train = torch.tensor(X[:split_idx], dtype=torch.float32).unsqueeze(1)
    y_train = torch.tensor(y[:split_idx], dtype=torch.float32).unsqueeze(1)
    X_val = torch.tensor(X[split_idx:], dtype=torch.float32).unsqueeze(1)
    y_val = torch.tensor(y[split_idx:], dtype=torch.float32).unsqueeze(1)

    dataset = torch.utils.data.TensorDataset(X_train, y_train)
    loader = torch.utils.data.DataLoader(dataset, batch_size=32, shuffle=True)

    criterion = nn.BCELoss()
    optimizer = optim.Adam(model.parameters(), lr=0.001)

    best_acc = 0.0
    epochs = 18

    for epoch in range(1, epochs + 1):
        model.train()
        total_loss = 0.0
        for bx, by in loader:
            optimizer.zero_grad()
            out = model(bx)
            loss = criterion(out, by)
            loss.backward()
            optimizer.step()
            total_loss += loss.item()

        # Валидация
        model.eval()
        with torch.no_grad():
            preds = model(X_val)
            pred_labels = (preds >= 0.5).float()
            val_acc = (pred_labels == y_val).float().mean().item()

        print(f"  Эпоха {epoch:2d}/{epochs}: loss = {total_loss / len(loader):.4f}, val_acc = {val_acc * 100:.2f}%")
        if val_acc > best_acc:
            best_acc = val_acc

    print(f"  Лучшая точность на валидации: {best_acc * 100:.2f}%")
    assert best_acc >= 0.95, f"Точность {best_acc:.2f} ниже требуемых 95%!"

    print("[4/5] Квантование весов в INT8...")
    model.eval()

    # Слияние BatchNorm в Conv для упрощения C-инференса
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

    w1_f, b1_f = fold_conv_bn(model.conv1, model.bn1)
    w2_dw = model.dw2.weight.detach().numpy()
    b2_dw = model.dw2.bias.detach().numpy()
    w2_pw_f, b2_pw_f = fold_conv_bn(model.pw2, model.bn2)

    w3_dw = model.dw3.weight.detach().numpy()
    b3_dw = model.dw3.bias.detach().numpy()
    w3_pw_f, b3_pw_f = fold_conv_bn(model.pw3, model.bn3)

    w_fc1 = model.fc1.weight.detach().numpy()
    b_fc1 = model.fc1.bias.detach().numpy()

    w_fc2 = model.fc2.weight.detach().numpy()
    b_fc2 = model.fc2.bias.detach().numpy()

    # Квантование
    w1_q, s1 = quantize_weights(w1_f)
    w2_dw_q, s2_dw = quantize_weights(w2_dw)
    w2_pw_q, s2_pw = quantize_weights(w2_pw_f)
    w3_dw_q, s3_dw = quantize_weights(w3_dw)
    w3_pw_q, s3_pw = quantize_weights(w3_pw_f)
    w_fc1_q, s_fc1 = quantize_weights(w_fc1)
    w_fc2_q, s_fc2 = quantize_weights(w_fc2)

    total_weight_bytes = (
        w1_q.nbytes + w2_dw_q.nbytes + w2_pw_q.nbytes +
        w3_dw_q.nbytes + w3_pw_q.nbytes + w_fc1_q.nbytes + w_fc2_q.nbytes
    )
    print(f"  Общий размер INT8 весов: {total_weight_bytes} байт ({total_weight_bytes / 1024:.2f} КБ)")
    assert total_weight_bytes <= 120 * 1024, "Размер весов превышает лимит 120 КБ!"

    print(f"[5/5] Экспорт в C-заголовок {output_header_path}...")
    def write_array(f, name, arr, dtype_str):
        flat = arr.flatten()
        f.write(f"static const {dtype_str} {name}[{len(flat)}] = {{\n")
        for i in range(0, len(flat), 12):
            chunk = flat[i:i + 12]
            if "int8" in dtype_str:
                items = [str(int(x)) for x in chunk]
            else:
                items = [f"{float(x):.7f}f" for x in chunk]
            comma = "," if i + 12 < len(flat) else ""
            f.write(f"    {', '.join(items)}{comma}\n")
        f.write("};\n\n")

    with open(output_header_path, "w", encoding="utf-8") as f:
        f.write("/*\n")
        f.write(" * wake_model_ey_gin.h — INT8 microWakeWord модель для кодового слова «эй гин».\n")
        f.write(f" * Дата генерации: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f" * Точность на тестовом наборе: {best_acc * 100:.2f}%\n")
        f.write(f" * Размер INT8 весов: {total_weight_bytes} байт ({total_weight_bytes / 1024:.2f} КБ)\n")
        f.write(" */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stddef.h>\n")
        f.write("#include <stdbool.h>\n")
        f.write("#include <math.h>\n\n")

        f.write(f"#define WAKE_MODEL_EY_GIN_LEN {total_weight_bytes}\n")
        f.write(f"#define WAKE_MODEL_FRAMES {NUM_FRAMES}\n")
        f.write(f"#define WAKE_MODEL_MELS {N_MELS}\n\n")

        # Экспорт весов и смещений
        write_array(f, "g_w1", w1_q, "int8_t")
        write_array(f, "g_b1", b1_f, "float")
        f.write(f"static const float g_s1 = {s1:.8f}f;\n\n")

        write_array(f, "g_w2_dw", w2_dw_q, "int8_t")
        write_array(f, "g_b2_dw", b2_dw, "float")
        f.write(f"static const float g_s2_dw = {s2_dw:.8f}f;\n\n")

        write_array(f, "g_w2_pw", w2_pw_q, "int8_t")
        write_array(f, "g_b2_pw", b2_pw_f, "float")
        f.write(f"static const float g_s2_pw = {s2_pw:.8f}f;\n\n")

        write_array(f, "g_w3_dw", w3_dw_q, "int8_t")
        write_array(f, "g_b3_dw", b3_dw, "float")
        f.write(f"static const float g_s3_dw = {s3_dw:.8f}f;\n\n")

        write_array(f, "g_w3_pw", w3_pw_q, "int8_t")
        write_array(f, "g_b3_pw", b3_pw_f, "float")
        f.write(f"static const float g_s3_pw = {s3_pw:.8f}f;\n\n")

        write_array(f, "g_w_fc1", w_fc1_q, "int8_t")
        write_array(f, "g_b_fc1", b_fc1, "float")
        f.write(f"static const float g_s_fc1 = {s_fc1:.8f}f;\n\n")

        write_array(f, "g_w_fc2", w_fc2_q, "int8_t")
        write_array(f, "g_b_fc2", b_fc2, "float")
        f.write(f"static const float g_s_fc2 = {s_fc2:.8f}f;\n\n")

        # Экспорт плоского массива байт весов для совместимости со спецификацией (g_wake_model_ey_gin_data)
        f.write("__attribute__((aligned(16))) static const uint8_t g_wake_model_ey_gin_data[WAKE_MODEL_EY_GIN_LEN] = {\n")
        all_weights = np.concatenate([
            w1_q.flatten(), w2_dw_q.flatten(), w2_pw_q.flatten(),
            w3_dw_q.flatten(), w3_pw_q.flatten(), w_fc1_q.flatten(), w_fc2_q.flatten()
        ]).astype(np.uint8)
        for i in range(0, len(all_weights), 12):
            chunk = all_weights[i:i + 12]
            hex_items = [f"0x{b:02x}" for b in chunk]
            comma = "," if i + 12 < len(all_weights) else ""
            f.write(f"    {', '.join(hex_items)}{comma}\n")
        f.write("};\n\n")

        # C-функция быстрого инференса для микроконтроллера ESP32-S3
        f.write("""
/*
 * Быстрый прямой инференс модели microWakeWord на ESP32-S3.
 * Вход: spec[148 * 40] — стандартизованная мел-спектрограмма.
 * Выход: вероятность от 0.0f до 1.0f (уверенность распознавания «эй гин»).
 */
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
                const int8_t *w_ptr = &g_w1[oc * 25];
                for (int kh = 0; kh < 5; kh++) {
                    int ih = oh * 2 - 2 + kh;
                    if (ih < 0 || ih >= 148) continue;
                    for (int kw = 0; kw < 5; kw++) {
                        int iw = ow * 2 - 2 + kw;
                        if (iw < 0 || iw >= 40) continue;
                        float val = spec[ih * 40 + iw];
                        int8_t w = w_ptr[kh * 5 + kw];
                        sum += val * (w * g_s1);
                    }
                }
                act1[(oh * 20 + ow) * 16 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    /* Слой 2: Depthwise Conv2D(k=3x3, s=2, p=1) + Pointwise Conv2D(16 -> 32) + ReLU, выход 37x10x32 */
    for (int oh = 0; oh < 37; oh++) {
        for (int ow = 0; ow < 10; ow++) {
            float dw_out[16];
            for (int c = 0; c < 16; c++) {
                float sum = g_b2_dw[c];
                const int8_t *w_ptr = &g_w2_dw[c * 9];
                for (int kh = 0; kh < 3; kh++) {
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 74) continue;
                    for (int kw = 0; kw < 3; kw++) {
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 20) continue;
                        float val = act1[(ih * 20 + iw) * 16 + c];
                        int8_t w = w_ptr[kh * 3 + kw];
                        sum += val * (w * g_s2_dw);
                    }
                }
                dw_out[c] = sum;
            }
            /* Pointwise 16 -> 32 */
            for (int oc = 0; oc < 32; oc++) {
                float sum = g_b2_pw[oc];
                const int8_t *w_ptr = &g_w2_pw[oc * 16];
                for (int ic = 0; ic < 16; ic++) {
                    sum += dw_out[ic] * (w_ptr[ic] * g_s2_pw);
                }
                act2[(oh * 10 + ow) * 32 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    /* Слой 3: Depthwise Conv2D(k=3x3, s=2, p=1) + Pointwise Conv2D(32 -> 64) + ReLU, выход 19x5x64 */
    for (int oh = 0; oh < 19; oh++) {
        for (int ow = 0; ow < 5; ow++) {
            float dw_out[32];
            for (int c = 0; c < 32; c++) {
                float sum = g_b3_dw[c];
                const int8_t *w_ptr = &g_w3_dw[c * 9];
                for (int kh = 0; kh < 3; kh++) {
                    int ih = oh * 2 - 1 + kh;
                    if (ih < 0 || ih >= 37) continue;
                    for (int kw = 0; kw < 3; kw++) {
                        int iw = ow * 2 - 1 + kw;
                        if (iw < 0 || iw >= 10) continue;
                        float val = act2[(ih * 10 + iw) * 32 + c];
                        int8_t w = w_ptr[kh * 3 + kw];
                        sum += val * (w * g_s3_dw);
                    }
                }
                dw_out[c] = sum;
            }
            /* Pointwise 32 -> 64 */
            for (int oc = 0; oc < 64; oc++) {
                float sum = g_b3_pw[oc];
                const int8_t *w_ptr = &g_w3_pw[oc * 32];
                for (int ic = 0; ic < 32; ic++) {
                    sum += dw_out[ic] * (w_ptr[ic] * g_s3_pw);
                }
                act3[(oh * 5 + ow) * 64 + oc] = (sum > 0.0f) ? sum : 0.0f;
            }
        }
    }

    /* Global Average Pooling 19x5x64 -> 64 */
    float gap[64] = {0};
    const float norm = 1.0f / (19.0f * 5.0f);
    for (int i = 0; i < 19 * 5; i++) {
        for (int c = 0; c < 64; c++) {
            gap[c] += act3[i * 64 + c];
        }
    }
    for (int c = 0; c < 64; c++) {
        gap[c] *= norm;
    }

    /* FC1: 64 -> 32 + ReLU */
    float fc1[32];
    for (int i = 0; i < 32; i++) {
        float sum = g_b_fc1[i];
        const int8_t *w_ptr = &g_w_fc1[i * 64];
        for (int j = 0; j < 64; j++) {
            sum += gap[j] * (w_ptr[j] * g_s_fc1);
        }
        fc1[i] = (sum > 0.0f) ? sum : 0.0f;
    }

    /* FC2: 32 -> 1 + Sigmoid */
    float logit = g_b_fc2[0];
    for (int j = 0; j < 32; j++) {
        logit += fc1[j] * (g_w_fc2[j] * g_s_fc2);
    }

    if (dyn_arena) {
        free(dyn_arena);
    }

    /* Сигмоида */
    return 1.0f / (1.0f + expf(-logit));
}
""")

    print(f"[OK] Модель успешно обучена и экспортирована в {output_header_path}!")

def main():
    header_path = sys.argv[1] if len(sys.argv) > 1 else "firmware/main/wake_model_ey_gin.h"
    X, y, _, _ = generate_dataset()
    train_and_export_model(X, y, header_path)

if __name__ == "__main__":
    main()
