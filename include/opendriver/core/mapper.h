#pragma once

#include <opendriver/core/device_registry.h>
#include <openvr_driver.h>
#include <vector>

namespace opendriver::core {

class Mapper {
public:
    /// Mapuje wewnętrzny DeviceType na OpenVR TrackedDeviceClass
    static vr::ETrackedDeviceClass MapDeviceClass(DeviceType type) {
        switch (type) {
            case DeviceType::HMD:             return vr::TrackedDeviceClass_HMD;
            case DeviceType::HAND_TRACKER:    return vr::TrackedDeviceClass_Controller; // Map to controller for now
            case DeviceType::GENERIC_TRACKER: return vr::TrackedDeviceClass_GenericTracker;
            case DeviceType::LIGHTHOUSE:      return vr::TrackedDeviceClass_TrackingReference;
            case DeviceType::CONTROLLER:      return vr::TrackedDeviceClass_Controller;
            default:                          return vr::TrackedDeviceClass_Invalid;
        }
    }

    /// Mapuje właściwości urządzenia na kontenery OpenVR
    static void MapProperties(const Device& src, vr::PropertyContainerHandle_t container) {
        auto* props = vr::VRProperties();
        
        props->SetStringProperty(container, vr::Prop_ModelNumber_String, src.name.c_str());
        props->SetStringProperty(container, vr::Prop_SerialNumber_String, src.serial_number.c_str());
        props->SetStringProperty(container, vr::Prop_ManufacturerName_String, src.manufacturer.c_str());
        props->SetUint64Property(container, vr::Prop_HardwareRevision_Uint64, 1);
        props->SetUint64Property(container, vr::Prop_FirmwareVersion_Uint64, 1);
        
        props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
        props->SetFloatProperty(container, vr::Prop_DeviceBatteryPercentage_Float, src.battery_percent / 100.0f);
        
        if (src.type == DeviceType::HMD) {
            props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, src.display.refresh_rate);
            props->SetBoolProperty(container, vr::Prop_ReportsTimeSinceVSync_Bool, false);
        }
    }

    /// Tworzy domyślny pose dla OpenVR
    static vr::DriverPose_t CreateDefaultPose() {
        vr::DriverPose_t pose = { 0 };
        pose.poseIsValid = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.deviceIsConnected = true;
        
        pose.qWorldFromDriverRotation = { 1.0, 0.0, 0.0, 0.0 };
        pose.qDriverFromHeadRotation = { 1.0, 0.0, 0.0, 0.0 };
        pose.qRotation = { 1.0, 0.0, 0.0, 0.0 };
        
        return pose;
    }
};

} // namespace opendriver::core
