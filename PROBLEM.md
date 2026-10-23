# Problem memo -- OpenChair (Kacper Mazur, Alan De Lira)

## The user
Powered (motorized) wheelchair users who experience controller faults, particularly those whose controllers report faults through diagnostic LED blink patterns, along with the caregivers and technicians who help them.

## The problem
Wheelchairs are vehicles of freedom, independence, and life for those with mobility issues. Powered wheelchairs, especially when used around the clock and in various conditions, are prone to faults. Some controllers report a fault as a flashing diagnostic LED, and the meaning of the flash pattern may be buried in a manual. OpenChair reads the existing diagnostic LED flash sequence and translates verified, controller-specific codes into a short, understandable explanation. If the pattern is unclear or unrecognized, it says so and offers retry or manual code entry instead of guessing.

## Why a device
OpenChair does not plug into the wheelchair. It reads the LED with non-contact optical sensing and uses a proximity sensor to confirm it is positioned correctly. A dedicated sensing device is intended to provide repeatable measurements through controlled sensor geometry, a defined sampling rate, and a placement check before readings are accepted. It is designed to work fully offline, with no network, cloud service, or phone required. A rechargeable internal battery is a design goal, not yet an implemented feature; development and soak testing use an external supply.

## The sensors
Detailed sensor design will be documented in DESIGN.md.

Sensor A:
- ALS-PT19 optical sensor
- Purpose: capture wheelchair diagnostic LED brightness over time
- Analog output connects to channel A0 of an ADS1115 16-bit I²C ADC (default address 0x48)

Sensor B:
- VL6180X time-of-flight proximity/distance sensor
- I²C address 0x29
- Purpose: verify sensor placement and detect movement during capture

Sensor cooperation:
- Optical readings are not accepted independently
- Distance/stability measurements participate in capture validation
- The optical reading must pass distance/position validation before automatic diagnosis is accepted
- Unstable or invalid positioning causes the optical capture to be rejected or retried
- The final diagnostic decision therefore depends on both sensors

## The mechanisms
Mechanisms E and D are the provisional selection of the architecture lead. Final confirmation and detailed mechanism mapping will occur in DESIGN.md.

Mechanism E — Multi-process architecture:
- Separate sensor acquisition and analysis into processes communicating through explicit IPC.
- A supervisor detects process failures, reports degraded operation, and restarts failed processes where appropriate.
- Fault isolation is important because a sensor or software component failure should not bring down the entire diagnostic reader.

Mechanism D — Custom storage layer:
- Implement a custom append-only event log and/or ring buffer for sensor events, diagnostic results, and fault records.
- Define write batching, fsync behavior, and recovery from interrupted writes or power loss.
- Reliable local storage supports offline operation, diagnostic history, and recovery during unattended use.

If confirmed, both mechanisms will be implemented and quantitatively evaluated as required by the CS370 specification.

## The risk
The primary technical risk is reliably detecting a diagnostic LED flash sequence under variable LED brightness, sensor positioning, ambient lighting, and flash timing. The software must reject an uncertain reading rather than force a diagnosis. Controller compatibility is a limitation: OpenChair cannot support every wheelchair brand, so we will explicitly define supported controller models using only verified documentation.
