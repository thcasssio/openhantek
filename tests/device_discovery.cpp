// SPDX-License-Identifier: GPL-2.0+
// Production discovery/model code with fake libusb and firmware upload.
// The offscreen combo box checks the same model notifications used by selection.
#include <QApplication>
#include <QComboBox>
#include <QPersistentModelIndex>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "hantekdso/dsomodel.h"
#include "selectdevice/devicelistentry.h"
#include "selectdevice/deviceslistmodel.h"
#include "usb/finddevices.h"
#include "usb/uploadFirmware.h"

struct FakeDevice;
struct libusb_device { FakeDevice *fixture; };
struct libusb_device_handle { FakeDevice *fixture; };
struct libusb_context { std::vector<libusb_device *> devices; };

namespace {
void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class SupportedModel : public DSOModel {
  public:
    SupportedModel()
        : DSOModel(0x2250, 0x04b5, 0x2250, 0x04b4, 0x2250, "test", "DSO-2250",
                   Dso::ControlSpecification(2)) {}
    void applyRequirements(HantekDsoControl *) const override {}
};

int unexpectedTransfers = 0;
}

struct FakeDevice {
    libusb_device raw{this};
    libusb_device_handle handle{this};
    uint16_t vendor = 0x04b5;
    uint16_t product = 0x2250;
    uint8_t bus = 2;
    uint8_t address = 1;
    std::vector<uint8_t> ports{1};
    int topologyError = 0;
    int descriptorError = 0;
    int openError = 0;
    int references = 1;
    int opened = 0;
    int claimed = 0;
    int uploads = 0;
    bool uploadSucceeds = true;
    libusb_endpoint_descriptor endpoints[2]{};
    libusb_interface_descriptor alternate{};
    libusb_interface interfaceDescriptor{};
    libusb_config_descriptor configuration{};

    FakeDevice() {
        endpoints[0].bEndpointAddress = 0x02;
        endpoints[1].bEndpointAddress = 0x86;
        for (auto &endpoint : endpoints) {
            endpoint.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
            endpoint.wMaxPacketSize = 512;
        }
        alternate.bInterfaceClass = LIBUSB_CLASS_VENDOR_SPEC;
        alternate.bNumEndpoints = 2;
        alternate.endpoint = endpoints;
        interfaceDescriptor.altsetting = &alternate;
        interfaceDescriptor.num_altsetting = 1;
        configuration.bNumInterfaces = 1;
        configuration.interface = &interfaceDescriptor;
    }
};

extern "C" {
libusb_device *LIBUSB_CALL libusb_ref_device(libusb_device *device) {
    ++device->fixture->references;
    return device;
}
void LIBUSB_CALL libusb_unref_device(libusb_device *device) { --device->fixture->references; }
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *context, libusb_device ***list) {
    *list = new libusb_device *[context->devices.size() + 1]();
    for (size_t i = 0; i < context->devices.size(); ++i) (*list)[i] = libusb_ref_device(context->devices[i]);
    return static_cast<ssize_t>(context->devices.size());
}
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref) {
    if (unref) for (size_t i = 0; list[i]; ++i) libusb_unref_device(list[i]);
    delete[] list;
}
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *device, uint8_t *ports, int length) {
    const auto &fixture = *device->fixture;
    if (fixture.topologyError) {
        std::memset(ports, 0xff, length); // Error output must not become a valid path.
        return fixture.topologyError;
    }
    if (fixture.ports.size() > static_cast<size_t>(length)) return LIBUSB_ERROR_OVERFLOW;
    std::copy(fixture.ports.begin(), fixture.ports.end(), ports);
    return static_cast<int>(fixture.ports.size());
}
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device) { return device->fixture->bus; }
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device) { return device->fixture->address; }
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device, libusb_device_descriptor *descriptor) {
    const auto &fixture = *device->fixture;
    if (fixture.descriptorError) return fixture.descriptorError;
    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->idVendor = fixture.vendor;
    descriptor->idProduct = fixture.product;
    return LIBUSB_SUCCESS;
}
int LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle) {
    auto &fixture = *device->fixture;
    if (fixture.openError) return fixture.openError;
    *handle = &fixture.handle;
    ++fixture.opened;
    ++fixture.references;
    return LIBUSB_SUCCESS;
}
void LIBUSB_CALL libusb_close(libusb_device_handle *handle) {
    --handle->fixture->opened;
    --handle->fixture->references;
}
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *device, uint8_t, libusb_config_descriptor **descriptor) {
    *descriptor = &device->fixture->configuration;
    return LIBUSB_SUCCESS;
}
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor *) {}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int) {
    ++handle->fixture->claimed;
    return LIBUSB_SUCCESS;
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int) {
    --handle->fixture->claimed;
    return LIBUSB_SUCCESS;
}
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *, uint8_t, uint8_t, uint16_t, uint16_t,
                                       unsigned char *, uint16_t, unsigned int) {
    ++unexpectedTransfers;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int) {
    ++unexpectedTransfers;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
}

bool UploadFirmware::startUpload(USBDevice *device) {
    auto &fixture = *device->getRawDevice()->fixture;
    check(device->needsFirmware(), "Discovery attempted to upload an already warm device");
    ++fixture.uploads;
    errorMessage = fixture.uploadSucceeds ? QString() : QStringLiteral("Fake firmware upload failed");
    return fixture.uploadSucceeds;
}
const QString &UploadFirmware::getErrorMessage() const { return errorMessage; }

namespace {
void coldToWarmAndView() {
    FakeDevice cold, warm;
    cold.vendor = 0x04b4;
    cold.address = 4;
    warm.address = 5;
    libusb_context context{{&cold.raw}};
    {
        FindDevices finder(&context);
        DevicesListModel model(&finder);
        std::vector<bool> readyNotifications;
        QComboBox combo;
        combo.setModel(&model);
        QObject::connect(&combo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                         [&]() { readyNotifications.push_back(combo.currentData(Qt::UserRole + 1).toBool()); });
        int resets = 0, inserted = 0;
        QObject::connect(&model, &QAbstractItemModel::modelReset, [&]() { ++resets; });
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&]() { ++inserted; });
        check(finder.updateDeviceList() == 1, "Initial cold device was not discovered");
        model.updateDeviceList();
        const UniqueUSBid id = combo.currentData(Qt::UserRole).value<UniqueUSBid>();
        check(combo.currentIndex() == 0 && !combo.currentData(Qt::UserRole + 1).toBool() &&
                  combo.currentData(Qt::UserRole + 2).toBool() && cold.uploads == 1,
              "Cold device did not remain unready while awaiting re-enumeration");
        QPersistentModelIndex coldIndex(model.index(0, 0));

        // No empty enumeration separates the bootloader and application devices.
        context.devices = {&warm.raw};
        check(finder.updateDeviceList() == 1, "Cold-to-warm replacement at the same port was not reported");
        const auto &replacement = finder.getDevices()->at(id);
        check(replacement->getRawDevice() == &warm.raw && !replacement->needsFirmware() && cold.references == 1,
              "Cold descriptor/raw device survived warm re-enumeration");
        model.updateDeviceList();
        check(!coldIndex.isValid() && resets == 2 && inserted == 0 && model.rowCount(QModelIndex()) == 1,
              "Discovery refresh did not issue a consistent model reset");
        check(combo.currentIndex() == 0 && combo.currentData(Qt::UserRole).value<UniqueUSBid>() == id &&
                  combo.currentData(Qt::UserRole + 1).toBool() && !combo.currentData(Qt::UserRole + 2).toBool() &&
                  !readyNotifications.empty() && readyNotifications.back(),
              "Combo box did not observe the warm device becoming ready at row zero");
        USBDevice *same = replacement.get();
        check(finder.updateDeviceList() == 0 && finder.getDevices()->at(id).get() == same && cold.uploads == 1,
              "Repeated enumeration of the same raw device recreated or re-uploaded it");
    }
    check(cold.references == 1 && warm.references == 1 && warm.opened == 0 && warm.claimed == 0,
          "Re-enumeration or the readiness probe leaked USB references/handles");
}

void topologyIdentity() {
    static_assert(sizeof(UniqueUSBid) == 8, "Device IDs must retain all seven ports and the bus on every platform");
    FakeDevice first, second;
    second.bus = 3;
    check(USBDevice::computeUSBdeviceID(&first.raw) != USBDevice::computeUSBdeviceID(&second.raw),
          "Equal port paths on different buses collided");
    first.bus = 200;
    first.ports = {1, 2, 3, 4, 5, 6, 7};
    check(USBDevice::computeUSBdeviceID(&first.raw) == (UniqueUSBid(200) << 56 | UniqueUSBid(0x07060504030201)),
          "Seven-level port path or bus was truncated or depended on byte order");
    first.ports.clear();
    const UniqueUSBid root = USBDevice::computeUSBdeviceID(&first.raw);
    check((root & 0xff) == 0 && (root >> 56) == first.bus, "Root fallback lost its bus or used a real port prefix");
    first.ports = {1};
    first.topologyError = LIBUSB_ERROR_NOT_SUPPORTED;
    first.address = 7;
    const UniqueUSBid fallback = USBDevice::computeUSBdeviceID(&first.raw);
    first.address = 8;
    check(fallback != USBDevice::computeUSBdeviceID(&first.raw) && (fallback & 0xff) == 0 &&
                  (fallback >> 56) == first.bus && fallback != root,
          "Missing topology did not use a distinct bus/address fallback");
    first.topologyError = 0;
    first.ports = {1, 2, 3, 4, 5, 6, 7, 8};
    check((USBDevice::computeUSBdeviceID(&first.raw) & 0xff) == 0,
          "Overflowing topology was mistaken for a valid port path");
}

void unrelatedReplacementAndBuses() {
    FakeDevice scope, other;
    other.vendor = 0x1234;
    other.address = 2;
    libusb_context context{{&scope.raw}};
    {
        FindDevices finder(&context);
        check(finder.updateDeviceList() == 1, "Replacement fixture did not find the scope");
        context.devices = {&other.raw};
        check(finder.updateDeviceList() == 1 && finder.getDevices()->empty(),
              "Unrelated replacement at the same port preserved a stale scope");
    }
    other.bus = 1;
    context.devices = {&scope.raw};
    {
        FindDevices finder(&context);
        finder.updateDeviceList();
        context.devices = {&other.raw};
        check(finder.updateDeviceList() == 1 && finder.getDevices()->empty(),
              "Unrelated device on another bus kept a disconnected scope alive");
    }
    other.vendor = 0x04b5;
    context.devices = {&scope.raw, &other.raw};
    {
        FindDevices finder(&context);
        check(finder.updateDeviceList() == 2 && finder.getDevices()->size() == 2,
              "Scopes on equal port paths of two buses were merged");
        const auto otherId = USBDevice::computeUSBdeviceID(&other.raw);
        USBDevice *survivor = finder.getDevices()->at(otherId).get();
        context.devices = {&other.raw};
        check(finder.updateDeviceList() == 1 && finder.getDevices()->size() == 1 &&
                  finder.getDevices()->at(otherId).get() == survivor,
              "Removing one bus disturbed the device on another bus");
        other.descriptorError = LIBUSB_ERROR_IO;
        check(finder.updateDeviceList() == 1 && finder.getDevices()->empty(),
              "Failed descriptor read retained an unverified cached device");
    }
    check(scope.references == 1 && other.references == 1, "Replacement/removal leaked a raw device reference");
}

void failedUploadAndEmptyModel() {
    DeviceListEntry defaults;
    check(!defaults.canConnect && !defaults.needFirmware, "Device status flags do not start false");
    FakeDevice cold, warm;
    cold.vendor = 0x04b4;
    cold.uploadSucceeds = false;
    warm.openError = LIBUSB_ERROR_ACCESS;
    libusb_context context{{&cold.raw}};
    FindDevices finder(&context);
    DevicesListModel model(&finder);
    QComboBox combo;
    combo.setModel(&model);
    finder.updateDeviceList();
    model.updateDeviceList();
    check(!combo.currentData(Qt::UserRole + 1).toBool() && combo.currentData(Qt::UserRole + 2).toBool() &&
                  combo.currentData(Qt::UserRole + 3).toString() == "Fake firmware upload failed",
          "Firmware failure was hidden behind an upload/ready flag");
    context.devices = {&warm.raw};
    finder.updateDeviceList();
    model.updateDeviceList();
    check(!combo.currentData(Qt::UserRole + 1).toBool() && !combo.currentData(Qt::UserRole + 2).toBool() &&
                  !combo.currentData(Qt::UserRole + 3).toString().isEmpty(),
          "Failed warm probe falsely advertised firmware upload or hid its error");
    context.devices.clear();
    finder.updateDeviceList();
    model.updateDeviceList();
    check(model.rowCount(QModelIndex()) == 0 && combo.currentIndex() == -1,
          "Empty discovery refresh left a selected/stale model row");
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    SupportedModel supported;
    try {
        coldToWarmAndView();
        topologyIdentity();
        unrelatedReplacementAndBuses();
        failedUploadAndEmptyModel();
        check(unexpectedTransfers == 0, "Discovery fixture unexpectedly sent a bulk/control transfer");
        std::cout << "Offline device discovery, re-enumeration and Qt selection model tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
