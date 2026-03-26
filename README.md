🚀 OpenDriver VR

"License" (https://img.shields.io/badge/license-MIT-green)
"Platform" (https://img.shields.io/badge/platform-Windows-blue)
"Linux" (https://img.shields.io/badge/Linux-planned-yellow)
"Status" (https://img.shields.io/badge/status-WIP-orange)

An open-source, modular SteamVR driver that emulates a virtual HMD with full 6DOF tracking and support for Kinect, webcam tracking, and custom sensors.

---

📚 Table of Contents

- "Overview" (#-overview)
- "Features" (#-features)
- "Architecture" (#-architecture)
- "6DOF Tracking" (#-6dof-tracking)
- "Installation" (#-installation-windows)
- "Linux Support" (#-linux-support)
- "Development" (#-development)
- "Roadmap" (#-roadmap)
- "Contributing" (#-contributing)
- "License" (#-license)

---

🚀 Overview

OpenDriverVR lets you build your own VR system using external tracking and streaming solutions.

[SteamVR / OpenVR]
   ↓
[OpenDriverVR Driver]  ← HMD + 6DOF tracking
   ↓
[Tracking Input Layer]
   ↓
[Kinect / Webcam / Sensors]

[External App] ← video capture & streaming
   ↓
[Client Device (Phone VR / PC VR)]

---

✨ Features

- 🧠 Full 6DOF tracking (position + rotation)
- 📷 Kinect tracking support
- 🎥 Webcam tracking (OpenCV / AI-ready)
- 📡 UDP / IPC tracking input
- 🕶️ Virtual HMD for SteamVR
- ⚡ Modular architecture
- 🌍 Fully open-source
- 🐧 Planned Linux support

---

🧱 Architecture

🔹 Driver (C++)

Handles:

- HMD emulation
- Pose updates (6DOF)
- Receiving tracking data

🔹 Tracking Systems

- Kinect (body/head tracking)
- Webcam (OpenCV / AI / markers)
- Custom sensors (IMU, phone gyro)

🔹 External App

- Frame capture (DXGI / future Linux APIs)
- Encoding (JPEG / H264 / GPU acceleration)
- Streaming via UDP

---

🎯 6DOF Tracking

Supports full spatial movement:

- Rotation → Pitch / Yaw / Roll
- Position → X / Y / Z

Input sources:

- Kinect skeleton tracking
- Webcam pose estimation
- Custom pipelines

---

📦 Installation (Windows)

1. Build the driver (x64)

2. Copy DLL:

SteamVR/drivers/OpenDriverVR/bin/win64/driver_OpenDriverVR.dll

3. Folder structure:

SteamVR/
 └── drivers/
     └── OpenDriverVR/
         ├── bin/win64/
         └── resources/

4. Restart SteamVR

---

🐧 Linux Support

Planned features:

- OpenVR on Linux
- SteamVR Linux runtime support
- Wayland / X11 capture
- VAAPI encoding

---

⚠️ Requirements

- Windows (x64) (Linux planned)
- SteamVR / OpenVR
- Visual Studio

Optional:

- Kinect SDK
- Python / OpenCV
- FFmpeg

---

🔧 Development

Key Components

- "HmdDriverFactory" → entry point
- "IServerTrackedDeviceProvider" → driver core
- "ITrackedDeviceServerDriver" → HMD

Notes

- Must be 64-bit DLL
- Must export "HmdDriverFactory"
- Wrong interface = driver won’t load

---

🧪 Status

- ✅ Driver loads
- ✅ OpenVR detected
- ⚠️ Fake HMD (WIP)
- ⚠️ 6DOF tracking (WIP)
- ⚠️ Linux support (planned)

---

🔮 Roadmap

- [ ] Virtual HMD fully working
- [ ] Stable 6DOF tracking
- [ ] Kinect integration
- [ ] Webcam AI tracking
- [ ] Low latency streaming (H264)
- [ ] Linux support
- [ ] Mobile VR client

---

🤝 Contributing

Pull requests are welcome.

Ideas, experiments, and improvements are encouraged.

---

⚠️ Disclaimer

This project is experimental.

It may:

- crash SteamVR
- behave unpredictably
- require debugging

Use at your own risk.

---

📜 License

MIT License

---

OpenDriverVR — DIY VR without limits.
