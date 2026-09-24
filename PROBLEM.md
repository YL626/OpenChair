# Problem memo -- <OpenChair> (<Kacper Mazur>, <Alan De Lira>)

## The user
Anyone who uses a wheelchair.

## The problem
Wheelchairs are vehicles of freedom, independence, and life for those with mobility issues. These mechanical and electrical vehicles, especially when being used around the clock and in various conditions, are prone to break. Our device would assist in the process of repair by producing repair codes, offering repair instructions, or references to repair technicians near the user.

## Why a device
What our device does that a phone can't is plug directly into the wheelchair. It also comes with a seperate power source so you don't have to rely on your phone's connection or battery to operate the device.

## The sensors
More can be read in DESIGN.md:
Sensor A:
- ALS-PT19 optical sensor
- purpose: capture wheelchair diagnostic LED brightness over time
- analog output passes through an external ADC such as MCP3008

Sensor B:
- VL6180X time-of-flight proximity/distance sensor
- purpose: verify sensor placement and detect movement during capture

Sensor cooperation:
- optical readings are not accepted independently
- distance/stability measurements participate in capture validation
- unstable or invalid positioning causes the optical capture to be rejected or retried
- the final diagnostic decision therefore depends on both sensors

## The mechanisms
Mechanism A:
- Interrupt-driven input, with a head-to-head comparison against a polling design. One of our sensor detects diagnostic LEDs, which we need to measure to offer repair services

Mechanism B:
- A custom storage layer: ring buffer, append-only log, or small FUSE filesystem, with an explicit crash consistency story. A file system for repair instructions that can be accessed offline is crucial to our services.

## The risk
The biggest risk our device faces is compatability with every wheelchair brand or device. There might be one out there that we simply can't make work.
