# OpenDriverVR

https://img.shields.io/github/stars/rozgaleziacz/OpenDriverVR?style=for-the-badge

OpenDriverVR is an open-source SteamVR driver that emulates a VR headset (HMD) and enables custom tracking solutions such as Kinect, webcam, or other devices.

## 🚀 Features

- 🧠 Custom HMD emulation for SteamVR
- 🎯 6DOF tracking (position + rotation)
- 📷 Webcam-based tracking support
- 🕺 Kinect tracking support
- 🔌 Modular tracking input system (easy to extend)
- 🌐 UDP / network data support (for external apps)
- 🧩 Works with custom VR setups (DIY headsets, phone VR, etc.)

## 🧪 Current Status

⚠️ Work in progress

Core systems are under development:
- HMD driver ✔️
- Tracking input (basic) ✔️
- Rendering pipeline ❌ (planned)
- Full SteamVR integration ⚠️

## 🛠️ How It Works

1. The driver emulates a VR headset inside SteamVR
2. Tracking data is received from external sources (Python, Kinect, webcam, etc.)
3. Data is injected into SteamVR as real HMD movement
4. (Planned) Video stream is sent to external display (phone / custom headset)

## 🔌 Tracking Sources

Supported / planned tracking inputs:

- Kinect (body + head tracking)
- Webcam (color / AI tracking)
- Custom UDP input (Python, OpenCV, etc.)
- IMU / DIY sensors (planned)

## 🖥️ Platforms

- ✅ Windows (primary target)
- 🐧 Linux (planned)

## 📦 Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/yourname/OpenDriverVR.git
   ```

2. Build the driver (Visual Studio recommended)

3. Copy the driver folder to:
   ```
   SteamVR/drivers/
   ```

4. Enable the driver in SteamVR settings

## 🧠 Example Use Case

- DIY VR headset using a phone as display
- Kinect full-body tracking
- Webcam-based head tracking
- Custom VR experiments

## 🤝 Contributing

Contributions are welcome!

Ideas:
- Improve tracking accuracy
- Add new input sources
- Optimize latency
- Linux support

## 📜 License

MIT License

## 💡 Vision

OpenDriverVR aims to become a flexible, open platform for experimental VR setups — from cheap DIY builds to advanced custom tracking systems.

---

Made with ☕ and a bit of madness