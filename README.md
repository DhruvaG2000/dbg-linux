# Using PM domain hierarchy for TI LPM on AM62L

This document aims to sumamrize the WIP development for enabling PM
domain (PD) hierarchy for TI LPM usecases, with a focus on the AM62L SoC.


## Hierarchy on AM62L

At a high level, the domain hierarchy consists of CPUs, which are
grouped into cluster(s), and then clusters may further be part of
top-level PDs.  In the case of AM62L, there's a single CPU cluster,
which is part of a top-level domain called MAIN.

This hierarchy is described in k3-am62l3.dtsi.  Here you will find
that CPU nodes have a power-domains property,  for example, the cpu0 node 
has `power-domains = <&CPU_PD0>`.

### CPUs

Looking closer at `CPU_PD0`, there is a `domain-idle-states` property.
If a PD has multiple different idle states, this propery is used to
describe those.  For this example, `CPU_PD0` has two domain-idle-states:
`CPU_SLEEP_0` and `CPU_SLEEP_1`.  When the idle state has `compatible
= "arm,idle-state"`, this means that the states will be used by the
CPUidle driver for PSCI to select between the states based on
comparing the expected idle time of the kernel vs the entry/exit
latencies and minimum residency times of the idle states.

### Clusters

Further, `CPU_PD0` itself has a power-domains property `power-domains =
<&CLUSTER_PD>` which describes that `CPU_PD0` is a subdomain of
`CLUSTER_PD`.

Similar to the CPUs, the cluster PD also has a list of
`domain-idle-states`.  However, notice that unlike the CPU idle
states, these states have `compatible = "domain-idle-state".  This
indicates that these are handled by the pmdomain idle state code, not
by CPUidle.

### Top-level domains

The next level up the hierarchy, `CLUSTER_PD` also has a power-domains
property: `power-domains = <&MAIN_PD>` indicating that the cluster is
a subdomain of the top-level domain MAIN.a

Similar to CLUSTER_PD, MAIN_PD has multiple domain-idle-states.

## Idle state statistics

In order to see when the various idle states, at all levels, are
entered, that information is vailable under various `sysfs` locations:

### CPU idle states
```
# (cd /sys/devices/system/cpu/cpu0; cat cpuidle/state?/name; cat cpuidle/state
?/usage)
WFI
STBY
DEEP
203
171
43
```

### Cluster idle states

For `CLUSTER_PD`

```
# (cd /sys/kernel/debug/pm_genpd; cat power-controller-cluster/idle_states)
State          Time Spent(ms) Usage          Rejected
S0             110            44337          0
S1             72             28943          0
```

### Top level domains

For `MAIN`

```
 # (cd /sys/kernel/debug/pm_genpd; cat power-controller-main/idle_states)
State          Time Spent(ms) Usage          Rejected
S0             0              0              0
S1             0              0              0
```

Here, we can see taht none of the domain-idle-states for MAIN have
been entered.  This indicates that all of the subdomains of MAIN have
not yet been off, so MAIN itself has not had the opportunity to go off.

However, doing a system-wide suspend will shutdown all the subdomains,
so that after a suspend we should see that one of MAIN's
domain-idle-states has been hit once:

```
~ # (cd /sys/kernel/debug/pm_genpd; cat power-controller-main/idle_states)
State          Time Spent(ms) Usage          Rejected
S0             0              0              0
S1             0              1              0
```

## PSCI

From a Linux PoV, all of these idle states, whether they are
`"arm,idle-state"` or `"domain-idle-state"`, they are all PSCI states.

This means that the implementation details of each of these states is
handled by the PSCI code in TF-A.

It also means that the transition into an idle state doesn't actually
happen until PSCI is called when CPU(s) or cluster(s) go idle.

The PSCI firmware is able to determine which state Linux would like to
enter by the `arm,psci-suspend-param` property in each idle state
definition.

This value will be passed by Linux to PSCI firmware, where the
firmware is then reponsible to implement the specific way to enter &
exit that state.

The various bits of this value are defined in the PSCI
spec and can be used to distinguish between various types of CPU,
cluster or system-wide states, as well as states where there is memory
retention or power off.

## Suspend-to-idle (s2idle)

The current state of s2idle is that when entered, the deepest CPU idle
state is selected as the 

### WIP: s2idle governor

In order to explore the basic idea of adding an "governor" for s2idle
state selection, we chose to implement a basic governor which simply
choses the state based on history.  It uses historical suspend
duration data to select power domain states during system suspend.

This simple/basic approach was used to explore the various ways that
governors might be implemented in the pmdomain & s2idle core code
before upstreaming.

#### Next steps

For upstreaming, we believe the proper way of adding a governor will
be to use the existing PM QoS framework and select the
domain-idle-state based on any constraints set in all devices or
subdomains that are part of the PD.
