#pragma once
#include <Arduino.h>

// 体积用空实现：产品不需要 mDNS，ArduinoOTA 仅因 #include 而引用本符号。
// ArduinoOTA.setMdnsEnabled(false) 后 begin/end 也不会被调用。
class MDNSResponder {
 public:
  bool begin(const char* /*hostname*/) { return false; }
  void end() {}
  void enableArduino(uint16_t /*port*/, bool /*auth*/) {}
};

extern MDNSResponder MDNS;
