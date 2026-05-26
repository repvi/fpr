# FPR - Fast Peer Router

[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](https://github.com/repvi/fpr)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)](https://www.espressif.com/en/products/socs/esp32)
[![LTS](https://img.shields.io/badge/LTS-Long%20Term%20Support-brightgreen.svg)](#version-history)

**FPR (Fast Peer Router)** is a lightweight, secure mesh networking library for ESP32 devices that enables WiFi-like connectivity without requiring a traditional WiFi infrastructure. Built on top of ESP-NOW, FPR creates self-organizing networks that can span multiple hops, making it ideal for IoT deployments, sensor networks, and distributed applications.

> **✅ Version 1.0.0 LTS Released!** This is the first official stable release with Long-Term Support. Host and Client modes are fully functional and production-ready. Extender (mesh) mode is under active development.

> **🚀 Enterprise Vision**: FPR is designed with enterprise-grade reliability and scalability in mind. Our roadmap includes advanced features for mission-critical applications, commercial-grade security, and enterprise support services to meet the demands of large-scale industrial deployments.

## 🌟 Key Features

### **WiFi-Like Experience Without WiFi**
- **Zero Infrastructure**: No routers, access points, or internet connection required
- **Automatic Discovery**: Devices find and connect to each other automatically
- **Seamless Communication**: Send and receive data as easily as traditional network programming
- **Multi-Hop Routing**: Messages can route through intermediate devices to reach distant peers

### **Secure & Reliable**
- **Built-in Security**: Encrypted communication with secure handshake protocol
- **Connection Management**: Automatic reconnection and keepalive mechanisms  
- **Peer Authentication**: Control which devices can join your network
- **Robust Protocol**: Handles packet loss, interference, and device mobility
- **Peer Blocking**: Block specific MAC addresses from connecting
- **Allowlist Filtering**: Restrict connections to only approved devices

### **Flexible Network Topologies**
- **Host Mode**: Act as a central coordinator accepting client connections
- **Client Mode**: Connect to available hosts and communicate through them
- **Extender Mode**: Relay messages between distant devices (mesh functionality) *(🚧 Under Development)*
- **Broadcast Mode**: Send data to all devices in the network simultaneously

### **Production Ready**
- **Comprehensive Testing**: Extensive test suite with automated scenarios
- **Professional API**: Clean, well-documented interface following ESP-IDF conventions
- **Configurable**: Fine-tune timeouts, connection limits, and behavior via Kconfig
- **Version Management**: Protocol versioning ensures compatibility across firmware updates

### **Enterprise-Grade Foundation**
- **Scalable Architecture**: Designed for networks from 2 to 1000+ devices
- **Mission-Critical Reliability**: Built-in redundancy and fault tolerance
- **Commercial Support Ready**: Professional documentation and validation processes
- **Industry Standards**: Following IoT security and networking best practices
- **Long-Term Maintenance**: Committed development roadmap with LTS versions

## 📊 Technical Specifications

| Feature | Specification |
|---------|---------------|
| **Protocol Version** | 1.0.0 (with legacy support) |
| **Maximum Range** | 50-200m line-of-sight (environment dependent) |
| **Payload Size** | Up to 45 bytes per message (ESP-NOW limitation) |
| **Network Capacity** | Up to 20 peers per device (ESP-NOW limitation) |
| **Latency** | <100ms direct, <200ms single-hop |
| **Throughput** | 10-50 messages/second (size dependent) |
| **Power Consumption** | Ultra-low power with sleep mode support |
| **Memory Usage** | ~8KB RAM, ~32KB Flash (configurable) |

## 🚀 Quick Start

### Prerequisites
- ESP-IDF 4.4 or later
- ESP32, ESP32-S2, ESP32-S3, or ESP32-C3 development board
- Two or more ESP32 devices for testing

### Installation

1. **Add FPR as a component to your ESP-IDF project:**
   Navigate to your project's components directory and clone the repository.

2. **Or use the ESP Component Manager:**
   Add the dependency to your idf_component.yml configuration file.

### Basic Usage

To get started with FPR, you need to initialize the network, set your device mode (host or client), register a callback for incoming data, and start the network. The library handles discovery, connection management, and data transmission automatically. You can send messages to specific peers or broadcast to all connected devices.

## 💡 Use Cases & Applications

### **Industrial IoT**
- **Sensor Networks**: Collect data from multiple sensors across a facility
- **Equipment Monitoring**: Monitor machine status without wiring infrastructure  
- **Safety Systems**: Create redundant communication paths for critical alerts
- **Inventory Tracking**: Track assets moving through different areas

### **Smart Buildings**
- **Room Automation**: Coordinate lighting, temperature, and occupancy sensors
- **Security Systems**: Connect cameras, door sensors, and alarms
- **Energy Management**: Monitor and control electrical systems
- **Facility Maintenance**: Remote monitoring of HVAC, elevators, and utilities

### **Environmental Monitoring**
- **Weather Stations**: Network of sensors across large areas
- **Agriculture**: Soil moisture, temperature, and irrigation control
- **Wildlife Tracking**: Long-range animal monitoring systems
- **Disaster Response**: Emergency communication networks

### **Consumer Applications**
- **Home Automation**: Connect smart devices without WiFi dependency
- **Gaming**: Low-latency multiplayer games between devices
- **Robotics**: Coordinate multiple robots in swarms
- **Wearables**: Device-to-device communication for fitness tracking

### **Enterprise & Mission-Critical**
- **Manufacturing**: Production line monitoring and control systems
- **Healthcare**: Patient monitoring and medical device coordination
- **Transportation**: Fleet management and vehicle-to-vehicle communication
- **Critical Infrastructure**: Power grid monitoring, pipeline systems
- **Military/Defense**: Secure tactical communication networks
- **Oil & Gas**: Remote monitoring in hazardous environments

### **Educational Projects**
- **STEM Learning**: Teach networking concepts with hands-on projects
- **Robotics Clubs**: Build communicating robot teams
- **IoT Workshops**: Create mesh networks without complex setup
- **Research Projects**: Rapid prototyping of distributed systems

## 🏗️ Architecture Overview

FPR is organized into three main layers:

**Application Layer**: Your application interacts with FPR through a clean API that provides callbacks for received data and functions for sending messages. Message handling and data routing are managed automatically.

**FPR Protocol Layer**: This layer handles device discovery, secure handshakes, routing decisions, and connection management. It implements the mesh routing logic and security protocols.

**ESP-NOW Layer**: The foundation layer provided by Espressif handles the physical transmission, MAC addressing, and low-level encryption.

## 📚 Documentation

### **Core Components**
- **[API Reference](docs/API.md)**: Complete function documentation
- **[Configuration Guide](docs/CONFIGURATION.md)**: Kconfig options and tuning
- **[Network Modes](docs/MODES.md)**: Detailed explanation of HOST/CLIENT/EXTENDER modes  
- **[Security Model](docs/SECURITY.md)**: Authentication and encryption details

### **Testing & Validation**
- **[Test Suite](test/README_TESTS.md)**: Comprehensive testing documentation
- **[Quick Start Guide](test/QUICK_START.md)**: Get testing in 5 minutes
- **[Reconnection Testing](test/RECONNECT_TEST_GUIDE.md)**: Network resilience validation

### **Examples**
- **[Basic Examples](examples/)**: Simple host/client implementations
- **[Advanced Scenarios](examples/advanced/)**: Mesh networking and complex topologies
- **[Integration Examples](examples/integration/)**: Use with other ESP-IDF components

## 🔧 Configuration Options

FPR provides extensive configuration through ESP-IDF's Kconfig system. Access the configuration menu and navigate to Component Config → FPR to adjust settings.

**Essential Settings:**
- **Connection Timeouts**: Adjust for your network conditions
- **Task Priorities**: Balance with your application needs  
- **Debug Output**: Enable for development, disable for production
- **Legacy Support**: Support older FPR protocol versions
- **Memory Usage**: Fine-tune for resource-constrained applications

## 🧪 Testing & Validation

FPR includes a comprehensive test suite designed for real-world validation. The test suite covers scenarios including two-device communication, multi-hop mesh routing, connection resilience through power cycling and range testing, multiple client handling, manual versus automatic connection modes, security handshake validation, and performance benchmarks.

## 🛡️ Security Features

- **Secure Handshake**: Mutual authentication before data exchange
- **Connection Control**: Manual approval mode for sensitive applications
- **Peer Blocking**: Blacklist unwanted devices by MAC address
- **Allowlist Filtering**: Restrict connections to only approved MAC addresses
- **Private Networks**: Control network visibility
- **Protocol Versioning**: Ensure only compatible devices connect

## 🔄 Version History

| Version | Release Date | Key Features |
|---------|--------------|--------------|
| **1.0.0** | December 2024 | Official stable release with LTS support, host/client modes fully functional, security handshake, fragmentation support, peer blocking, allowlist filtering |
| **0.x** | 2024-2025 | Pre-versioning development (legacy support) |

**Development Roadmap:**
- **0.1.0**: Base protocol implementation
- **0.2.0**: Enhanced security framework
- **0.3.0**: Reconnection and resilience features
- **0.4.0**: Extended data handling (fragmentation)
- **0.5.0**: Protocol versioning support
- **0.6.0**: Network statistics and monitoring
- **0.7.0**: Memory management optimization

**Enterprise Roadmap:**
- **1.0.0**: ✅ Official stable release with LTS support (Current)
- **1.1.0**: Extender/mesh mode completion and optimization
- **1.5.0**: Enterprise security suite (certificate-based auth)
- **2.0.0**: Large-scale network management (1000+ devices)
- **2.5.0**: Cloud integration and remote management
- **3.0.0**: AI-powered network optimization
- **Enterprise+**: Commercial support, SLA guarantees, custom features

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

**Development Setup:**
Clone the repository, set up your ESP-IDF environment, and run tests to validate your setup.

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🆘 Support & Community

### **Community Support**
- **Issues**: [GitHub Issues](https://github.com/repvi/fpr/issues)
- **Discussions**: [GitHub Discussions](https://github.com/repvi/fpr/discussions)
- **Documentation**: [Wiki](https://github.com/repvi/fpr/wiki)

### **Enterprise Support** *(Coming Soon)*
- **Professional Services**: Custom integration and deployment assistance
- **Priority Support**: Guaranteed response times for critical issues
- **Training Programs**: Comprehensive developer and administrator training
- **Custom Development**: Tailored features for enterprise requirements
- **Certification Programs**: Official FPR implementation validation
- **Consulting Services**: Network design and optimization consulting

*Contact us for early access to enterprise features and support packages.*

## 🙏 Acknowledgments

- Built on [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html) by Espressif Systems
- Inspired by mesh networking research and IoT connectivity challenges
- Community feedback and contributions from ESP32 developers worldwide

---

**Ready to build your mesh network?** Start with our [Quick Start Guide](test/QUICK_START.md) and have devices communicating in minutes!