# Mainline Qualcomm Kona (SM8250) Radio Interface Layer (RIL)

This repository provides the mainline Linux Radio Interface Layer (RIL), QMI modem daemon (`kebab-rild`), IMS voice daemon, and cellular data connection daemons for Qualcomm Snapdragon 865 (SM8250 / Kona) platforms paired with Snapdragon X55 5G modems.

## Target Path in Android Tree
```
hardware/mainline/kona-ril
```

## Subsystems
- **kebab-rild/**: Open-source QMI RIL implementation communicating with SDX55 over QRTR / rmnet
- **dataconn/**: Mainline data-session helper (`kebab-dataconn`) for external SDX55 modem over rmnet / QMAP
- **voicecall/**: Voice and audio call routing helper (`kebab-voicecall`)
- **guest/hals/ril/**: Reference libril AIDL interface implementation
- **guest/hals/rild/**: Hardware RIL daemon (`libcuttlefish-rild`) and VINTF fragment (`radio-kebab.xml`)
