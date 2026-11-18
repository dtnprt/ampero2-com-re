#include "UsbMidiHost.h"

#include <usb/usb_host.h>

#define LOGF(...) Serial0.printf(__VA_ARGS__)

static constexpr size_t kXferSize = 512;          // >= max packet size (FS: 64)
static constexpr uint32_t kPowerOnDelayMs = 1500; // give the device time to boot before the first reset
static constexpr uint32_t kRetryMs = 4000;        // no device enumerated for this long -> power-cycle the root port

bool UsbMidiHost::begin() {
  rx_ = xStreamBufferCreate(4096, 1);
  outSem_ = xSemaphoreCreateBinary();
  outLock_ = xSemaphoreCreateMutex();

  usb_host_config_t host = {};
  host.skip_phy_setup = false;
  host.root_port_unpowered = true;   // powered in libTask after kPowerOnDelayMs
  host.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&host) != ESP_OK) return false;
  xTaskCreatePinnedToCore(libTask, "usb_lib", 4096, this, 5, nullptr, 0);

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = (usb_host_client_event_cb_t)clientEvent;
  cfg.async.callback_arg = this;
  if (usb_host_client_register(&cfg, (usb_host_client_handle_t*)&client_) != ESP_OK) return false;
  xTaskCreatePinnedToCore(clientTask, "usb_client", 6144, this, 5, nullptr, 0);
  return true;
}

/*
 * Runs the USB host library. ESP-IDF does not retry after "Root port reset failed" (it waits for a
 * new connection event that never comes), so the root port is power-cycled while no device is
 * enumerated. The ESP32-S3 has no VBUS switch, "power" only means the port logic; it still re-runs
 * connection detection and the bus reset.
 */
void UsbMidiHost::libTask(void* arg) {
  auto* self = static_cast<UsbMidiHost*>(arg);
  vTaskDelay(pdMS_TO_TICKS(kPowerOnDelayMs));
  usb_host_lib_set_root_port_power(true);
  uint32_t idleSince = millis(), attempts = 0;
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();

    usb_host_lib_info_t info = {};
    usb_host_lib_info(&info);
    if (info.num_devices > 0 || self->device_) {
      idleSince = millis();
      attempts = 0;
      continue;
    }
    if (millis() - idleSince >= kRetryMs) {
      esp_err_t off = usb_host_lib_set_root_port_power(false);
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_err_t on = usb_host_lib_set_root_port_power(true);
      if (++attempts <= 3 || attempts % 15 == 0)
        LOGF("USB: kein Gerät enumeriert, Root-Port neu gestartet (Versuch %lu, off=%d on=%d)\r\n",
             (unsigned long)attempts, off, on);
      idleSince = millis();
    }
  }
}

void UsbMidiHost::clientEvent(const void* m, void* arg) {
  auto* self = static_cast<UsbMidiHost*>(arg);
  auto* msg = static_cast<const usb_host_client_event_msg_t*>(m);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    self->pendingOpen_ = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (msg->dev_gone.dev_hdl == self->device_) self->pendingClose_ = true;
  }
}

void UsbMidiHost::clientTask(void* arg) {
  auto* self = static_cast<UsbMidiHost*>(arg);
  for (;;) {
    usb_host_client_handle_events((usb_host_client_handle_t)self->client_, pdMS_TO_TICKS(50));
    if (self->pendingClose_) {
      self->pendingClose_ = false;
      self->closeDevice();
    }
    if (self->pendingOpen_ && !self->device_) {
      uint8_t addr = self->pendingOpen_;
      self->pendingOpen_ = 0;
      self->openDevice(addr);
    }
  }
}

void UsbMidiHost::openDevice(uint8_t address) {
  auto client = (usb_host_client_handle_t)client_;
  usb_device_handle_t dev;
  if (usb_host_device_open(client, address, &dev) != ESP_OK) return;

  const usb_device_desc_t* dd;
  const usb_config_desc_t* cd;
  usb_host_get_device_descriptor(dev, &dd);
  usb_host_get_active_config_descriptor(dev, &cd);
  LOGF("USB: Gerät %04x:%04x, Konfiguration %u Byte\r\n", dd->idVendor, dd->idProduct, cd->wTotalLength);

  // walk the configuration descriptor: MIDI streaming interface (alt 0) with bulk IN/OUT
  const uint8_t* p = (const uint8_t*)cd;
  int iface = -1, curIf = -1, curAlt = -1;
  bool curIsMidi = false;
  uint8_t epIn = 0, epOut = 0;
  uint16_t mpsIn = 64, mpsOut = 64;
  for (int o = 0; o + 2 <= cd->wTotalLength && p[o] >= 2; o += p[o]) {
    const uint8_t* d = p + o;
    if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      curIf = d[2];
      curAlt = d[3];
      curIsMidi = d[5] == 0x01 && d[6] == 0x03 && curAlt == 0;
    } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curIsMidi && (d[3] & 0x03) == 0x02) {
      uint16_t mps = d[4] | (d[5] << 8);
      if (d[2] & 0x80) { epIn = d[2]; mpsIn = mps; }
      else { epOut = d[2]; mpsOut = mps; }
      if (epIn && epOut && iface < 0) iface = curIf;
    }
  }
  if (iface < 0 || usb_host_interface_claim(client, dev, iface, 0) != ESP_OK) {
    LOGF("USB: kein MIDI-Interface gefunden\r\n");
    usb_host_device_close(client, dev);
    return;
  }

  usb_transfer_t* in;
  usb_transfer_t* out;
  usb_host_transfer_alloc(kXferSize, 0, &in);
  usb_host_transfer_alloc(kXferSize, 0, &out);
  in->device_handle = dev;
  in->bEndpointAddress = epIn;
  in->callback = (usb_transfer_cb_t)inDone;
  in->context = this;
  in->num_bytes = (mpsIn <= kXferSize) ? mpsIn : kXferSize;
  out->device_handle = dev;
  out->bEndpointAddress = epOut;
  out->callback = (usb_transfer_cb_t)outDone;
  out->context = this;

  device_ = dev;
  inXfer_ = in;
  outXfer_ = out;
  interface_ = iface;
  epIn_ = epIn;
  epOut_ = epOut;
  mpsIn_ = mpsIn;
  mpsOut_ = mpsOut;
  vid_ = dd->idVendor;
  pid_ = dd->idProduct;
  sxActive_ = false;
  sxLen_ = 0;
  xStreamBufferReset((StreamBufferHandle_t)rx_);
  connected_ = true;
  epoch_ = epoch_ + 1;
  LOGF("USB: MIDI-Interface %d, IN 0x%02x (%u), OUT 0x%02x (%u)\r\n", iface, epIn, mpsIn, epOut, mpsOut);
  usb_host_transfer_submit(in);
}

void UsbMidiHost::closeDevice() {
  if (!device_) return;
  connected_ = false;
  epoch_ = epoch_ + 1;
  auto client = (usb_host_client_handle_t)client_;
  auto dev = (usb_device_handle_t)device_;
  // pending transfers complete with status NO_DEVICE; give the client task a moment to process them
  for (int i = 0; i < 10; i++) usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  usb_host_interface_release(client, dev, interface_);
  usb_host_transfer_free((usb_transfer_t*)inXfer_);
  usb_host_transfer_free((usb_transfer_t*)outXfer_);
  usb_host_device_close(client, dev);
  device_ = inXfer_ = outXfer_ = nullptr;
  xSemaphoreGive(outSem_);   // release a sender that might be waiting
  LOGF("USB: Gerät getrennt\r\n");
}

void UsbMidiHost::inDone(void* t) {
  auto* x = static_cast<usb_transfer_t*>(t);
  auto* self = static_cast<UsbMidiHost*>(x->context);
  if (x->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (x->actual_num_bytes) xStreamBufferSend(self->rx_, x->data_buffer, x->actual_num_bytes, 0);
  } else if (x->status == USB_TRANSFER_STATUS_NO_DEVICE || x->status == USB_TRANSFER_STATUS_CANCELED) {
    return;
  }
  if (self->connected_) usb_host_transfer_submit(x);
}

void UsbMidiHost::outDone(void* t) {
  auto* x = static_cast<usb_transfer_t*>(t);
  xSemaphoreGive(static_cast<UsbMidiHost*>(x->context)->outSem_);
}

bool UsbMidiHost::sendSysEx(const uint8_t* msg, size_t len) {
  if (!connected_ || len < 2) return false;
  xSemaphoreTake(outLock_, portMAX_DELAY);
  auto* out = static_cast<usb_transfer_t*>(outXfer_);
  bool ok = out != nullptr;
  size_t i = 0;
  while (ok && i < len) {
    // pack USB-MIDI event packets (cable 0) into one transfer of up to mpsOut bytes
    size_t n = 0;
    while (i < len && n + 4 <= mpsOut_ && n + 4 <= kXferSize) {
      size_t rest = len - i;
      uint8_t* pk = out->data_buffer + n;
      if (rest > 3) {
        pk[0] = 0x04; pk[1] = msg[i]; pk[2] = msg[i + 1]; pk[3] = msg[i + 2];
        i += 3;
      } else {
        pk[0] = (uint8_t)(0x04 + rest);   // 5/6/7 = SysEx ends with 1/2/3 bytes
        pk[1] = msg[i];
        pk[2] = rest > 1 ? msg[i + 1] : 0;
        pk[3] = rest > 2 ? msg[i + 2] : 0;
        i += rest;
      }
      n += 4;
    }
    out->num_bytes = n;
    xSemaphoreTake(outSem_, 0);
    ok = connected_ && usb_host_transfer_submit(out) == ESP_OK &&
         xSemaphoreTake(outSem_, pdMS_TO_TICKS(500)) == pdTRUE && connected_ &&
         out->status == USB_TRANSFER_STATUS_COMPLETED;
  }
  xSemaphoreGive(outLock_);
  return ok;
}

size_t UsbMidiHost::readSysEx(uint8_t* buf, size_t cap) {
  uint8_t pk[4];
  static const uint8_t kBytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
  while (xStreamBufferBytesAvailable(rx_) >= 4 && xStreamBufferReceive(rx_, pk, 4, 0) == 4) {
    const uint8_t cin = pk[0] & 0x0F;
    if (cin < 4 || cin > 7) continue;              // only SysEx packets are of interest
    for (int k = 1; k <= kBytes[cin]; k++) {
      const uint8_t b = pk[k];
      if (b == 0xF0) { sxActive_ = true; sxLen_ = 0; }
      if (!sxActive_) continue;
      if (sxLen_ < sizeof sx_) sx_[sxLen_++] = b;
      else sxActive_ = false;                      // too long -> drop
      if (b == 0xF7) {
        sxActive_ = false;
        if (sxLen_ > cap) return 0;
        memcpy(buf, sx_, sxLen_);
        return sxLen_;
      }
    }
  }
  return 0;
}
