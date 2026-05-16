# LoRaHAM daemon refactor workspace

This directory contains a copied daemon source and ex-ante tests for the
planned refactor. The upstream daemon source is not modified directly.

The target platform is Raspberry Pi / Raspberry Pi OS with LoRaHAM_Pi or
LoRaHAM Cartridge hardware.

## Build and test

Run the default non-transmitting test set:

```bash
./loradaemon_refactor/run_tests.sh
```

Run optional RF transmit smoke tests:

```bash
./loradaemon_refactor/run_tests.sh --TX --rx-seconds 5
```

`--TX` sends real RF packets. Use it only on suitable test hardware and
frequencies.

## Test groups

- `test_interface_baseline`: current public socket/config/RSSI behavior
- `test_config_stream`: persistent config sockets and RSSI stream behavior
- `test_client_lifecycle`: client connect/disconnect behavior
- `test_known_issues`: desired future behavior, currently XFAIL

The test runner prints a final summary at the end of the log.

## Refactor rule

Keep the public socket interface stable unless a change is intentional,
documented, and covered by tests.

Functional changes should be called out in short comments near the changed
code.

## Planned direction

Refactor in small steps:

1. Extract transport/client helpers.
2. Extract config parsing.
3. Introduce a `RadioChannel` abstraction for 433/868 MHz.
4. Move timing into explicit timer state.
5. Introduce an event-loop abstraction.
6. Switch the event-loop implementation to `epoll`.
7. Convert known-issue XFAIL tests into normal passing tests one by one.

Avoid changing radio behavior during structural refactors unless the change is
tested and explicitly documented.
