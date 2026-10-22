# popcorn

popcorn runs STM32 Cortex-M firmware on an emulated core so it can be unit tested from Python.
A test loads the firmware ELF, boots it with emulated peripherals, drives inputs and asserts on
outputs, memory, registers and peripheral state.

```mermaid
flowchart LR
    D[MachineDescription] --> M[Machine]
    M -->|run| S[Stop]
    S -->|assert| T[pytest]
```

## A first test

The test below boots the gpio example firmware to `main`, runs it for 1 ms and checks the LED pin.

```python
--8<-- "test_quickstart.py:boot"
```

Run it with pytest:

=== "uv"

    ```sh
    uv run pytest tests/docs/test_quickstart.py
    ```

=== "pip"

    ```sh
    python -m pytest tests/docs/test_quickstart.py
    ```

!!! note
    This page is a placeholder. Step 04 writes the guide it points to.
