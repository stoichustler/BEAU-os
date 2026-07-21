# BEAU OS porting guide

- [ ] RAS
- [ ] VHE


## RAS

- `Reliability`: Continuity, Computation needs be correct and
reliable.

- `Availability`: Readiness, System needs to remain available as
long as possible.

- `Serviceability`: Ability to undergo modifications and repairs,
System should provide information to administrator to aid in
system servicing.

1. **ESB**

Synchronization barrier instruction, used to synchronize Unrecoverable
errors, to make sure containable errors architecturally consumed by
the PE and not silently propagated.

2. **Registers Group**

- Processor Feature
- Trapping accesses to Error system
- Error records (Single/Group)
- Deferred Interrupt Status(work with ESB)

3. **POISON**

signals corrupted data (hardware mechanism)

## VHE

(TODO)

## AAOS bringup

- Refer to [AAOS.md](../bsp/boot/guest/AAOS.md) for more infos on Android bringup.


---

Hustle Embedded OS.
