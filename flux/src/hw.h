// hw.h — Flux 硬件抽象层（Platform Abstraction Layer）
// 提供 GPIO, UART, SPI, I2C 等硬件接口的抽象
#pragma once
#include "interpreter.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// Platform 检测与注册
// ═══════════════════════════════════════════════════════════
enum class Platform {
    Generic,    // 通用 Linux/macOS
    RaspberryPi,
    Arduino,
    ESP32,
    STM32,
    RISCV,
};

// ── GPIO 引脚模式 ─────────────────────────────────────────
enum class PinMode {
    Input,
    Output,
    InputPullup,
    InputPulldown,
    PWM,
};

// ── GPIO 引脚状态 ─────────────────────────────────────────
enum class PinState {
    Low = 0,
    High = 1,
};

// ═══════════════════════════════════════════════════════════
// HardwareDriver — 硬件驱动抽象基类
// ═══════════════════════════════════════════════════════════
class HardwareDriver {
public:
    virtual ~HardwareDriver() = default;

    // GPIO
    virtual bool pinMode(int pin, PinMode mode) = 0;
    virtual bool digitalWrite(int pin, PinState state) = 0;
    virtual PinState digitalRead(int pin) = 0;
    virtual int analogRead(int pin) = 0;
    virtual bool analogWrite(int pin, int value) = 0;

    // UART
    virtual int uartOpen(const std::string& port, int baud) = 0;
    virtual bool uartWrite(int fd, const std::string& data) = 0;
    virtual std::string uartRead(int fd, int bytes) = 0;
    virtual void uartClose(int fd) = 0;

    // Platform info
    virtual Platform platform() const = 0;
    virtual std::string platformName() const = 0;
};

// ═══════════════════════════════════════════════════════════
// GenericDriver — 模拟/桩硬件驱动（通用平台）
// ═══════════════════════════════════════════════════════════
class GenericDriver : public HardwareDriver {
public:
    bool pinMode(int pin, PinMode mode) override {
        pinModes_[pin] = mode;
        return true;
    }

    bool digitalWrite(int pin, PinState state) override {
        pinStates_[pin] = state;
        return true;
    }

    PinState digitalRead(int pin) override {
        auto it = pinStates_.find(pin);
        return it != pinStates_.end() ? it->second : PinState::Low;
    }

    int analogRead(int /*pin*/) override { return 0; }
    bool analogWrite(int /*pin*/, int /*value*/) override { return true; }

    int uartOpen(const std::string& port, int baud) override {
        // On generic platform, try to open the serial port
        (void)port; (void)baud;
        return -1;  // Not available on generic platform
    }

    bool uartWrite(int /*fd*/, const std::string& /*data*/) override { return false; }
    std::string uartRead(int /*fd*/, int /*bytes*/) override { return ""; }
    void uartClose(int /*fd*/) override {}

    Platform platform() const override { return Platform::Generic; }
    std::string platformName() const override { return "Generic"; }

private:
    std::unordered_map<int, PinMode>  pinModes_;
    std::unordered_map<int, PinState> pinStates_;
};

// ═══════════════════════════════════════════════════════════
// 注册 hw.* 标准库模块
// ═══════════════════════════════════════════════════════════
inline std::unordered_map<std::string, StdlibFn> makeHwGpioModule(
    std::shared_ptr<HardwareDriver> driver) {
    return {
        {"pinMode", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.gpio.pinMode(pin, mode)");
            int pin = (int)args[0].number;
            std::string mode = args[1].toString();
            PinMode pm = PinMode::Output;
            if (mode == "input") pm = PinMode::Input;
            else if (mode == "output") pm = PinMode::Output;
            else if (mode == "input_pullup") pm = PinMode::InputPullup;
            else if (mode == "input_pulldown") pm = PinMode::InputPulldown;
            else if (mode == "pwm") pm = PinMode::PWM;
            return Value::Bool(driver->pinMode(pin, pm));
        }},
        {"write", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.gpio.write(pin, value)");
            int pin = (int)args[0].number;
            PinState state = args[1].isTruthy() ? PinState::High : PinState::Low;
            return Value::Bool(driver->digitalWrite(pin, state));
        }},
        {"read", [driver](std::vector<Value> args) -> Value {
            if (args.empty()) throw std::runtime_error("hw.gpio.read(pin)");
            int pin = (int)args[0].number;
            return Value::Num(driver->digitalRead(pin) == PinState::High ? 1.0 : 0.0);
        }},
        {"analogRead", [driver](std::vector<Value> args) -> Value {
            if (args.empty()) throw std::runtime_error("hw.gpio.analogRead(pin)");
            return Value::Num((double)driver->analogRead((int)args[0].number));
        }},
        {"analogWrite", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.gpio.analogWrite(pin, value)");
            return Value::Bool(driver->analogWrite((int)args[0].number, (int)args[1].number));
        }},
    };
}

inline std::unordered_map<std::string, StdlibFn> makeHwUartModule(
    std::shared_ptr<HardwareDriver> driver) {
    return {
        {"open", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.uart.open(port, baud)");
            int fd = driver->uartOpen(args[0].toString(), (int)args[1].number);
            return Value::Num((double)fd);
        }},
        {"write", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.uart.write(fd, data)");
            return Value::Bool(driver->uartWrite((int)args[0].number, args[1].toString()));
        }},
        {"read", [driver](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("hw.uart.read(fd, bytes)");
            return Value::Str(driver->uartRead((int)args[0].number, (int)args[1].number));
        }},
        {"close", [driver](std::vector<Value> args) -> Value {
            if (args.empty()) throw std::runtime_error("hw.uart.close(fd)");
            driver->uartClose((int)args[0].number);
            return Value::Nil();
        }},
    };
}

inline std::unordered_map<std::string, StdlibFn> makeHwPlatformModule(
    std::shared_ptr<HardwareDriver> driver) {
    return {
        {"name", [driver](std::vector<Value>) -> Value {
            return Value::Str(driver->platformName());
        }},
        {"isGeneric", [driver](std::vector<Value>) -> Value {
            return Value::Bool(driver->platform() == Platform::Generic);
        }},
    };
}

// Build the merged "hw" module map
// Called from registerStdlib() in stdlib.cpp
inline std::unordered_map<std::string, StdlibFn> makeHwModule(
    std::shared_ptr<HardwareDriver> driver = nullptr) {
    if (!driver) driver = std::make_shared<GenericDriver>();

    auto gpioMod = makeHwGpioModule(driver);
    auto uartMod = makeHwUartModule(driver);
    auto platMod = makeHwPlatformModule(driver);

    // Merge into a single "hw" module with prefixed names
    std::unordered_map<std::string, StdlibFn> hwMod;
    for (auto& [k, v] : gpioMod)  hwMod["gpio_" + k] = v;
    for (auto& [k, v] : uartMod)  hwMod["uart_" + k] = v;
    for (auto& [k, v] : platMod)  hwMod["platform_" + k] = v;

    // Also add flat GPIO access (most common)
    for (auto& [k, v] : gpioMod) hwMod[k] = v;

    return hwMod;
}
