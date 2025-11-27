The Linux kernel supports two modes for PSCI (Power State Coordination Interface): **PC (Platform Coordinated)** and **OSI (OS Initiated)**. In PC mode, the OS requests idle states for individual CPUs, and the platform firmware coordinates when to power down shared resources (like clusters). In OSI mode, the OS explicitly tells the firmware when to power down shared domains (clusters, systems) via the "Last Man Standing" algorithm implemented in the OS.

Here is the deep dive into how this works in the Linux kernel.

### **Relevant Files**

| File Path | Role |
| :--- | :--- |
| `drivers/firmware/psci/psci.c` | Core PSCI driver. Handles feature detection (`psci_has_osi_support`), switching modes (`psci_set_osi_mode`), and issuing SMC/HVC calls. |
| `drivers/cpuidle/cpuidle-psci-domain.c` | Manages PM Domains (GenPD) for PSCI. Responsible for the "coordination" logic by tracking CPU usage in a domain and calculating the composite state. |
| `drivers/cpuidle/cpuidle-psci.c` | The CPUIdle driver. Hooked into the Linux generic power management (PM) runtime to trigger domain checks upon idle entry. |
| `drivers/cpuidle/cpuidle-psci.h` | Header sharing `psci_set_domain_state` between the cpuidle driver and the domain manager. |

---

### **Code Deep Dive: The OSI Coordination Flow**

#### **1. Detection & Initialization**
The kernel first detects if the firmware supports OSI.
*   **File**: `drivers/firmware/psci/psci.c`
*   **Function**: `psci_init_cpu_suspend` checks `PSCI_1_0_OS_INITIATED` bit.
*   **Function**: `psci_has_osi_support()` returns this boolean status.

If supported, the domain topology driver initializes it:
*   **File**: `drivers/cpuidle/cpuidle-psci-domain.c`
*   **Function**: `psci_cpuidle_domain_probe`
    1.  Calls `psci_has_osi_support()`.
    2.  Initializes generic PM domains (GenPD) representing clusters.
    3.  Sets `pd->power_off = psci_pd_power_off`. **(Crucial: This is the callback that runs when the domain is empty)**.
    4.  Calls `psci_set_osi_mode(true)` to switch firmware to OSI mode.

#### **2. Runtime Idle Entry (The Trigger)**
When a CPU goes idle, it enters the `psci_idle` driver.
*   **File**: `drivers/cpuidle/cpuidle-psci.c`
*   **Function**: `psci_enter_domain_idle_state`
    *   This is the specific entry function used when OSI is active.
    *   It calls `pm_runtime_put_sync_suspend(pd_dev)`. This tells the Linux Generic Power Domain subsystem: "This CPU device is suspending."

#### **3. Coordination (Last Man Standing)**
This is where the coordination happens. Linux GenPD tracks the devices (CPUs) in the domain.

**Scenario: CPU 0 is already idle. CPU 1 (the last CPU) is now entering idle.**

1.  **CPU 0** previously called `pm_runtime_put_sync_suspend`. GenPD marked it suspended.
2.  **CPU 1** calls `pm_runtime_put_sync_suspend` (via `psci_enter_domain_idle_state`).
3.  **GenPD** (`drivers/base/power/domain.c`) sees that *all* devices in this domain are now suspended.
4.  **GenPD** determines the domain can be powered off and calls the registered callback: `psci_pd_power_off`.

#### **4. Setting the Composite State**
*   **File**: `drivers/cpuidle/cpuidle-psci-domain.c`
*   **Function**: `psci_pd_power_off`
    *   This runs on the current CPU (CPU 1) inside the `pm_runtime` call.
    *   It selects the appropriate domain state (e.g., "Cluster Power Down").
    *   It calls `psci_set_domain_state(*pd_state)`.

*   **File**: `drivers/cpuidle/cpuidle-psci.c`
*   **Function**: `psci_set_domain_state`
    *   This saves the calculated composite state ID into a per-cpu variable: `__this_cpu_write(domain_state, state)`.

#### **5. Execution (The PSCI Call)**
Control returns to `psci_enter_domain_idle_state` on CPU 1.
*   **File**: `drivers/cpuidle/cpuidle-psci.c`
*   **Function**: `psci_enter_domain_idle_state`
    1.  Retrieves the state: `state = psci_get_domain_state()`.
        *   **CPU 0** (First man): `domain_state` was 0 (default). It sends a simple CPU-level suspend request.
        *   **CPU 1** (Last man): `domain_state` was updated by `psci_pd_power_off` to include the Cluster-level bits.
    2.  Calls `psci_cpu_suspend_enter(state)`.

*   **File**: `drivers/firmware/psci/psci.c`
*   **Function**: `psci_cpu_suspend_enter` -> `invoke_psci_fn`.
    *   The firmware receives the command. Since it is in OSI mode, it trusts the state ID provided by the OS. For CPU 1, this ID requests "Power Down CPU + Power Down Cluster".

---

### **Trace: 2 CPUs entering Cluster Off**

To trace this coordination, follow this sequence of events:

| Step | CPU | Function | File | Description |
| :--- | :--- | :--- | :--- | :--- |
| 1 | CPU 0 | `psci_enter_domain_idle_state` | `cpuidle-psci.c` | Enters idle loop. |
| 2 | CPU 0 | `pm_runtime_put_sync_suspend` | `cpuidle-psci.c` | Notifies GenPD. |
| 3 | CPU 0 | GenPD Logic | `base/power/domain.c` | Checks domain. CPU 1 is still active. **Do not power off domain.** |
| 4 | CPU 0 | `psci_cpu_suspend_enter` | `psci.c` | Enters CPU-only suspend (PC=0, OSI=1, Domain=Off). |
| | | | | |
| 5 | CPU 1 | `psci_enter_domain_idle_state` | `cpuidle-psci.c` | Enters idle loop. |
| 6 | CPU 1 | `pm_runtime_put_sync_suspend` | `cpuidle-psci.c` | Notifies GenPD. |
| 7 | CPU 1 | GenPD Logic | `base/power/domain.c` | Checks domain. All CPUs suspended. **Power off domain.** |
| 8 | CPU 1 | `psci_pd_power_off` | `cpuidle-psci-domain.c` | **Coordinator**: Calculates cluster state ID. |
| 9 | CPU 1 | `psci_set_domain_state` | `cpuidle-psci.c` | Writes composite state to per-cpu `domain_state`. |
| 10 | CPU 1 | `psci_enter_domain_idle_state` | `cpuidle-psci.c` | Reads `domain_state` (now contains Cluster Off bits). |
| 11 | CPU 1 | `psci_cpu_suspend_enter` | `psci.c` | Calls PSCI firmware with Cluster Off request. |
